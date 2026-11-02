#include "msc_host.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diskio_impl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "blob_store.h"
#include "busid_owner.h"
#include "device_rescue.h"
#include "rescue_logic.h"
#include "serial_link.h"
#include "serial_monitor.h"
#include "usb_backend.h"

static const char *TAG = "msc";

#define MSC_XFER_MAX        4096    /* CONFIG_USBIP_MAX_TRANSFER: one bulk transfer */
#define MSC_CMD_SECTORS_MAX 64
#define MSC_TIMEOUT_MS      5000
#define MSC_READY_WAIT_MS   4000
#define MSC_PATH_MAX        256
#define MSC_COPY_CHUNK      4096

typedef struct {
    bool open;
    char busid[32];
    uint32_t devnum;
    uint8_t iface, ep_in, ep_out, lun;
    uint32_t tag;
    uint32_t block_size, block_count;
    char vendor[9], product[17], revision[5];
    int last_err;               /* last transport error (-ENODEV when the device left) */
} msc_dev_t;

static msc_dev_t s_dev;
static BYTE s_pdrv = 0xFF;
static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_init_lock = portMUX_INITIALIZER_UNLOCKED;

static void lock(void)
{
    if (s_mutex == NULL) {
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        taskENTER_CRITICAL(&s_init_lock);
        if (s_mutex == NULL) {
            s_mutex = m;
            m = NULL;
        }
        taskEXIT_CRITICAL(&s_init_lock);
        if (m != NULL) {
            vSemaphoreDelete(m);
        }
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mutex);
}

static int fail(cJSON **out, int status, const char *msg)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", false);
    cJSON_AddStringToObject(j, "error", msg);
    *out = j;
    return status;
}

/* ===========================================================================
 *  Bulk-Only Transport
 * ======================================================================== */

static int clear_halt(msc_dev_t *d, uint8_t ep)
{
    usb_setup_packet_t setup = { .bmRequestType = 0x02, .bRequest = 0x01, .wValue = 0, .wIndex = ep, .wLength = 0 };
    return usb_backend_transfer_sync(d->busid, 0, &setup, NULL, 0, NULL, 0, 1000, NULL, NULL);
}

static void reset_recovery(msc_dev_t *d)
{
    usb_setup_packet_t setup = { .bmRequestType = 0x21, .bRequest = 0xFF, .wValue = 0, .wIndex = d->iface, .wLength = 0 };
    usb_backend_transfer_sync(d->busid, 0, &setup, NULL, 0, NULL, 0, 1000, NULL, NULL);
    clear_halt(d, d->ep_in);
    clear_halt(d, d->ep_out);
}

/* One SCSI command.  Returns 0, 1 (the command failed: check sense), or a
   negative errno (transport). */
static int bot(msc_dev_t *d, const uint8_t *cdb, uint8_t cdb_len, uint8_t *data, uint32_t len, bool in)
{
    uint8_t cbw[31];
    const uint32_t tag = ++d->tag;
    rescue_msc_cbw(tag, len, in, d->lun, cdb, cdb_len, cbw);
    int rc = usb_backend_transfer_sync(d->busid, d->ep_out, NULL, cbw, sizeof(cbw), NULL, 0, MSC_TIMEOUT_MS, NULL, NULL);
    if (rc != 0) {
        d->last_err = rc;
        if (rc != -ENODEV) {
            reset_recovery(d);
        }
        return rc;
    }
    uint32_t done = 0;
    while (done < len) {
        const uint32_t n = (len - done) > MSC_XFER_MAX ? MSC_XFER_MAX : len - done;
        size_t got = 0;
        rc = in ? usb_backend_transfer_sync(d->busid, d->ep_in, NULL, NULL, 0, data + done, n, MSC_TIMEOUT_MS, &got, NULL)
                : usb_backend_transfer_sync(d->busid, d->ep_out, NULL, data + done, n, NULL, 0, MSC_TIMEOUT_MS, NULL, NULL);
        if (rc == -EPIPE) {
            clear_halt(d, in ? d->ep_in : d->ep_out);   /* the device ended the data phase: read the CSW */
            break;
        }
        if (rc != 0) {
            d->last_err = rc;
            if (rc != -ENODEV) {
                reset_recovery(d);
            }
            return rc;
        }
        done += in ? (uint32_t)got : n;
        if (in && got < n) {
            break;                                      /* short packet: end of data */
        }
    }
    uint8_t csw[64];
    size_t got = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        rc = usb_backend_transfer_sync(d->busid, d->ep_in, NULL, NULL, 0, csw, sizeof(csw), MSC_TIMEOUT_MS, &got, NULL);
        if (rc != -EPIPE) {
            break;
        }
        clear_halt(d, d->ep_in);
    }
    uint32_t residue = 0;
    uint8_t status = 0;
    if (rc != 0 || !rescue_msc_csw(csw, got, tag, &residue, &status) || status == 2) {
        d->last_err = rc ? rc : -EIO;
        if (rc != -ENODEV) {
            reset_recovery(d);
        }
        return rc ? rc : -EIO;
    }
    return status ? 1 : 0;
}

static int scsi_simple(msc_dev_t *d, uint8_t op, uint8_t alloc, uint8_t *data, bool in)
{
    uint8_t cdb[6] = { op, 0, 0, 0, alloc, 0 };
    return bot(d, cdb, 6, data, alloc, in);
}

/* Find the mass storage interface and get the medium ready. */
static int msc_open(msc_dev_t *d, const char *busid, char *msg, size_t msg_len)
{
    memset(d, 0, sizeof(*d));
    strlcpy(d->busid, busid, sizeof(d->busid));
    usbip_backend_device_t dev;
    if (!usb_backend_get_device_by_busid(d->busid, &dev)) {
        snprintf(msg, msg_len, "no device on %s", busid);
        return 404;
    }
    d->devnum = dev.devnum;
    uint8_t *cfgbuf = malloc(1024);
    if (cfgbuf == NULL) {
        snprintf(msg, msg_len, "out of memory");
        return 503;
    }
    rescue_config_t cfg;
    rescue_parse_config(cfgbuf, usb_backend_get_config_descriptor(d->busid, cfgbuf, 1024), &cfg);
    free(cfgbuf);
    bool found = false;
    for (int i = 0; i < cfg.num_ifaces && !found; i++) {
        const rescue_iface_t *it = &cfg.ifaces[i];
        if (it->cls == 0x08 && it->subclass == 0x06 && it->protocol == 0x50 && it->ep_in && it->ep_out) {
            d->iface = it->number;
            d->ep_in = it->ep_in;
            d->ep_out = it->ep_out;
            found = true;
        }
    }
    if (!found) {
        snprintf(msg, msg_len, "no mass storage (SCSI, bulk-only) interface on this device");
        return 409;
    }
    uint8_t buf[36];
    if (scsi_simple(d, 0x12, 36, buf, true) == 0) {              /* INQUIRY */
        memcpy(d->vendor, &buf[8], 8);
        memcpy(d->product, &buf[16], 16);
        memcpy(d->revision, &buf[32], 4);
        for (char *s = d->vendor; *s; s++) if (*s < ' ') *s = ' ';
        for (char *s = d->product; *s; s++) if (*s < ' ') *s = ' ';
    }
    /* UF2 drives and CIRCUITPY report NOT READY briefly after enumerating. */
    const int64_t t0 = esp_timer_get_time();
    int rc;
    while ((rc = scsi_simple(d, 0x00, 0, NULL, false)) != 0) {  /* TEST UNIT READY */
        if (rc < 0 || esp_timer_get_time() - t0 > (int64_t)MSC_READY_WAIT_MS * 1000) {
            snprintf(msg, msg_len, "medium not ready (%s)", rc < 0 ? serial_link_errno_name(rc) : "TEST UNIT READY failed");
            return 503;
        }
        uint8_t sense[18];
        scsi_simple(d, 0x03, 18, sense, true);                  /* REQUEST SENSE clears the condition */
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    uint8_t cdb[10] = { 0x25 };
    uint8_t cap[8];
    if (bot(d, cdb, 10, cap, 8, true) != 0) {                   /* READ CAPACITY(10) */
        snprintf(msg, msg_len, "READ CAPACITY failed");
        return 502;
    }
    rescue_scsi_capacity(cap, &d->block_count, &d->block_size);
    if (d->block_size != 512 && d->block_size != 1024 && d->block_size != 2048 && d->block_size != 4096) {
        snprintf(msg, msg_len, "unsupported block size %lu", (unsigned long)d->block_size);
        return 409;
    }
    d->open = true;
    return 0;
}

/* ===========================================================================
 *  FatFS disk driver
 * ======================================================================== */

static DSTATUS msc_disk_init(BYTE pdrv)
{
    (void)pdrv;
    return s_dev.open ? 0 : STA_NOINIT;
}

static DSTATUS msc_disk_status(BYTE pdrv)
{
    (void)pdrv;
    return s_dev.open ? 0 : STA_NOINIT;
}

static DRESULT msc_disk_rw(BYTE *buff, uint32_t sector, UINT count, bool write)
{
    while (count > 0) {
        const UINT n = count > MSC_CMD_SECTORS_MAX ? MSC_CMD_SECTORS_MAX : count;
        uint8_t cdb[10];
        rescue_scsi_rw10(write ? 0x2A : 0x28, sector, (uint16_t)n, cdb);
        const int rc = bot(&s_dev, cdb, 10, buff, n * s_dev.block_size, !write);
        if (rc != 0) {
            return RES_ERROR;
        }
        buff += n * s_dev.block_size;
        sector += n;
        count -= n;
    }
    return RES_OK;
}

static DRESULT msc_disk_read(BYTE pdrv, BYTE *buff, uint32_t sector, UINT count)
{
    (void)pdrv;
    return msc_disk_rw(buff, sector, count, false);
}

static DRESULT msc_disk_write(BYTE pdrv, const BYTE *buff, uint32_t sector, UINT count)
{
    (void)pdrv;
    return msc_disk_rw((BYTE *)buff, sector, count, true);
}

static DRESULT msc_disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    (void)pdrv;
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = s_dev.block_count;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = (WORD)s_dev.block_size;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

static const ff_diskio_impl_t k_diskio = {
    .init = msc_disk_init,
    .status = msc_disk_status,
    .read = msc_disk_read,
    .write = msc_disk_write,
    .ioctl = msc_disk_ioctl,
};

typedef struct {
    FATFS *fs;
    char drive[4];
} mount_t;

/* Open the device and mount its FAT volume (lock held). */
static int mount(const char *busid, mount_t *m, char *msg, size_t msg_len)
{
    int st = msc_open(&s_dev, busid, msg, msg_len);
    if (st != 0) {
        return st;
    }
    if (s_pdrv == 0xFF && ff_diskio_get_drive(&s_pdrv) != ESP_OK) {
        snprintf(msg, msg_len, "no free FatFS drive");
        return 503;
    }
    ff_diskio_register(s_pdrv, &k_diskio);
    m->drive[0] = (char)('0' + (s_pdrv % 10));    /* FF_VOLUMES is small */
    m->drive[1] = ':';
    m->drive[2] = 0;
    m->fs = heap_caps_calloc(1, sizeof(FATFS), MALLOC_CAP_DEFAULT);
    if (m->fs == NULL) {
        snprintf(msg, msg_len, "out of memory");
        return 503;
    }
    const FRESULT fr = f_mount(m->fs, m->drive, 1);
    if (fr != FR_OK) {
        snprintf(msg, msg_len, fr == FR_NO_FILESYSTEM ? "no FAT file system on the volume"
                               : "mount failed (FatFS error %d)", (int)fr);
        heap_caps_free(m->fs);
        m->fs = NULL;
        return fr == FR_NO_FILESYSTEM ? 409 : 502;
    }
    return 0;
}

static void unmount(mount_t *m)
{
    if (m->fs != NULL) {
        f_mount(NULL, m->drive, 0);
        heap_caps_free(m->fs);
        m->fs = NULL;
    }
    if (s_pdrv != 0xFF) {
        ff_diskio_unregister(s_pdrv);
    }
    if (s_dev.busid[0]) {
        usb_backend_session_ended(s_dev.busid);    /* release the claimed interface */
    }
    s_dev.open = false;
}

static const char *fr_name(FRESULT fr)
{
    switch (fr) {
    case FR_OK: return "ok";
    case FR_DISK_ERR: return "disk error";
    case FR_NOT_READY: return "not ready";
    case FR_NO_FILE: return "no such file";
    case FR_NO_PATH: return "no such directory";
    case FR_INVALID_NAME: return "invalid name";
    case FR_DENIED: return "denied (full, read-only, or not empty)";
    case FR_EXIST: return "already exists";
    case FR_WRITE_PROTECTED: return "write protected";
    case FR_NO_FILESYSTEM: return "no FAT file system";
    default: return "FatFS error";
    }
}

static bool make_path(const mount_t *m, const char *path, char *out, size_t out_len)
{
    if (path == NULL || path[0] == '\0') {
        path = "/";
    }
    if (strlen(path) + 4 > out_len) {
        return false;
    }
    snprintf(out, out_len, "%s%s%s", m->drive, path[0] == '/' ? "" : "/", path);
    return true;
}

/* ===========================================================================
 *  Synchronous API (HTTP task)
 * ======================================================================== */

/* Reserve the device for a quick operation (no job, no USB/IP session). */
static int take(const char *busid, cJSON **out)
{
    if (busid == NULL || busid[0] == '\0' || strlen(busid) > 31) {
        return fail(out, 400, "busid required");
    }
    if (rescue_job_busy(busid)) {
        return fail(out, 409, "a rescue job is running on this device");
    }
    serial_monitor_stop(busid);
    busid_owner_info_t owner;
    if (!busid_owner_try_begin(busid, BUSID_OWNER_RESCUE, "rescue:msc", &owner)) {
        char msg[96];
        snprintf(msg, sizeof(msg), owner.kind == BUSID_OWNER_USBIP
                 ? "device is imported over USB/IP by %s; detach it first" : "device is busy (%s)", owner.peer);
        return fail(out, 409, msg);
    }
    return 0;
}

static cJSON *list_dir(const char *dirpath, int *count)
{
    cJSON *arr = cJSON_CreateArray();
    *count = 0;
    FF_DIR *dir = calloc(1, sizeof(FF_DIR));
    FILINFO *fi = calloc(1, sizeof(FILINFO));
    if (dir != NULL && fi != NULL && f_opendir(dir, dirpath) == FR_OK) {
        while (f_readdir(dir, fi) == FR_OK && fi->fname[0] != '\0' && *count < 512) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", fi->fname);
            cJSON_AddBoolToObject(o, "dir", (fi->fattrib & AM_DIR) != 0);
            cJSON_AddNumberToObject(o, "size", (double)fi->fsize);
            char date[20];
            snprintf(date, sizeof(date), "%04u-%02u-%02u %02u:%02u", (fi->fdate >> 9) + 1980, (fi->fdate >> 5) & 15,
                     fi->fdate & 31, fi->ftime >> 11, (fi->ftime >> 5) & 63);
            cJSON_AddStringToObject(o, "modified", date);
            cJSON_AddBoolToObject(o, "hidden", (fi->fattrib & (AM_HID | AM_SYS)) != 0);
            cJSON_AddItemToArray(arr, o);
            (*count)++;
        }
        f_closedir(dir);
    }
    free(dir);
    free(fi);
    return arr;
}

int msc_info(const char *busid, cJSON **out)
{
    int st = take(busid, out);
    if (st != 0) {
        return st;
    }
    lock();
    mount_t m = {0};
    char msg[128] = "";
    st = mount(busid, &m, msg, sizeof(msg));
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", st == 0);
    if (s_dev.block_size) {
        cJSON_AddStringToObject(j, "vendor", s_dev.vendor);
        cJSON_AddStringToObject(j, "product", s_dev.product);
        cJSON_AddStringToObject(j, "revision", s_dev.revision);
        cJSON_AddNumberToObject(j, "block_size", s_dev.block_size);
        cJSON_AddNumberToObject(j, "blocks", s_dev.block_count);
        cJSON_AddNumberToObject(j, "capacity", (double)s_dev.block_size * s_dev.block_count);
    }
    if (st == 0) {
        DWORD nclst = 0;
        FATFS *fsp = NULL;
        if (f_getfree(m.drive, &nclst, &fsp) == FR_OK && fsp != NULL) {
            cJSON_AddNumberToObject(j, "free", (double)nclst * fsp->csize * s_dev.block_size);
            cJSON_AddStringToObject(j, "fs", fsp->fs_type == FS_FAT12 ? "FAT12" : fsp->fs_type == FS_FAT16 ? "FAT16"
                                    : fsp->fs_type == FS_FAT32 ? "FAT32" : "exFAT");
        }
        char root[8];
        make_path(&m, "/", root, sizeof(root));
        int n = 0;
        cJSON_AddItemToObject(j, "files", list_dir(root, &n));
        /* A UF2 bootloader says so in INFO_UF2.TXT. */
        cJSON_AddBoolToObject(j, "uf2_bootloader", false);
        cJSON *files = cJSON_GetObjectItemCaseSensitive(j, "files");
        const cJSON *f;
        cJSON_ArrayForEach(f, files) {
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(f, "name");
            if (cJSON_IsString(name) && strcasecmp(name->valuestring, "INFO_UF2.TXT") == 0) {
                cJSON_ReplaceItemInObject(j, "uf2_bootloader", cJSON_CreateBool(true));
            }
        }
    } else {
        cJSON_AddStringToObject(j, "error", msg);
    }
    unmount(&m);
    unlock();
    busid_owner_end(busid, BUSID_OWNER_RESCUE);
    *out = j;
    return st == 0 ? 200 : st;
}

int msc_list(const char *busid, const char *path, cJSON **out)
{
    int st = take(busid, out);
    if (st != 0) {
        return st;
    }
    lock();
    mount_t m = {0};
    char msg[128] = "";
    st = mount(busid, &m, msg, sizeof(msg));
    cJSON *j = NULL;
    if (st == 0) {
        char p[MSC_PATH_MAX];
        if (!make_path(&m, path, p, sizeof(p))) {
            st = 400;
            snprintf(msg, sizeof(msg), "path too long");
        } else {
            FILINFO fi;
            if (strcmp(path ? path : "/", "/") != 0 && (f_stat(p, &fi) != FR_OK || !(fi.fattrib & AM_DIR))) {
                st = 404;
                snprintf(msg, sizeof(msg), "no such directory");
            } else {
                j = cJSON_CreateObject();
                cJSON_AddBoolToObject(j, "ok", true);
                cJSON_AddStringToObject(j, "path", path ? path : "/");
                int n = 0;
                cJSON_AddItemToObject(j, "files", list_dir(p, &n));
            }
        }
    }
    unmount(&m);
    unlock();
    busid_owner_end(busid, BUSID_OWNER_RESCUE);
    if (j == NULL) {
        return fail(out, st, msg);
    }
    *out = j;
    return 200;
}

typedef enum { OP_DELETE, OP_RENAME, OP_MKDIR } fs_op_t;

static int fs_op(const char *busid, const cJSON *args, fs_op_t op, cJSON **out)
{
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(args, op == OP_RENAME ? "from" : "path");
    const cJSON *b = cJSON_GetObjectItemCaseSensitive(args, "to");
    if (!cJSON_IsString(a) || (op == OP_RENAME && !cJSON_IsString(b))) {
        return fail(out, 400, op == OP_RENAME ? "from and to (paths) required" : "path required");
    }
    if (strcmp(a->valuestring, "/") == 0 || a->valuestring[0] == '\0') {
        return fail(out, 400, "not the root");
    }
    int st = take(busid, out);
    if (st != 0) {
        return st;
    }
    lock();
    mount_t m = {0};
    char msg[128] = "";
    st = mount(busid, &m, msg, sizeof(msg));
    FRESULT fr = FR_OK;
    if (st == 0) {
        char p[MSC_PATH_MAX], q[MSC_PATH_MAX];
        if (!make_path(&m, a->valuestring, p, sizeof(p)) || (op == OP_RENAME && !make_path(&m, b->valuestring, q, sizeof(q)))) {
            fr = FR_INVALID_NAME;
        } else if (op == OP_DELETE) {
            fr = f_unlink(p);
        } else if (op == OP_MKDIR) {
            fr = f_mkdir(p);
        } else {
            fr = f_rename(p, q);
        }
        if (fr != FR_OK) {
            st = (fr == FR_NO_FILE || fr == FR_NO_PATH) ? 404 : (fr == FR_EXIST || fr == FR_DENIED) ? 409 : 502;
            snprintf(msg, sizeof(msg), "%s", fr_name(fr));
        }
    }
    unmount(&m);
    unlock();
    busid_owner_end(busid, BUSID_OWNER_RESCUE);
    if (st != 0) {
        return fail(out, st, msg);
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    *out = j;
    return 200;
}

int msc_delete(const char *busid, const cJSON *args, cJSON **out)
{
    return fs_op(busid, args, OP_DELETE, out);
}

int msc_rename(const char *busid, const cJSON *args, cJSON **out)
{
    return fs_op(busid, args, OP_RENAME, out);
}

int msc_mkdir(const char *busid, const cJSON *args, cJSON **out)
{
    return fs_op(busid, args, OP_MKDIR, out);
}

/* ===========================================================================
 *  Jobs
 * ======================================================================== */

static int write_file(rescue_job_t *job, const char *busid, const cJSON *args, bool uf2)
{
    const cJSON *file = cJSON_GetObjectItemCaseSensitive(args, "file");
    const cJSON *path = cJSON_GetObjectItemCaseSensitive(args, "path");
    if (!cJSON_IsString(file)) {
        return rescue_job_fail(job, "file (staged name) required");
    }
    char dest[MSC_PATH_MAX];
    if (cJSON_IsString(path)) {
        strlcpy(dest, path->valuestring, sizeof(dest));
    } else if (uf2) {
        strlcpy(dest, "/FIRMWARE.UF2", sizeof(dest));
    } else {
        snprintf(dest, sizeof(dest), "/%s", file->valuestring);
    }
    const uint8_t *data;
    size_t len;
    if (!blob_acquire(file->valuestring, &data, &len)) {
        return rescue_job_fail(job, "no staged file named %s", file->valuestring);
    }
    if (uf2) {
        rescue_uf2_block_t blk;
        if (!rescue_uf2_block(data, len, 0, &blk) || (len % RESCUE_UF2_BLOCK) != 0) {
            blob_release(file->valuestring);
            return rescue_job_fail(job, "%s is not a UF2 file", file->valuestring);
        }
        const char *fam = rescue_uf2_family_name(blk.family_id);
        rescue_job_log(job, "UF2: %u blocks, family %s", (unsigned)(len / RESCUE_UF2_BLOCK), fam ? fam : "none/unknown");
        /* No mass storage yet: enter the bootloader first. */
        usbip_backend_device_t d;
        uint8_t *cfgbuf = malloc(1024);
        bool has_msc = false;
        if (cfgbuf != NULL && usb_backend_get_device_by_busid(busid, &d)) {
            rescue_config_t cfg;
            rescue_parse_config(cfgbuf, usb_backend_get_config_descriptor(busid, cfgbuf, 1024), &cfg);
            for (int i = 0; i < cfg.num_ifaces; i++) {
                has_msc |= cfg.ifaces[i].cls == 0x08;
            }
            if (!has_msc && !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(args, "enter_bootloader"))) {
                rescue_caps_t caps;
                rescue_classify(d.id_vendor, d.id_product, d.bcd_device, &cfg, 0, &caps);
                rescue_job_progress(job, 2, "entering the bootloader");
                if (caps.rp_reset_if >= 0) {
                    rescue_ctrl_t r;
                    rescue_rp_reset_request(caps.rp_reset_if, true, &r);
                    usb_setup_packet_t setup = { .bmRequestType = r.bm, .bRequest = r.req, .wValue = r.value, .wIndex = r.index };
                    usb_backend_transfer_sync(busid, 0, &setup, NULL, 0, NULL, 0, 1000, NULL, NULL);
                    rescue_job_log(job, "rebooted into BOOTSEL through the reset interface");
                } else if (caps.serial == RESCUE_SERIAL_CDC_ACM) {
                    serial_link_t *l = calloc(1, sizeof(*l));
                    char msg[96];
                    if (l != NULL && serial_link_open(l, busid, 0, msg, sizeof(msg)) == 0) {
                        serial_link_set_lines(l, true, true);
                        serial_link_set_baud(l, 1200);
                        vTaskDelay(pdMS_TO_TICKS(50));
                        serial_link_set_lines(l, false, false);
                        serial_link_close(l);
                    }
                    free(l);
                    rescue_job_log(job, "1200-baud touch");
                }
                usbip_backend_device_t now;
                bool dropped;
                if (!device_rescue_wait_reenumeration(busid, d.devnum, 6000, &now, &dropped)) {
                    free(cfgbuf);
                    blob_release(file->valuestring);
                    return rescue_job_fail(job, "no mass storage on the device and it did not re-enumerate into a bootloader");
                }
                rescue_job_log(job, "bootloader: %04x:%04x %s", now.id_vendor, now.id_product, now.product);
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }
        free(cfgbuf);
    }

    lock();
    mount_t m = {0};
    char msg[128] = "";
    rescue_job_progress(job, 5, "mounting");
    int st = mount(busid, &m, msg, sizeof(msg));
    int rc = 0;
    if (st != 0) {
        rc = rescue_job_fail(job, "%s", msg);
    }
    const uint32_t devnum = s_dev.devnum;
    size_t written = 0;
    FIL *f = rc == 0 ? heap_caps_calloc(1, sizeof(FIL), MALLOC_CAP_DEFAULT) : NULL;
    char p[MSC_PATH_MAX];
    if (rc == 0 && (f == NULL || !make_path(&m, dest, p, sizeof(p)))) {
        rc = rescue_job_fail(job, f == NULL ? "out of memory" : "path too long");
    }
    FRESULT fr = FR_OK;
    if (rc == 0) {
        rescue_job_log(job, "writing %s (%u bytes) to %s on %s %s", file->valuestring, (unsigned)len, dest,
                       s_dev.vendor, s_dev.product);
        fr = f_open(f, p, FA_CREATE_ALWAYS | FA_WRITE);
        if (fr != FR_OK) {
            rc = rescue_job_fail(job, "cannot create %s: %s", dest, fr_name(fr));
        }
    }
    const int64_t t0 = esp_timer_get_time();
    while (rc == 0 && written < len) {
        if (rescue_job_cancelled(job)) {
            rc = rescue_job_fail(job, "cancelled");
            break;
        }
        UINT n = (len - written) > MSC_COPY_CHUNK ? MSC_COPY_CHUNK : (UINT)(len - written);
        UINT bw = 0;
        fr = f_write(f, data + written, n, &bw);
        written += bw;
        if (fr != FR_OK || bw != n) {
            break;
        }
        rescue_job_progress(job, 5 + (int)(90 * written / len), "copying");
    }
    if (rc == 0) {
        const FRESULT fc = (fr == FR_OK) ? f_close(f) : fr;
        bool dropped = false;
        if (fc != FR_OK && uf2 && written >= len - RESCUE_UF2_BLOCK) {
            /* A UF2 bootloader reboots after the last block: the closing FAT writes fail. */
            usbip_backend_device_t now;
            device_rescue_wait_reenumeration(busid, devnum, 3000, &now, &dropped);
        }
        if (fc == FR_OK || dropped) {
            const double secs = (double)(esp_timer_get_time() - t0) / 1e6;
            rescue_job_log(job, "%u bytes copied in %.1f s%s", (unsigned)written, secs,
                           dropped ? "; the bootloader rebooted the board (flashed)" : "");
            cJSON *res = rescue_job_result(job);
            cJSON_AddStringToObject(res, "path", dest);
            cJSON_AddNumberToObject(res, "bytes", (double)written);
            cJSON_AddBoolToObject(res, "device_rebooted", dropped);
            cJSON_AddNumberToObject(res, "seconds", secs);
        } else {
            rc = rescue_job_fail(job, "write failed after %u of %u bytes: %s (%s)", (unsigned)written, (unsigned)len,
                                 fr_name(fc), serial_link_errno_name(s_dev.last_err));
        }
    }
    heap_caps_free(f);
    unmount(&m);
    unlock();
    blob_release(file->valuestring);
    return rc;
}

int msc_write_job(rescue_job_t *job, const char *busid, const cJSON *args)
{
    return write_file(job, busid, args, false);
}

int msc_uf2_job(rescue_job_t *job, const char *busid, const cJSON *args)
{
    return write_file(job, busid, args, true);
}

int msc_read_job(rescue_job_t *job, const char *busid, const cJSON *args)
{
    const cJSON *path = cJSON_GetObjectItemCaseSensitive(args, "path");
    const cJSON *file = cJSON_GetObjectItemCaseSensitive(args, "file");
    if (!cJSON_IsString(path)) {
        return rescue_job_fail(job, "path required");
    }
    char name[BLOB_NAME_MAX];
    if (cJSON_IsString(file)) {
        strlcpy(name, file->valuestring, sizeof(name));
    } else {
        const char *base = strrchr(path->valuestring, '/');
        strlcpy(name, base ? base + 1 : path->valuestring, sizeof(name));
    }
    if (!blob_name_valid(name)) {
        return rescue_job_fail(job, "give file: a staged name ([A-Za-z0-9._-])");
    }
    lock();
    mount_t m = {0};
    char msg[128] = "";
    rescue_job_progress(job, 5, "mounting");
    int rc = 0;
    if (mount(busid, &m, msg, sizeof(msg)) != 0) {
        rc = rescue_job_fail(job, "%s", msg);
    }
    FIL *f = rc == 0 ? heap_caps_calloc(1, sizeof(FIL), MALLOC_CAP_DEFAULT) : NULL;
    char p[MSC_PATH_MAX];
    if (rc == 0 && (f == NULL || !make_path(&m, path->valuestring, p, sizeof(p)))) {
        rc = rescue_job_fail(job, f == NULL ? "out of memory" : "path too long");
    }
    if (rc == 0) {
        const FRESULT fr = f_open(f, p, FA_READ);
        if (fr != FR_OK) {
            rc = rescue_job_fail(job, "cannot open %s: %s", path->valuestring, fr_name(fr));
        }
    }
    if (rc == 0) {
        const size_t size = f_size(f);
        uint8_t *buf = NULL;
        const esp_err_t err = blob_begin(name, size ? size : 1, &buf);
        if (err != ESP_OK) {
            rc = rescue_job_fail(job, "cannot stage %u bytes as %s (%s)", (unsigned)size, name, esp_err_to_name(err));
        } else {
            size_t got = 0;
            FRESULT fr = FR_OK;
            while (got < size) {
                UINT n = (size - got) > MSC_COPY_CHUNK ? MSC_COPY_CHUNK : (UINT)(size - got);
                UINT br = 0;
                fr = f_read(f, buf + got, n, &br);
                got += br;
                if (fr != FR_OK || br == 0) {
                    break;
                }
                rescue_job_progress(job, 5 + (int)(90 * got / size), "copying");
            }
            if (got == size) {
                blob_commit(name, size);
                rescue_job_log(job, "%s (%u bytes) staged as %s", path->valuestring, (unsigned)size, name);
                cJSON *res = rescue_job_result(job);
                cJSON_AddStringToObject(res, "file", name);
                cJSON_AddNumberToObject(res, "bytes", (double)size);
                char url[96];
                snprintf(url, sizeof(url), "/api/rescue/files/%s", name);
                cJSON_AddStringToObject(res, "download", url);
            } else {
                blob_abort(name);
                rc = rescue_job_fail(job, "read failed after %u of %u bytes: %s", (unsigned)got, (unsigned)size, fr_name(fr));
            }
        }
        f_close(f);
    }
    heap_caps_free(f);
    unmount(&m);
    unlock();
    (void)TAG;
    return rc;
}
