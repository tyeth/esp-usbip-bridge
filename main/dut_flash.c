#include "dut_flash.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "blob_store.h"
#include "dut_fs.h"
#include "dut_part_logic.h"
#include "esp_flasher.h"
#include "picoboot_flasher.h"
#include "rescue_logic.h"
#include "samba_flasher.h"
#include "usb_backend.h"

#define DF_UNIT             4096u
#define DF_MAX_REGIONS      40
#define DF_CACHE_MAX        (2u * 1024 * 1024)  /* RAM shadow before clean sectors are dropped */
#define DF_RUN_MAX          16u                 /* sectors per flash write */
#define DF_READ_CHUNK       0x10000u
#define DF_OPS_KEEP         24
#define DF_KEEP_OPEN_MAX    900
#define DF_HDR              64u                 /* bytes read per block when scanning */
#define DF_RP_DEFAULT_SIZE  0x200000u

typedef enum { PLAT_ESP, PLAT_RP2, PLAT_SAMD } plat_t;

typedef struct {
    char name[24];
    char type[24];
    uint32_t offset, size;
    bool protect;               /* code or system data: no file system writes without force */
    bool probe;                 /* look for a file system in it */
    bool probed;
    dpl_fs_info_t fs;
} region_t;

typedef struct df df_t;

/* The RAM shadow of one region: 4 KB sectors loaded on first use, changed
   ones written back by sh_flush. */
typedef struct {
    df_t *df;
    uint32_t off, size, units;
    uint8_t **blk;
    uint8_t *dirty;
    uint32_t loaded;
    bool may_flush;             /* false during read-only ops: changes are dropped */
    int err;
} shadow_t;

struct df {
    rescue_job_t *job;
    char busid[32];
    plat_t plat;
    espf_t *esp;
    pb_session_t *pb;
    samba_t *samba;
    uint32_t base;              /* bus address of flash offset 0 */
    uint32_t flash_size;
    bool writable;
    bool encrypted;
    const char *chip;
    uint32_t lfs_prog_default;
    region_t regions[DF_MAX_REGIONS];
    int nregions;
    cJSON *notes;
    shadow_t sh;
    uint32_t sectors_written;
};

/* The open session, for requests that add ops to it. */
static struct {
    bool active, accepting, close;
    char busid[32];
    uint32_t job_id;
    cJSON *pending;
} s_sess;
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

static bool parse_u32(const cJSON *v, uint32_t *out)
{
    if (cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble <= 4294967295.0) {
        *out = (uint32_t)v->valuedouble;
        return true;
    }
    if (cJSON_IsString(v)) {
        char *end = NULL;
        const unsigned long n = strtoul(v->valuestring, &end, 0);
        if (end != v->valuestring) {
            uint32_t m = 1;
            if (*end == 'K' || *end == 'k') {
                m = 1024;
                end++;
            } else if (*end == 'M' || *end == 'm') {
                m = 1024 * 1024;
                end++;
            }
            if (*end == 'B' || *end == 'b') {
                end++;
            }
            if (*end == '\0') {
                *out = (uint32_t)n * m;
                return true;
            }
        }
    }
    return false;
}

/* ===========================================================================
 *  The device
 * ======================================================================== */

static int dev_read(df_t *df, uint32_t off, uint8_t *buf, uint32_t len)
{
    if (off > df->flash_size || len > df->flash_size - off) {
        return -1;
    }
    switch (df->plat) {
    case PLAT_ESP: return esp_flasher_read(df->esp, off, buf, len);
    case PLAT_RP2: return picoboot_read(df->pb, df->base + off, buf, len);
    default: return samba_read(df->samba, off, buf, len);
    }
}

/* Whole sectors: erased and written. */
static int dev_write(df_t *df, uint32_t off, const uint8_t *buf, uint32_t len)
{
    if (!df->writable || off % DF_UNIT || len % DF_UNIT || off > df->flash_size || len > df->flash_size - off) {
        return -1;
    }
    df->sectors_written += len / DF_UNIT;
    switch (df->plat) {
    case PLAT_ESP: return esp_flasher_write(df->esp, df->job, off, buf, len);
    case PLAT_RP2: return picoboot_write(df->pb, df->base + off, buf, len);
    default: return -1;
    }
}

static bool want(const cJSON *args, const char *key, const char *value)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, key);
    return cJSON_IsString(v) && strcmp(v->valuestring, value) == 0;
}

/* Pick the platform: asked for, or clear from the USB identity. */
static int pick_platform(df_t *df, const cJSON *args)
{
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(args, "platform");
    if (cJSON_IsString(p) && strcmp(p->valuestring, "auto") != 0) {
        if (strcmp(p->valuestring, "esp") == 0) {
            df->plat = PLAT_ESP;
        } else if (strcmp(p->valuestring, "rp2") == 0) {
            df->plat = PLAT_RP2;
        } else if (strcmp(p->valuestring, "samd") == 0) {
            df->plat = PLAT_SAMD;
        } else {
            return rescue_job_fail(df->job, "platform: esp, rp2 or samd");
        }
        return 0;
    }
    usbip_backend_device_t d;
    if (!usb_backend_get_device_by_busid(df->busid, &d)) {
        return rescue_job_fail(df->job, "no device on %s", df->busid);
    }
    uint8_t *cfg = malloc(1024);
    if (cfg == NULL) {
        return rescue_job_fail(df->job, "out of memory");
    }
    rescue_config_t c;
    rescue_parse_config(cfg, usb_backend_get_config_descriptor(df->busid, cfg, 1024), &c);
    free(cfg);
    rescue_caps_t caps;
    rescue_classify(d.id_vendor, d.id_product, d.bcd_device, &c, 0, &caps);
    if (caps.picoboot_if >= 0 || caps.rp_reset_if >= 0 || d.id_vendor == 0x2e8a) {
        df->plat = PLAT_RP2;
    } else if (caps.esp_usb_jtag || d.id_vendor == 0x303a || caps.serial == RESCUE_SERIAL_CP210X ||
               caps.serial == RESCUE_SERIAL_CH34X || caps.serial == RESCUE_SERIAL_FTDI) {
        df->plat = PLAT_ESP;
    } else {
        return rescue_job_fail(df->job, "%04x:%04x could be an ESP, RP2 or SAMD board: say platform (esp, rp2, samd)",
                               d.id_vendor, d.id_product);
    }
    return 0;
}

static int dev_open(df_t *df, const cJSON *args)
{
    int rc = pick_platform(df, args);
    if (rc != 0) {
        return rc;
    }
    if (df->plat == PLAT_ESP) {
        df->esp = esp_flasher_open(df->job, df->busid, args, &df->encrypted);
        if (df->esp == NULL) {
            return -1;
        }
        df->flash_size = esp_flasher_flash_size(df->esp);
        df->chip = esp_flasher_chip(df->esp);
        df->writable = true;
        df->lfs_prog_default = 128;                     /* arduino-esp32 / esp_littlefs */
    } else if (df->plat == PLAT_RP2) {
        df->pb = picoboot_open(df->job, df->busid, args);
        if (df->pb == NULL) {
            return -1;
        }
        df->base = DPL_RP_FLASH_BASE;
        df->chip = picoboot_is_rp2350(df->pb) ? "RP2350" : "RP2040";
        df->flash_size = DPL_RP_FLASH_MAX;              /* until the layout says */
        df->writable = true;
        df->lfs_prog_default = 256;                     /* arduino-pico */
    } else {
        df->samba = samba_open(df->job, df->busid, args);
        if (df->samba == NULL) {
            return -1;
        }
        uint32_t page, app;
        samba_geometry(df->samba, &df->flash_size, &page, &app, &df->chip);
        df->writable = false;
        df->lfs_prog_default = 256;
    }
    return 0;
}

static void dev_close(df_t *df, const cJSON *args)
{
    const bool stay = want(args, "after", "no_reset") || want(args, "after", "no-reset");
    if (df->esp != NULL) {
        esp_flasher_close(df->esp, df->job, args);
    }
    if (df->pb != NULL) {
        picoboot_close(df->pb, !stay);
    }
    if (df->samba != NULL) {
        samba_close(df->samba, !stay);
    }
    df->esp = NULL;
    df->pb = NULL;
    df->samba = NULL;
}

/* ===========================================================================
 *  The RAM shadow
 * ======================================================================== */

static void sh_free(shadow_t *s)
{
    for (uint32_t u = 0; u < s->units; u++) {
        if (s->blk[u] != NULL) {
            blob_unreserve(s->blk[u], DF_UNIT);
        }
    }
    free(s->blk);
    free(s->dirty);
    memset(s, 0, sizeof(*s));
}

static int sh_init(shadow_t *s, df_t *df, uint32_t off, uint32_t size)
{
    if (s->blk != NULL && s->off == off && s->size == size) {
        return 0;
    }
    sh_free(s);
    s->df = df;
    s->off = off;
    s->size = size;
    s->units = (size + DF_UNIT - 1) / DF_UNIT;
    s->blk = calloc(s->units, sizeof(uint8_t *));
    s->dirty = calloc(s->units, 1);
    if (s->blk == NULL || s->dirty == NULL) {
        sh_free(s);
        return -1;
    }
    return 0;
}

/* Write the changed sectors back, in runs of up to DF_RUN_MAX. */
static int sh_flush(shadow_t *s)
{
    if (s->blk == NULL) {
        return 0;
    }
    uint8_t *run = NULL;
    int rc = 0;
    for (uint32_t u = 0; u < s->units; u++) {
        if (!s->dirty[u]) {
            continue;
        }
        uint32_t n = 0;
        while (u + n < s->units && n < DF_RUN_MAX && s->dirty[u + n]) {
            n++;
        }
        if (run == NULL && (run = heap_caps_malloc(DF_RUN_MAX * DF_UNIT, MALLOC_CAP_SPIRAM)) == NULL) {
            return -1;
        }
        for (uint32_t k = 0; k < n; k++) {
            memcpy(run + k * DF_UNIT, s->blk[u + k], DF_UNIT);
        }
        if (dev_write(s->df, s->off + u * DF_UNIT, run, n * DF_UNIT) != 0) {
            rescue_job_log(s->df->job, "writing 0x%lx (%lu sectors) failed", (unsigned long)(s->off + u * DF_UNIT),
                           (unsigned long)n);
            rc = -1;
            break;
        }
        memset(&s->dirty[u], 0, n);
        u += n - 1;
    }
    heap_caps_free(run);
    return rc;
}

static bool sh_any_dirty(const shadow_t *s)
{
    for (uint32_t u = 0; s->blk && u < s->units; u++) {
        if (s->dirty[u]) {
            return true;
        }
    }
    return false;
}

/* Over the RAM budget: write back (or drop, in a read-only op) and free. */
static void sh_trim(shadow_t *s)
{
    if (s->may_flush) {
        if (sh_flush(s) != 0) {
            s->err = -1;
            return;
        }
    }
    for (uint32_t u = 0; u < s->units; u++) {
        if (s->blk[u] != NULL && (!s->dirty[u] || !s->may_flush)) {
            blob_unreserve(s->blk[u], DF_UNIT);
            s->blk[u] = NULL;
            s->dirty[u] = 0;
            s->loaded--;
        }
    }
}

static uint8_t *sh_unit(shadow_t *s, uint32_t u, bool load)
{
    if (s->blk[u] != NULL) {
        return s->blk[u];
    }
    if ((s->loaded + 1) * DF_UNIT > DF_CACHE_MAX) {
        sh_trim(s);
    }
    uint8_t *b = blob_reserve(DF_UNIT, "dut_flash");
    if (b == NULL) {
        sh_trim(s);
        b = blob_reserve(DF_UNIT, "dut_flash");
    }
    if (b == NULL) {
        return NULL;
    }
    const uint32_t n = s->size - u * DF_UNIT < DF_UNIT ? s->size - u * DF_UNIT : DF_UNIT;
    memset(b, 0xFF, DF_UNIT);
    if (load && dev_read(s->df, s->off + u * DF_UNIT, b, n) != 0) {
        blob_unreserve(b, DF_UNIT);
        return NULL;
    }
    s->blk[u] = b;
    s->loaded++;
    return b;
}

typedef enum { SH_READ, SH_WRITE, SH_PROGRAM } sh_mode_t;

static int sh_access(shadow_t *s, uint32_t off, uint8_t *buf, uint32_t len, sh_mode_t mode)
{
    if (s->err || off > s->size || len > s->size - off) {
        return -1;
    }
    while (len > 0) {
        const uint32_t u = off / DF_UNIT, o = off % DF_UNIT;
        const uint32_t n = DF_UNIT - o < len ? DF_UNIT - o : len;
        uint8_t *b = sh_unit(s, u, true);
        if (b == NULL) {
            return -1;
        }
        if (mode == SH_READ) {
            memcpy(buf, b + o, n);
        } else {
            for (uint32_t i = 0; i < n; i++) {
                const uint8_t v = mode == SH_PROGRAM ? (uint8_t)(b[o + i] & buf[i]) : buf[i];
                if (v != b[o + i]) {
                    b[o + i] = v;
                    s->dirty[u] = 1;
                }
            }
        }
        off += n;
        buf += n;
        len -= n;
    }
    return 0;
}

static int bd_read(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    return sh_access(ctx, off, buf, len, SH_READ);
}

static int bd_write(void *ctx, uint32_t off, const void *buf, uint32_t len)
{
    return sh_access(ctx, off, (uint8_t *)buf, len, SH_WRITE);
}

static int bd_program(void *ctx, uint32_t off, const void *buf, uint32_t len)
{
    return sh_access(ctx, off, (uint8_t *)buf, len, SH_PROGRAM);
}

static int bd_erase(void *ctx, uint32_t off, uint32_t len)
{
    shadow_t *s = ctx;
    if (s->err || off % DF_UNIT || len % DF_UNIT || off > s->size || len > s->size - off) {
        return -1;
    }
    for (uint32_t u = off / DF_UNIT; u < (off + len) / DF_UNIT; u++) {
        const bool known = s->blk[u] != NULL;
        uint8_t *b = sh_unit(s, u, false);              /* not read: it is erased anyway */
        if (b == NULL) {
            return -1;
        }
        if (!known || !dpl_blank(b, DF_UNIT)) {
            memset(b, 0xFF, DF_UNIT);
            s->dirty[u] = 1;                            /* unread sectors may not be blank on the flash */
        }
    }
    return 0;
}

/* ===========================================================================
 *  Regions
 * ======================================================================== */

static region_t *add_region(df_t *df, const char *name, const char *type, uint32_t off, uint32_t size,
                            bool protect, bool probe)
{
    if (df->nregions >= DF_MAX_REGIONS || size == 0 || off >= df->flash_size) {
        return NULL;
    }
    if (size > df->flash_size - off) {
        size = df->flash_size - off;
    }
    region_t *r = &df->regions[df->nregions++];
    memset(r, 0, sizeof(*r));
    strlcpy(r->name, name, sizeof(r->name));
    strlcpy(r->type, type, sizeof(r->type));
    r->offset = off;
    r->size = size;
    r->protect = protect;
    r->probe = probe;
    return r;
}

static void note(df_t *df, const char *text)
{
    cJSON_AddItemToArray(df->notes, cJSON_CreateString(text));
    rescue_job_log(df->job, "%s", text);
}

/* Block 0, block 1 and the last block, straight from the device. */
static void probe(df_t *df, region_t *r)
{
    uint8_t *b = heap_caps_malloc(3 * DF_UNIT, MALLOC_CAP_SPIRAM);
    if (b == NULL || r->size < DF_UNIT) {
        heap_caps_free(b);
        return;
    }
    const bool two = r->size >= 2 * DF_UNIT;
    const bool last = r->size >= 3 * DF_UNIT;
    if (dev_read(df, r->offset, b, DF_UNIT) == 0 &&
            (!two || dev_read(df, r->offset + DF_UNIT, b + DF_UNIT, DF_UNIT) == 0) &&
            (!last || dev_read(df, r->offset + (r->size / DF_UNIT - 1) * DF_UNIT, b + 2 * DF_UNIT, DF_UNIT) == 0)) {
        dpl_detect(b, two ? b + DF_UNIT : NULL, last ? b + 2 * DF_UNIT : NULL, DF_UNIT, r->offset, r->size, &r->fs);
        r->probed = true;
    }
    heap_caps_free(b);
}

static void map_esp(df_t *df)
{
    add_region(df, "flash", "flash", 0, df->flash_size, true, false);
    add_region(df, "bootloader", "bootloader", 0, DPL_ESP_PT_OFFSET, true, false);
    uint8_t *pt = malloc(DPL_ESP_PT_SIZE);
    dpl_esp_part_t *parts = calloc(DPL_ESP_PT_MAX, sizeof(*parts));
    if (pt == NULL || parts == NULL || dev_read(df, DPL_ESP_PT_OFFSET, pt, DPL_ESP_PT_SIZE) != 0) {
        note(df, "could not read the partition table at 0x8000");
        free(pt);
        free(parts);
        return;
    }
    bool md5 = false;
    const int n = dpl_esp_parse_table(pt, DPL_ESP_PT_SIZE, parts, DPL_ESP_PT_MAX, &md5);
    if (n < 0) {
        note(df, "no partition table at 0x8000 (blank or not an ESP-IDF layout)");
    } else {
        add_region(df, "partition_table", "partition_table", DPL_ESP_PT_OFFSET, 0x1000, true, false);
    }
    for (int i = 0; i < n; i++) {
        const dpl_esp_part_t *p = &parts[i];
        char sub[16], type[24];
        const char *tn = dpl_esp_type_name(p->type);
        const char *sn = dpl_esp_subtype_name(p->type, p->subtype, sub, sizeof(sub));
        if (sn == NULL) {
            snprintf(sub, sizeof(sub), "0x%02x", p->subtype);
            sn = sub;
        }
        snprintf(type, sizeof(type), "%s:%s", tn ? tn : "?", sn);
        /* Code and the system's own data are not file systems. */
        const bool system = p->type != 0x01 || p->subtype <= 0x05;
        region_t *r = add_region(df, p->label[0] ? p->label : "?", type, p->offset, p->size, system, !system);
        if (r != NULL && (p->flags & 1)) {
            r->probe = false;
            note(df, "an encrypted partition is listed but not browsed");
        }
    }
    if (df->encrypted) {
        note(df, "flash encryption is on: file systems are not browsed (the bootloader reads ciphertext)");
        for (int i = 0; i < df->nregions; i++) {
            df->regions[i].probe = false;
        }
    }
    free(pt);
    free(parts);
}

/* Look for file systems on 4 KB boundaries in [from, to): a 64-byte header
   per block, the whole block when it could be a LittleFS or FAT start. */
static void scan_fs(df_t *df, uint32_t from, uint32_t to)
{
    uint8_t hdr[DF_HDR];
    uint8_t *blk = heap_caps_malloc(2 * DF_UNIT, MALLOC_CAP_SPIRAM);
    if (blk == NULL) {
        return;
    }
    rescue_job_log(df->job, "scanning 0x%lx-0x%lx for file systems", (unsigned long)from, (unsigned long)to);
    for (uint32_t off = from; off + DF_UNIT <= to && !rescue_job_cancelled(df->job); off += DF_UNIT) {
        if (dev_read(df, off, hdr, DF_HDR) != 0) {
            break;
        }
        const bool lfs = memcmp(&hdr[8], "littlefs", 8) == 0;
        const bool fat = (hdr[0] == 0xEB && hdr[2] == 0x90) || hdr[0] == 0xE9;
        if (!lfs && !fat) {
            continue;
        }
        dpl_fs_info_t fs;
        if (dev_read(df, off, blk, DF_UNIT) != 0) {
            break;
        }
        if (lfs && dpl_lfs_superblock(blk, DF_UNIT, &fs)) {
            /* The superblock pair is blocks 0 and 1; scanning upwards this is
               the first, so the file system starts here (the other copy, if
               any, is in the next block). */
            const uint32_t size = fs.block_size * fs.block_count;
            if (size <= to - off) {
                char name[24];
                snprintf(name, sizeof(name), "littlefs@%lx", (unsigned long)off);
                region_t *r = add_region(df, name, "fs:littlefs", off, size, false, true);
                if (r != NULL) {
                    r->fs = fs;
                    r->probed = true;
                }
                off += size - DF_UNIT;
                continue;
            }
        }
        if (fat && dpl_fat_bpb(blk, DF_UNIT, &fs)) {
            const uint64_t size = (uint64_t)fs.block_size * fs.block_count;
            if (size <= to - off) {
                char name[24];
                snprintf(name, sizeof(name), "fat@%lx", (unsigned long)off);
                region_t *r = add_region(df, name, "fs:fat", off, (uint32_t)size, false, true);
                if (r != NULL) {
                    r->fs = fs;
                    r->probed = true;
                }
                off += ((uint32_t)size + DF_UNIT - 1) / DF_UNIT * DF_UNIT - DF_UNIT;
            }
        }
    }
    heap_caps_free(blk);
}

/* Flash size by mirroring: a smaller chip answers the same bytes at +size. */
static uint32_t rp2_mirror_size(df_t *df)
{
    uint8_t a[DF_HDR], b[DF_HDR], c[DF_HDR], d[DF_HDR];
    const uint32_t p1 = 0x100, p2 = 0x1f00;
    if (dev_read(df, p1, a, DF_HDR) != 0 || dev_read(df, p2, c, DF_HDR) != 0 ||
            (dpl_blank(a, DF_HDR) && dpl_blank(c, DF_HDR))) {
        return 0;
    }
    for (uint32_t size = 0x80000; size < DPL_RP_FLASH_MAX; size <<= 1) {
        if (dev_read(df, size + p1, b, DF_HDR) != 0 || dev_read(df, size + p2, d, DF_HDR) != 0) {
            return 0;
        }
        if (memcmp(a, b, DF_HDR) == 0 && memcmp(c, d, DF_HDR) == 0) {
            return size;
        }
    }
    return DPL_RP_FLASH_MAX;
}

static void map_rp2(df_t *df, const cJSON *args)
{
    uint32_t fs_start = 0, fs_end = 0, eeprom = 0, size = 0;
    uint8_t w[16];
    const bool ap = dev_read(df, DPL_RP_LAYOUT_ADDR - DPL_RP_FLASH_BASE, w, sizeof(w)) == 0 &&
                    dpl_arduino_pico_layout(w, &fs_start, &fs_end, &eeprom, &size);
    uint32_t asked = 0;
    if (parse_u32(cJSON_GetObjectItemCaseSensitive(args, "flash_size"), &asked) && asked >= 0x40000 &&
            asked <= DPL_RP_FLASH_MAX) {
        df->flash_size = asked;
    } else if (ap) {
        df->flash_size = size;
    } else {
        const uint32_t m = rp2_mirror_size(df);
        df->flash_size = m ? m : DF_RP_DEFAULT_SIZE;
        note(df, m ? "flash size found by address mirroring" : "flash size unknown: 2 MB assumed (give flash_size)");
    }
    add_region(df, "flash", "flash", 0, df->flash_size, true, false);
    bool have_table = false;
    if (picoboot_is_rp2350(df->pb)) {
        uint32_t *words = calloc(64, sizeof(uint32_t));
        const uint32_t params[3] = { DPL_PT_INFO_PT_INFO | DPL_PT_INFO_LOCATION_AND_FLAGS | DPL_PT_INFO_NAME, 0, 0 };
        dpl_rp_part_t *parts = calloc(DPL_RP_PART_MAX, sizeof(*parts));
        if (words != NULL && parts != NULL &&
                picoboot_get_info(df->pb, 2, params, (uint8_t *)words, 64 * sizeof(uint32_t)) == 0) {
            const int n = dpl_rp2350_pt_info(words, 64, params[0], &have_table, parts, DPL_RP_PART_MAX);
            if (n < 0) {
                note(df, "the RP2350 partition table answer could not be parsed");
            }
            for (int i = 0; have_table && i < n; i++) {
                char name[24];
                if (parts[i].name[0]) {
                    strlcpy(name, parts[i].name, sizeof(name));
                } else {
                    snprintf(name, sizeof(name), "partition_%d", i);
                }
                add_region(df, name, "rp2350:partition", parts[i].start, parts[i].end - parts[i].start, false, true);
            }
        } else {
            note(df, "PICOBOOT GET_INFO (partition table) failed");
        }
        free(words);
        free(parts);
    }
    if (ap) {
        add_region(df, "sketch", "app", 0, fs_start - DPL_RP_FLASH_BASE, true, false);
        if (fs_end > fs_start) {
            add_region(df, "fs", "fs:arduino-pico", fs_start - DPL_RP_FLASH_BASE, fs_end - fs_start, false, true);
        } else {
            note(df, "arduino-pico layout: no file system configured (Flash Size menu)");
        }
        add_region(df, "eeprom", "data:eeprom", eeprom - DPL_RP_FLASH_BASE, DF_UNIT, true, false);
    } else if (!have_table) {
        /* CircuitPython (FAT) and MicroPython (LittleFS) keep their file
           system in flash after the firmware: find it by its content. */
        scan_fs(df, 0x1000, df->flash_size);
    }
}

static void map_samd(df_t *df)
{
    uint32_t page, app;
    const char *family;
    samba_geometry(df->samba, &df->flash_size, &page, &app, &family);
    add_region(df, "flash", "flash", 0, df->flash_size, true, false);
    add_region(df, "bootloader", "bootloader", 0, app, true, false);
    add_region(df, "application", "app", app, df->flash_size - app, true, false);
    scan_fs(df, app, df->flash_size);
    note(df, "internal flash only: an external QSPI/SPI flash (e.g. CIRCUITPY on a Metro M4) is not reachable through SAM-BA");
    note(df, "SAM-BA is read-only here: its Arduino X command erases to the end of the flash");
}

static cJSON *regions_json(df_t *df)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < df->nregions; i++) {
        region_t *r = &df->regions[i];
        if (r->probe && !r->probed) {
            probe(df, r);
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "index", i);
        cJSON_AddStringToObject(o, "name", r->name);
        cJSON_AddStringToObject(o, "type", r->type);
        cJSON_AddNumberToObject(o, "offset", r->offset);
        cJSON_AddNumberToObject(o, "size", r->size);
        cJSON_AddBoolToObject(o, "protected", r->protect);
        if (r->probed) {
            cJSON_AddStringToObject(o, "fs", dpl_fs_name(r->fs.kind));
            if (r->fs.kind == DPL_FS_LITTLEFS) {
                char v[12];
                snprintf(v, sizeof(v), "%lu.%lu", (unsigned long)(r->fs.version >> 16),
                         (unsigned long)(r->fs.version & 0xffff));
                cJSON_AddStringToObject(o, "fs_version", v);
                cJSON_AddNumberToObject(o, "block_size", r->fs.block_size);
                cJSON_AddNumberToObject(o, "block_count", r->fs.block_count);
            } else if (r->fs.kind == DPL_FS_FAT) {
                cJSON_AddNumberToObject(o, "fat_bits", r->fs.fat_bits);
                cJSON_AddNumberToObject(o, "sector_size", r->fs.block_size);
                cJSON_AddStringToObject(o, "label", r->fs.label);
            }
        }
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

static void map_flash(df_t *df, const cJSON *args)
{
    df->nregions = 0;
    cJSON_Delete(df->notes);
    df->notes = cJSON_CreateArray();
    sh_free(&df->sh);
    if (df->plat == PLAT_ESP) {
        map_esp(df);
    } else if (df->plat == PLAT_RP2) {
        map_rp2(df, args);
    } else {
        map_samd(df);
    }
    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "platform", df->plat == PLAT_ESP ? "esp" : df->plat == PLAT_RP2 ? "rp2" : "samd");
    cJSON_AddStringToObject(info, "chip", df->chip ? df->chip : "unknown");
    cJSON_AddNumberToObject(info, "flash_size", df->flash_size);
    cJSON_AddBoolToObject(info, "writable", df->writable);
    rescue_job_result_set(df->job, "device", info);
    rescue_job_result_set(df->job, "regions", regions_json(df));
    rescue_job_result_set(df->job, "notes", cJSON_Duplicate(df->notes, true));
}

/* ===========================================================================
 *  Ops
 * ======================================================================== */

typedef struct {
    int status;
    char err[160];
    cJSON *res;
} op_t;

static int op_fail(op_t *o, int status, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int op_fail(op_t *o, int status, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(o->err, sizeof(o->err), fmt, ap);
    va_end(ap);
    o->status = status;
    return status;
}

/* The op's region: part (name or index), or offset + size. */
static region_t *op_region(df_t *df, const cJSON *a, region_t *tmp, op_t *o)
{
    const cJSON *part = cJSON_GetObjectItemCaseSensitive(a, "part");
    if (cJSON_IsString(part)) {
        for (int i = 0; i < df->nregions; i++) {
            if (strcasecmp(df->regions[i].name, part->valuestring) == 0) {
                return &df->regions[i];
            }
        }
        op_fail(o, 404, "no region named %s", part->valuestring);
        return NULL;
    }
    if (cJSON_IsNumber(part)) {
        const int i = (int)part->valuedouble;
        if (i >= 0 && i < df->nregions) {
            return &df->regions[i];
        }
        op_fail(o, 404, "no region %d", i);
        return NULL;
    }
    uint32_t off, size;
    if (parse_u32(cJSON_GetObjectItemCaseSensitive(a, "offset"), &off) &&
            parse_u32(cJSON_GetObjectItemCaseSensitive(a, "size"), &size)) {
        if (off % DF_UNIT || size == 0 || size % DF_UNIT || off > df->flash_size || size > df->flash_size - off) {
            op_fail(o, 400, "offset and size: 4 KB aligned, inside the %lu-byte flash", (unsigned long)df->flash_size);
            return NULL;
        }
        memset(tmp, 0, sizeof(*tmp));
        snprintf(tmp->name, sizeof(tmp->name), "0x%lx", (unsigned long)off);
        strlcpy(tmp->type, "custom", sizeof(tmp->type));
        tmp->offset = off;
        tmp->size = size;
        tmp->probe = true;
        /* Inside a protected region of the map is protected too. */
        for (int i = 0; i < df->nregions; i++) {
            const region_t *r = &df->regions[i];
            if (r->protect && strcmp(r->type, "flash") != 0 && off < r->offset + r->size && r->offset < off + size) {
                tmp->protect = true;
            }
        }
        return tmp;
    }
    op_fail(o, 400, "part (a region name or index) or offset + size required");
    return NULL;
}

static void staged_name(df_t *df, const cJSON *a, const char *what, char *name, size_t len)
{
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(a, "file");
    if (cJSON_IsString(f)) {
        strlcpy(name, f->valuestring, len);
        return;
    }
    snprintf(name, len, "%s-%s", df->busid, what);
    for (char *p = name; *p; p++) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '.' || *p == '-' || *p == '_')) {
            *p = '_';
        }
    }
}

static void add_download(cJSON *res, const char *name, size_t bytes)
{
    char url[96];
    snprintf(url, sizeof(url), "/api/rescue/files/%s", name);
    cJSON_AddStringToObject(res, "file", name);
    cJSON_AddNumberToObject(res, "bytes", (double)bytes);
    cJSON_AddStringToObject(res, "download", url);
}

static int op_read_region(df_t *df, const cJSON *a, op_t *o)
{
    region_t tmp;
    region_t *r = op_region(df, a, &tmp, o);
    if (r == NULL) {
        return o->status;
    }
    char base[40], name[BLOB_NAME_MAX];
    snprintf(base, sizeof(base), "%s.bin", r->name);
    staged_name(df, a, base, name, sizeof(name));
    if (!blob_name_valid(name)) {
        return op_fail(o, 400, "file: a staged name ([A-Za-z0-9._-])");
    }
    uint8_t *buf = NULL;
    const esp_err_t err = blob_begin(name, r->size, &buf);
    if (err != ESP_OK) {
        return op_fail(o, err == ESP_ERR_NO_MEM ? 507 : 409, "cannot stage %lu bytes as %s (%s)",
                       (unsigned long)r->size, name, esp_err_to_name(err));
    }
    if (sh_flush(&df->sh) != 0) {
        blob_abort(name);
        return op_fail(o, 502, "writing pending changes failed");
    }
    const int64_t t0 = esp_timer_get_time();
    for (uint32_t pos = 0; pos < r->size; pos += DF_READ_CHUNK) {
        if (rescue_job_cancelled(df->job)) {
            blob_abort(name);
            return op_fail(o, 499, "cancelled");
        }
        const uint32_t n = r->size - pos < DF_READ_CHUNK ? r->size - pos : DF_READ_CHUNK;
        if (dev_read(df, r->offset + pos, buf + pos, n) != 0) {
            blob_abort(name);
            return op_fail(o, 502, "reading 0x%lx failed", (unsigned long)(r->offset + pos));
        }
        char stage[48];
        snprintf(stage, sizeof(stage), "reading %s", r->name);
        rescue_job_progress(df->job, (int)(100ull * (pos + n) / r->size), stage);
    }
    blob_commit(name, r->size);
    const double secs = (double)(esp_timer_get_time() - t0) / 1e6;
    rescue_job_log(df->job, "%s: %lu bytes at 0x%lx staged as %s in %.1f s", r->name, (unsigned long)r->size,
                   (unsigned long)r->offset, name, secs);
    cJSON_AddStringToObject(o->res, "part", r->name);
    cJSON_AddNumberToObject(o->res, "offset", r->offset);
    add_download(o->res, name, r->size);
    cJSON_AddNumberToObject(o->res, "seconds", secs);
    return 0;
}

/* Detect the file system through the shadow, so its blocks stay cached. */
static int fs_detect(df_t *df, region_t *r, op_t *o)
{
    if (sh_init(&df->sh, df, r->offset, r->size) != 0) {
        return op_fail(o, 503, "out of memory");
    }
    uint8_t *b = heap_caps_malloc(3 * DF_UNIT, MALLOC_CAP_SPIRAM);
    if (b == NULL) {
        return op_fail(o, 503, "out of memory");
    }
    const bool two = r->size >= 2 * DF_UNIT, three = r->size >= 3 * DF_UNIT;
    int rc = bd_read(&df->sh, 0, b, DF_UNIT);
    if (rc == 0 && two) {
        rc = bd_read(&df->sh, DF_UNIT, b + DF_UNIT, DF_UNIT);
    }
    if (rc == 0 && three) {
        rc = bd_read(&df->sh, (r->size / DF_UNIT - 1) * DF_UNIT, b + 2 * DF_UNIT, DF_UNIT);
    }
    if (rc == 0) {
        dpl_detect(b, two ? b + DF_UNIT : NULL, three ? b + 2 * DF_UNIT : NULL, DF_UNIT, r->offset, r->size, &r->fs);
        r->probed = true;
    }
    heap_caps_free(b);
    return rc == 0 ? 0 : op_fail(o, 502, "reading the region failed");
}

static int fs_mount(df_t *df, const cJSON *a, region_t *tmp, op_t *o, region_t **rp, dut_fs_t **fs, dut_bd_t *bd)
{
    region_t *r = op_region(df, a, tmp, o);
    if (r == NULL) {
        return o->status;
    }
    if (df->encrypted) {
        return op_fail(o, 409, "flash encryption is on: the bootloader reads ciphertext");
    }
    if (fs_detect(df, r, o) != 0) {
        return o->status;
    }
    *bd = (dut_bd_t){ .ctx = &df->sh, .size = r->size, .read = bd_read, .write = bd_write,
                      .program = bd_program, .erase = bd_erase };
    const uint32_t prog = dpl_lfs_prog_size(r->fs.commit_align, df->lfs_prog_default);
    const int st = dut_fs_mount(bd, &r->fs, prog, fs, o->err, sizeof(o->err));
    if (st != 0) {
        o->status = st;
        return st;
    }
    *rp = r;
    cJSON_AddStringToObject(o->res, "part", r->name);
    cJSON_AddStringToObject(o->res, "fs", dpl_fs_name(r->fs.kind));
    return 0;
}

static const char *str_arg(const cJSON *a, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(a, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static bool is_write_op(const char *op)
{
    return strcmp(op, "write") == 0 || strcmp(op, "delete") == 0 || strcmp(op, "rename") == 0 ||
           strcmp(op, "mkdir") == 0 || strcmp(op, "format") == 0;
}

static int op_fs(df_t *df, const char *op, const cJSON *a, op_t *o)
{
    const bool writes = is_write_op(op);
    if (writes && !df->writable) {
        return op_fail(o, 501, "this bootloader is read-only here (%s)", df->plat == PLAT_SAMD ? "SAM-BA" : "?");
    }
    region_t tmp, *r = NULL;
    dut_fs_t *fs = NULL;
    dut_bd_t bd;
    if (fs_mount(df, a, &tmp, o, &r, &fs, &bd) != 0) {
        return o->status;
    }
    df->sh.may_flush = writes;
    if (writes && r->protect && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(a, "force"))) {
        dut_fs_unmount(fs);
        return op_fail(o, 409, "%s holds code or system data (force: true to write anyway)", r->name);
    }
    const char *path = str_arg(a, "path");
    int st = 0;
    if (strcmp(op, "fs_info") == 0 || strcmp(op, "list") == 0) {
        if (strcmp(op, "fs_info") == 0) {
            double total, free_bytes;
            dut_fs_usage(fs, &total, &free_bytes);
            cJSON_AddNumberToObject(o->res, "total", total);
            cJSON_AddNumberToObject(o->res, "free", free_bytes);
            cJSON_AddNumberToObject(o->res, "block_size", r->fs.block_size);
            if (r->fs.kind == DPL_FS_LITTLEFS) {
                char v[12];
                snprintf(v, sizeof(v), "%lu.%lu", (unsigned long)(r->fs.version >> 16), (unsigned long)(r->fs.version & 0xffff));
                cJSON_AddStringToObject(o->res, "fs_version", v);
                cJSON_AddNumberToObject(o->res, "prog_size", dpl_lfs_prog_size(r->fs.commit_align, df->lfs_prog_default));
            } else if (r->fs.kind == DPL_FS_FAT) {
                cJSON_AddNumberToObject(o->res, "fat_bits", r->fs.fat_bits);
                cJSON_AddStringToObject(o->res, "label", r->fs.label);
            }
            path = path ? path : "/";
        }
        cJSON *files = dut_fs_list(fs, path ? path : "/", &st, o->err, sizeof(o->err));
        if (files != NULL) {
            cJSON_AddStringToObject(o->res, "path", path ? path : "/");
            cJSON_AddItemToObject(o->res, "files", files);
        }
    } else if (strcmp(op, "read") == 0) {
        size_t size = 0;
        bool dir = false;
        char name[BLOB_NAME_MAX];
        const char *base = path ? strrchr(path, '/') : NULL;
        staged_name(df, a, base && base[1] ? base + 1 : "file", name, sizeof(name));
        st = dut_fs_stat(fs, path, &size, &dir, o->err, sizeof(o->err));
        if (st == 0 && dir) {
            st = op_fail(o, 409, "%s is a directory", path);
        } else if (st == 0 && !blob_name_valid(name)) {
            st = op_fail(o, 400, "file: a staged name ([A-Za-z0-9._-])");
        }
        uint8_t *buf = NULL;
        if (st == 0) {
            const esp_err_t err = blob_begin(name, size ? size : 1, &buf);
            if (err != ESP_OK) {
                st = op_fail(o, 507, "cannot stage %u bytes as %s (%s)", (unsigned)size, name, esp_err_to_name(err));
            }
        }
        if (st == 0) {
            st = dut_fs_read(fs, path, buf, size, o->err, sizeof(o->err));
            if (st == 0) {
                blob_commit(name, size);
                cJSON_AddStringToObject(o->res, "path", path);
                add_download(o->res, name, size);
            } else {
                blob_abort(name);
            }
        }
    } else if (strcmp(op, "write") == 0) {
        const char *file = str_arg(a, "file"), *text = str_arg(a, "text");
        const uint8_t *data = NULL;
        size_t len = 0;
        if (text != NULL) {
            data = (const uint8_t *)text;
            len = strlen(text);
        } else if (file == NULL || !blob_acquire(file, &data, &len)) {
            st = op_fail(o, file ? 404 : 400, file ? "no staged file named %s" : "file (a staged name) or text required",
                         file ? file : "");
        }
        if (st == 0) {
            st = dut_fs_write(fs, path, data, len, o->err, sizeof(o->err));
            if (file != NULL && text == NULL) {
                blob_release(file);
            }
            cJSON_AddStringToObject(o->res, "path", path ? path : "");
            cJSON_AddNumberToObject(o->res, "bytes", (double)len);
        }
    } else if (strcmp(op, "delete") == 0) {
        st = dut_fs_remove(fs, path, o->err, sizeof(o->err));
    } else if (strcmp(op, "mkdir") == 0) {
        st = dut_fs_mkdir(fs, path, o->err, sizeof(o->err));
    } else if (strcmp(op, "rename") == 0) {
        st = dut_fs_rename(fs, str_arg(a, "from"), str_arg(a, "to"), o->err, sizeof(o->err));
    }
    dut_fs_unmount(fs);
    o->status = st;
    if (writes) {
        const uint32_t before = df->sectors_written;
        if (sh_flush(&df->sh) != 0) {
            sh_free(&df->sh);                           /* the flash is what it is now: re-read it */
            if (st == 0) {
                st = op_fail(o, 502, "writing the changed sectors to the flash failed");
            }
        }
        cJSON_AddNumberToObject(o->res, "sectors_written", df->sectors_written - before);
    } else if (sh_any_dirty(&df->sh)) {
        sh_free(&df->sh);                               /* never write back from a read-only op */
    }
    return st;
}

static int op_format(df_t *df, const cJSON *a, op_t *o)
{
    if (!df->writable) {
        return op_fail(o, 501, "this bootloader is read-only here");
    }
    region_t tmp;
    region_t *r = op_region(df, a, &tmp, o);
    if (r == NULL) {
        return o->status;
    }
    const char *kind = str_arg(a, "fs");
    dut_fs_format_t how = { .lfs_prog = df->lfs_prog_default, .lfs_version = 0x00020000,
                            .lfs_name_max = df->plat == PLAT_ESP ? 64 : 255, .fat_sector = 512 };
    if (kind && strcmp(kind, "littlefs") == 0) {
        how.kind = DPL_FS_LITTLEFS;
    } else if (kind && strcmp(kind, "fat") == 0) {
        how.kind = DPL_FS_FAT;
        uint32_t ss;
        if (parse_u32(cJSON_GetObjectItemCaseSensitive(a, "sector_size"), &ss)) {
            how.fat_sector = ss;
        }
        if (how.fat_sector != 512 && how.fat_sector != 4096) {
            return op_fail(o, 400, "sector_size: 512 or 4096");
        }
    } else if (kind && strcmp(kind, "spiffs") == 0) {
        how.kind = DPL_FS_SPIFFS;
    } else {
        return op_fail(o, 400, "fs: littlefs, fat or spiffs");
    }
    if (r->protect && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(a, "force"))) {
        return op_fail(o, 409, "%s holds code or system data (force: true to format anyway)", r->name);
    }
    if (r->size % DF_UNIT || r->size < 4 * DF_UNIT) {
        return op_fail(o, 400, "the region must be 4 KB aligned and at least 16 KB");
    }
    if (fs_detect(df, r, o) != 0) {
        return o->status;
    }
    df->sh.may_flush = true;
    if (r->fs.kind != DPL_FS_BLANK && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(a, "confirm"))) {
        return op_fail(o, 409, "%s holds %s, not blank: confirm: true to erase it", r->name, dpl_fs_name(r->fs.kind));
    }
    cJSON_AddStringToObject(o->res, "part", r->name);
    cJSON_AddStringToObject(o->res, "was", dpl_fs_name(r->fs.kind));
    const dut_bd_t bd = { .ctx = &df->sh, .size = r->size, .read = bd_read, .write = bd_write,
                          .program = bd_program, .erase = bd_erase };
    /* FAT: an old LittleFS or SPIFFS superblock would win detection, so
       the first blocks start blank. */
    if (how.kind == DPL_FS_FAT && bd_erase(&df->sh, 0, 2 * DF_UNIT) != 0) {
        return op_fail(o, 502, "flash read failed");
    }
    int st = dut_fs_format(&bd, &how, o->err, sizeof(o->err));
    const uint32_t before = df->sectors_written;
    if (st == 0 && sh_flush(&df->sh) != 0) {
        st = op_fail(o, 502, "writing the new file system to the flash failed");
    }
    cJSON_AddNumberToObject(o->res, "sectors_written", df->sectors_written - before);
    sh_free(&df->sh);
    r->probed = false;
    if (st == 0) {
        probe(df, r);
        cJSON_AddStringToObject(o->res, "fs", dpl_fs_name(r->fs.kind));
        rescue_job_log(df->job, "%s formatted as %s", r->name, dpl_fs_name(r->fs.kind));
    } else {
        o->status = st;
    }
    return st;
}

static void run_op(df_t *df, const cJSON *a, int index)
{
    op_t o = { .res = cJSON_CreateObject() };
    const char *op = str_arg(a, "op");
    const int64_t t0 = esp_timer_get_time();
    cJSON_AddNumberToObject(o.res, "index", index);
    cJSON_AddStringToObject(o.res, "op", op ? op : "");
    char stage[48];
    snprintf(stage, sizeof(stage), "op %d: %s", index, op ? op : "?");
    rescue_job_progress(df->job, -1, stage);
    int st;
    if (op == NULL) {
        st = op_fail(&o, 400, "op required");
    } else if (strcmp(op, "partitions") == 0) {
        map_flash(df, a);
        st = 0;
    } else if (strcmp(op, "read_region") == 0) {
        st = op_read_region(df, a, &o);
    } else if (strcmp(op, "format") == 0) {
        st = op_format(df, a, &o);
    } else if (strcmp(op, "fs_info") == 0 || strcmp(op, "list") == 0 || strcmp(op, "read") == 0 ||
               strcmp(op, "write") == 0 || strcmp(op, "delete") == 0 || strcmp(op, "rename") == 0 ||
               strcmp(op, "mkdir") == 0) {
        st = op_fs(df, op, a, &o);
    } else {
        st = op_fail(&o, 400, "unknown op %s", op);
    }
    cJSON_AddBoolToObject(o.res, "ok", st == 0);
    cJSON_AddNumberToObject(o.res, "status", st == 0 ? 200 : st);
    if (st != 0) {
        cJSON_AddStringToObject(o.res, "error", o.err);
        rescue_job_log(df->job, "op %d %s: %s", index, op ? op : "?", o.err);
    } else {
        rescue_job_log(df->job, "op %d %s: ok", index, op);
    }
    cJSON_AddNumberToObject(o.res, "ms", (double)((esp_timer_get_time() - t0) / 1000));
    rescue_job_result_append(df->job, "ops", o.res, DF_OPS_KEEP);
    if (strcmp(op ? op : "", "format") == 0 || strcmp(op ? op : "", "partitions") == 0) {
        rescue_job_result_set(df->job, "regions", regions_json(df));
    }
}

/* ===========================================================================
 *  The session job
 * ======================================================================== */

int dut_flash_job(rescue_job_t *job, const char *busid, const cJSON *args)
{
    lock();
    if (!s_sess.active) {                       /* started as a plain job (MCP) */
        s_sess.active = true;
        strlcpy(s_sess.busid, busid, sizeof(s_sess.busid));
        s_sess.close = false;
    }
    s_sess.accepting = true;
    s_sess.job_id = rescue_job_id(job);
    unlock();

    df_t *df = heap_caps_calloc(1, sizeof(*df), MALLOC_CAP_DEFAULT);
    int rc = 0;
    if (df == NULL) {
        rc = rescue_job_fail(job, "out of memory");
    } else {
        df->job = job;
        strlcpy(df->busid, busid, sizeof(df->busid));
        df->notes = cJSON_CreateArray();
        rescue_job_progress(job, 2, "connecting");
        rc = dev_open(df, args);
    }
    if (rc == 0) {
        rescue_job_progress(job, 5, "mapping the flash");
        map_flash(df, args);
        int index = 0;
        const cJSON *ops = cJSON_GetObjectItemCaseSensitive(args, "ops");
        const cJSON *a;
        cJSON_ArrayForEach(a, ops) {
            if (rescue_job_cancelled(job)) {
                break;
            }
            run_op(df, a, index++);
        }
        int keep = 0;
        const cJSON *k = cJSON_GetObjectItemCaseSensitive(args, "keep_open_s");
        if (cJSON_IsNumber(k)) {
            keep = k->valuedouble < 0 ? 0 : k->valuedouble > DF_KEEP_OPEN_MAX ? DF_KEEP_OPEN_MAX : (int)k->valuedouble;
        }
        int64_t last = esp_timer_get_time();
        while (!rescue_job_cancelled(job)) {
            lock();
            cJSON *more = s_sess.pending;
            s_sess.pending = NULL;
            const bool idle = (esp_timer_get_time() - last) / 1000000 >= keep;
            const bool stop = more == NULL && (s_sess.close || idle);
            if (stop) {
                s_sess.accepting = false;               /* in the same lock as the empty check */
            }
            unlock();
            if (stop) {
                break;
            }
            if (more != NULL) {
                cJSON_ArrayForEach(a, more) {
                    run_op(df, a, index++);
                }
                cJSON_Delete(more);
                last = esp_timer_get_time();
                continue;
            }
            char stage[48];
            snprintf(stage, sizeof(stage), "ready (closes after %d s idle)", keep);
            rescue_job_progress(job, 50, stage);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (sh_flush(&df->sh) != 0) {
            rc = rescue_job_fail(job, "writing pending changes failed");
        }
        rescue_job_log(job, "%lu sectors written this session", (unsigned long)df->sectors_written);
    }
    if (df != NULL) {
        sh_free(&df->sh);
        dev_close(df, args);
        cJSON_Delete(df->notes);
        heap_caps_free(df);
    }
    lock();
    cJSON_Delete(s_sess.pending);
    memset(&s_sess, 0, sizeof(s_sess));
    unlock();
    return rc;
}

/* ===========================================================================
 *  HTTP side
 * ======================================================================== */

static int fail(cJSON **out, int status, const char *msg)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", false);
    cJSON_AddStringToObject(j, "error", msg);
    *out = j;
    return status;
}

static cJSON *accepted(uint32_t id, int queued, bool added)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddNumberToObject(j, "job", id);
    cJSON_AddBoolToObject(j, "added_to_open_session", added);
    cJSON_AddNumberToObject(j, "queued_ops", queued);
    char poll[48];
    snprintf(poll, sizeof(poll), "/api/rescue/jobs/%lu", (unsigned long)id);
    cJSON_AddStringToObject(j, "poll", poll);
    return j;
}

int dut_flash_request(const char *busid, const cJSON *args, cJSON **out)
{
    if (busid == NULL || busid[0] == '\0' || strlen(busid) >= sizeof(s_sess.busid)) {
        return fail(out, 400, "busid required");
    }
    const cJSON *ops = cJSON_GetObjectItemCaseSensitive(args, "ops");
    if (ops != NULL && !cJSON_IsArray(ops)) {
        return fail(out, 400, "ops: an array of {op, ...}");
    }
    const int n = ops ? cJSON_GetArraySize(ops) : 0;
    lock();
    if (s_sess.active && s_sess.accepting && strcmp(s_sess.busid, busid) == 0) {
        if (s_sess.pending == NULL) {
            s_sess.pending = cJSON_CreateArray();
        }
        const cJSON *a;
        cJSON_ArrayForEach(a, ops) {
            cJSON_AddItemToArray(s_sess.pending, cJSON_Duplicate(a, true));
        }
        const uint32_t id = s_sess.job_id;
        unlock();
        *out = accepted(id, n, true);
        return 202;
    }
    if (s_sess.active) {
        char msg[80];
        snprintf(msg, sizeof(msg), "a DUT flash session is open on %s", s_sess.busid);
        unlock();
        return fail(out, 409, msg);
    }
    s_sess.active = true;
    s_sess.accepting = false;
    s_sess.close = false;
    strlcpy(s_sess.busid, busid, sizeof(s_sess.busid));
    unlock();
    int status = 0;
    char msg[128] = "";
    const uint32_t id = rescue_job_start("dut_flash", busid, args, dut_flash_job, &status, msg, sizeof(msg));
    if (id == 0) {
        lock();
        if (s_sess.job_id == 0) {
            memset(&s_sess, 0, sizeof(s_sess));
        }
        unlock();
        return fail(out, status, msg);
    }
    *out = accepted(id, n, false);
    return 202;
}

int dut_flash_status(const char *busid, cJSON **out)
{
    cJSON *j = cJSON_CreateObject();
    lock();
    const bool open = s_sess.active && busid != NULL && strcmp(s_sess.busid, busid) == 0;
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddBoolToObject(j, "open", open);
    if (open && s_sess.job_id) {
        cJSON_AddNumberToObject(j, "job", s_sess.job_id);
        cJSON_AddBoolToObject(j, "accepting", s_sess.accepting);
    }
    unlock();
    *out = j;
    return 200;
}

int dut_flash_close(const char *busid, cJSON **out)
{
    lock();
    const bool open = s_sess.active && busid != NULL && strcmp(s_sess.busid, busid) == 0;
    const uint32_t id = s_sess.job_id;
    if (open) {
        s_sess.close = true;
    }
    unlock();
    if (!open) {
        return fail(out, 404, "no DUT flash session on this device");
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddNumberToObject(j, "job", id);
    *out = j;
    return 200;
}
