#include "dut_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diskio_impl.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lfs.h"
#include "spiffs.h"
#include "spiffs_api.h"
#include "spiffs_nucleus.h"

#define FS_PATH_MAX     256
#define LIST_MAX        512
#define LFS_LOOKAHEAD   128
#define SPIFFS_FDS      8

typedef struct dut_fs dut_fs_t;

/* SPIFFS user_data: SPIFFS_LOCK casts it to esp_spiffs_t and takes .lock. */
typedef struct {
    esp_spiffs_t esp;
    dut_fs_t *self;
} sp_ctx_t;

struct dut_fs {
    dpl_fs_kind_t kind;
    dut_bd_t bd;
    /* FAT */
    FATFS *fat;
    char drive[4];
    /* LittleFS */
    lfs_t *lfs;
    struct lfs_config lcfg;
    /* SPIFFS */
    sp_ctx_t *sp;
    spiffs *sfs;
    spiffs_config scfg;
    uint8_t *swork, *sfds, *scache;
};

static int set_err(char *err, size_t err_len, int status, const char *msg)
{
    snprintf(err, err_len, "%s", msg);
    return status;
}

static bool path_ok(const char *path)
{
    return path != NULL && path[0] == '/' && strlen(path) < FS_PATH_MAX - 4;
}

/* ===========================================================================
 *  FAT: a FatFS drive on the region (one at a time: jobs run one at a time)
 * ======================================================================== */

static BYTE s_pdrv = 0xFF;
static const dut_bd_t *s_fat_bd;
static uint32_t s_fat_ss;

static DSTATUS fat_init(BYTE pdrv)
{
    (void)pdrv;
    return s_fat_bd ? 0 : STA_NOINIT;
}

static DRESULT fat_read(BYTE pdrv, BYTE *buff, uint32_t sector, UINT count)
{
    (void)pdrv;
    if (s_fat_bd == NULL || ((uint64_t)sector + count) * s_fat_ss > s_fat_bd->size) {
        return RES_PARERR;
    }
    return s_fat_bd->read(s_fat_bd->ctx, sector * s_fat_ss, buff, count * s_fat_ss) == 0 ? RES_OK : RES_ERROR;
}

static DRESULT fat_write(BYTE pdrv, const BYTE *buff, uint32_t sector, UINT count)
{
    (void)pdrv;
    if (s_fat_bd == NULL || ((uint64_t)sector + count) * s_fat_ss > s_fat_bd->size) {
        return RES_PARERR;
    }
    return s_fat_bd->write(s_fat_bd->ctx, sector * s_fat_ss, buff, count * s_fat_ss) == 0 ? RES_OK : RES_ERROR;
}

static DRESULT fat_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    (void)pdrv;
    if (s_fat_bd == NULL) {
        return RES_NOTRDY;
    }
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = s_fat_bd->size / s_fat_ss;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = (WORD)s_fat_ss;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = s_fat_ss < 4096 ? 4096 / s_fat_ss : 1;     /* erase unit, for f_mkfs alignment */
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

static const ff_diskio_impl_t k_fat_diskio = {
    .init = fat_init, .status = fat_init, .read = fat_read, .write = fat_write, .ioctl = fat_ioctl,
};

/* The drive stays registered (and NOINIT while nothing is mounted), so the
   mass-storage explorer's drive is never the same one. */
static int fat_attach(const dut_bd_t *bd, uint32_t ss, char drive[4], char *err, size_t err_len)
{
    if (s_pdrv == 0xFF) {
        BYTE pdrv;
        if (ff_diskio_get_drive(&pdrv) != ESP_OK) {
            return set_err(err, err_len, 503, "no free FatFS drive");
        }
        s_pdrv = pdrv;
    }
    s_fat_bd = bd;
    s_fat_ss = ss;
    ff_diskio_register(s_pdrv, &k_fat_diskio);
    drive[0] = (char)('0' + (s_pdrv % 10));
    drive[1] = ':';
    drive[2] = '\0';
    return 0;
}

static void fat_detach(void)
{
    s_fat_bd = NULL;
}

static int fat_status(FRESULT fr, char *err, size_t err_len)
{
    switch (fr) {
    case FR_OK: return 0;
    case FR_NO_FILE: return set_err(err, err_len, 404, "no such file");
    case FR_NO_PATH: return set_err(err, err_len, 404, "no such directory");
    case FR_INVALID_NAME: return set_err(err, err_len, 400, "invalid name");
    case FR_EXIST: return set_err(err, err_len, 409, "already exists");
    case FR_DENIED: return set_err(err, err_len, 409, "denied (full, read-only, or a directory that is not empty)");
    case FR_NO_FILESYSTEM: return set_err(err, err_len, 409, "no FAT file system");
    case FR_DISK_ERR: return set_err(err, err_len, 502, "flash read/write error");
    default:
        snprintf(err, err_len, "FatFS error %d", (int)fr);
        return 502;
    }
}

static bool fat_path(const dut_fs_t *fs, const char *path, char *out, size_t out_len)
{
    if (!path_ok(path)) {
        return false;
    }
    snprintf(out, out_len, "%s%s", fs->drive, path);
    return true;
}

/* ===========================================================================
 *  LittleFS
 * ======================================================================== */

static int lfs_bd_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buf, lfs_size_t size)
{
    const dut_bd_t *bd = c->context;
    const uint64_t at = (uint64_t)block * c->block_size + off;
    if (at + size > bd->size) {
        return LFS_ERR_IO;
    }
    return bd->read(bd->ctx, (uint32_t)at, buf, size) == 0 ? 0 : LFS_ERR_IO;
}

static int lfs_bd_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buf, lfs_size_t size)
{
    const dut_bd_t *bd = c->context;
    const uint64_t at = (uint64_t)block * c->block_size + off;
    if (at + size > bd->size) {
        return LFS_ERR_IO;
    }
    return bd->program(bd->ctx, (uint32_t)at, buf, size) == 0 ? 0 : LFS_ERR_IO;
}

static int lfs_bd_erase(const struct lfs_config *c, lfs_block_t block)
{
    const dut_bd_t *bd = c->context;
    const uint64_t at = (uint64_t)block * c->block_size;
    if (at + c->block_size > bd->size) {
        return LFS_ERR_IO;
    }
    return bd->erase(bd->ctx, (uint32_t)at, c->block_size) == 0 ? 0 : LFS_ERR_IO;
}

static int lfs_bd_sync(const struct lfs_config *c)
{
    (void)c;
    return 0;
}

static void lfs_config_init(struct lfs_config *c, const dut_bd_t *bd, uint32_t block_size, uint32_t block_count,
                            uint32_t prog, uint32_t version, uint32_t name_max)
{
    memset(c, 0, sizeof(*c));
    c->context = (void *)bd;
    c->read = lfs_bd_read;
    c->prog = lfs_bd_prog;
    c->erase = lfs_bd_erase;
    c->sync = lfs_bd_sync;
    if (prog < 16 || prog > block_size || (prog & (prog - 1))) {
        prog = 128;
    }
    c->read_size = prog;
    c->prog_size = prog;
    c->block_size = block_size;
    c->block_count = block_count;
    c->block_cycles = 500;
    c->cache_size = prog > 512 ? prog : (block_size % 512 == 0 ? 512 : prog);
    c->lookahead_size = LFS_LOOKAHEAD;
    c->name_max = name_max;
    c->disk_version = version;
}

static int lfs_status(int e, char *err, size_t err_len)
{
    switch (e) {
    case LFS_ERR_OK: return 0;
    case LFS_ERR_NOENT: return set_err(err, err_len, 404, "no such file or directory");
    case LFS_ERR_EXIST: return set_err(err, err_len, 409, "already exists");
    case LFS_ERR_NOTEMPTY: return set_err(err, err_len, 409, "directory not empty");
    case LFS_ERR_ISDIR: return set_err(err, err_len, 409, "is a directory");
    case LFS_ERR_NOTDIR: return set_err(err, err_len, 409, "not a directory");
    case LFS_ERR_NOSPC: return set_err(err, err_len, 507, "no space left on the file system");
    case LFS_ERR_NAMETOOLONG: return set_err(err, err_len, 400, "name too long");
    case LFS_ERR_CORRUPT: return set_err(err, err_len, 502, "LittleFS corrupt");
    case LFS_ERR_IO: return set_err(err, err_len, 502, "flash read/write error");
    case LFS_ERR_NOMEM: return set_err(err, err_len, 503, "out of memory");
    default:
        snprintf(err, err_len, "LittleFS error %d", e);
        return 502;
    }
}

/* ===========================================================================
 *  SPIFFS (IDF core; its SPIFFS_LOCK takes esp_spiffs_t.lock from user_data)
 * ======================================================================== */

static dut_fs_t *spiffs_self(spiffs *fs)
{
    return ((sp_ctx_t *)fs->user_data)->self;
}

static s32_t sp_read(spiffs *fs, u32_t addr, u32_t size, u8_t *dst)
{
    const dut_bd_t *bd = &spiffs_self(fs)->bd;
    return (addr + size <= bd->size && bd->read(bd->ctx, addr, dst, size) == 0) ? SPIFFS_OK : SPIFFS_ERR_INTERNAL;
}

static s32_t sp_write(spiffs *fs, u32_t addr, u32_t size, u8_t *src)
{
    const dut_bd_t *bd = &spiffs_self(fs)->bd;
    return (addr + size <= bd->size && bd->program(bd->ctx, addr, src, size) == 0) ? SPIFFS_OK : SPIFFS_ERR_INTERNAL;
}

static s32_t sp_erase(spiffs *fs, u32_t addr, u32_t size)
{
    const dut_bd_t *bd = &spiffs_self(fs)->bd;
    return (addr + size <= bd->size && bd->erase(bd->ctx, addr, size) == 0) ? SPIFFS_OK : SPIFFS_ERR_INTERNAL;
}

static int spiffs_status(s32_t e, char *err, size_t err_len)
{
    if (e >= 0) {
        return 0;
    }
    switch (e) {
    case SPIFFS_ERR_NOT_FOUND: return set_err(err, err_len, 404, "no such file");
    case SPIFFS_ERR_FULL: return set_err(err, err_len, 507, "no space left on the file system");
    case SPIFFS_ERR_NAME_TOO_LONG: return set_err(err, err_len, 400, "name too long (SPIFFS: 31 characters)");
    case SPIFFS_ERR_CONFLICTING_NAME: return set_err(err, err_len, 409, "already exists");
    case SPIFFS_ERR_NOT_A_FS: return set_err(err, err_len, 409, "no SPIFFS file system");
    default:
        snprintf(err, err_len, "SPIFFS error %ld", (long)e);
        return 502;
    }
}

static void spiffs_free(dut_fs_t *fs)
{
    if (fs->sp != NULL && fs->sp->esp.lock != NULL) {
        vSemaphoreDelete(fs->sp->esp.lock);
    }
    free(fs->sp);
    free(fs->sfs);
    free(fs->swork);
    free(fs->sfds);
    free(fs->scache);
    fs->sp = NULL;
    fs->sfs = NULL;
    fs->swork = fs->sfds = fs->scache = NULL;
}

static int spiffs_setup(dut_fs_t *fs, char *err, size_t err_len)
{
    fs->sp = calloc(1, sizeof(*fs->sp));
    fs->sfs = calloc(1, sizeof(spiffs));
    const uint32_t page = 256;
    fs->swork = malloc(2 * page);
    const uint32_t fds_sz = SPIFFS_FDS * sizeof(spiffs_fd);
    fs->sfds = malloc(fds_sz);
    const uint32_t cache_sz = sizeof(spiffs_cache) + SPIFFS_FDS * (sizeof(spiffs_cache_page) + page);
    fs->scache = malloc(cache_sz);
    if (fs->sp == NULL || fs->sfs == NULL || fs->swork == NULL || fs->sfds == NULL || fs->scache == NULL) {
        spiffs_free(fs);
        return set_err(err, err_len, 503, "out of memory");
    }
    fs->sp->esp.lock = xSemaphoreCreateMutex();
    fs->sp->self = fs;
    fs->sfs->user_data = fs->sp;
    memset(&fs->scfg, 0, sizeof(fs->scfg));
    fs->scfg.hal_read_f = sp_read;
    fs->scfg.hal_write_f = sp_write;
    fs->scfg.hal_erase_f = sp_erase;
    fs->scfg.phys_size = fs->bd.size / 4096 * 4096;
    fs->scfg.phys_addr = 0;
    fs->scfg.phys_erase_block = 4096;
    fs->scfg.log_block_size = 4096;
    fs->scfg.log_page_size = page;
    fs->sp->esp.fds_sz = fds_sz;
    return 0;
}

static s32_t spiffs_do_mount(dut_fs_t *fs)
{
    return SPIFFS_mount(fs->sfs, &fs->scfg, fs->swork, fs->sfds, fs->sp->esp.fds_sz, fs->scache,
                        sizeof(spiffs_cache) + SPIFFS_FDS * (sizeof(spiffs_cache_page) + 256), NULL);
}

/* ===========================================================================
 *  Mount
 * ======================================================================== */

int dut_fs_mount(const dut_bd_t *bd, const dpl_fs_info_t *info, uint32_t lfs_prog, dut_fs_t **out,
                 char *err, size_t err_len)
{
    *out = NULL;
    dut_fs_t *fs = calloc(1, sizeof(*fs));
    if (fs == NULL) {
        return set_err(err, err_len, 503, "out of memory");
    }
    fs->kind = info->kind;
    fs->bd = *bd;
    int st = 0;
    if (info->kind == DPL_FS_FAT) {
        st = fat_attach(&fs->bd, info->block_size, fs->drive, err, err_len);
        fs->fat = st == 0 ? heap_caps_calloc(1, sizeof(FATFS), MALLOC_CAP_DEFAULT) : NULL;
        if (st == 0 && fs->fat == NULL) {
            st = set_err(err, err_len, 503, "out of memory");
        }
        if (st == 0) {
            st = fat_status(f_mount(fs->fat, fs->drive, 1), err, err_len);
        }
        if (st != 0) {
            if (fs->fat != NULL) {
                f_mount(NULL, fs->drive, 0);            /* FatFS keeps the object registered after a failed mount */
            }
            heap_caps_free(fs->fat);
            fat_detach();
        }
    } else if (info->kind == DPL_FS_LITTLEFS) {
        if (info->block_size * info->block_count > bd->size) {
            st = set_err(err, err_len, 409, "the LittleFS superblock says it is larger than the region");
        } else {
            fs->lfs = calloc(1, sizeof(lfs_t));
            lfs_config_init(&fs->lcfg, &fs->bd, info->block_size, info->block_count, lfs_prog, info->version,
                            LFS_NAME_MAX);
            st = fs->lfs ? lfs_status(lfs_mount(fs->lfs, &fs->lcfg), err, err_len)
                         : set_err(err, err_len, 503, "out of memory");
            if (st != 0) {
                free(fs->lfs);
            }
        }
    } else if (info->kind == DPL_FS_SPIFFS) {
        st = spiffs_setup(fs, err, err_len);
        if (st == 0) {
            st = spiffs_status(spiffs_do_mount(fs), err, err_len);
            if (st != 0) {
                spiffs_free(fs);
            }
        }
    } else if (info->kind == DPL_FS_FAT_WL) {
        st = set_err(err, err_len, 501, "FAT behind ESP-IDF wear levelling is not supported yet");
    } else {
        st = set_err(err, err_len, 409, info->kind == DPL_FS_BLANK ? "the region is blank (format it first)"
                                                                    : "no FAT, LittleFS or SPIFFS found in the region");
    }
    if (st != 0) {
        free(fs);
        return st;
    }
    *out = fs;
    return 0;
}

void dut_fs_unmount(dut_fs_t *fs)
{
    if (fs == NULL) {
        return;
    }
    if (fs->kind == DPL_FS_FAT) {
        f_mount(NULL, fs->drive, 0);
        heap_caps_free(fs->fat);
        fat_detach();
    } else if (fs->kind == DPL_FS_LITTLEFS) {
        lfs_unmount(fs->lfs);
        free(fs->lfs);
    } else if (fs->kind == DPL_FS_SPIFFS) {
        SPIFFS_unmount(fs->sfs);
        spiffs_free(fs);
    }
    free(fs);
}

void dut_fs_usage(dut_fs_t *fs, double *total, double *free_bytes)
{
    *total = 0;
    *free_bytes = 0;
    if (fs->kind == DPL_FS_FAT) {
        DWORD nclst = 0;
        FATFS *fsp = NULL;
        if (f_getfree(fs->drive, &nclst, &fsp) == FR_OK && fsp != NULL) {
            *total = (double)(fsp->n_fatent - 2) * fsp->csize * s_fat_ss;
            *free_bytes = (double)nclst * fsp->csize * s_fat_ss;
        }
    } else if (fs->kind == DPL_FS_LITTLEFS) {
        struct lfs_fsinfo fi;
        const lfs_ssize_t used = lfs_fs_size(fs->lfs);
        if (lfs_fs_stat(fs->lfs, &fi) == 0 && used >= 0) {
            *total = (double)fi.block_count * fi.block_size;
            *free_bytes = (double)(fi.block_count - (uint32_t)used) * fi.block_size;
        }
    } else if (fs->kind == DPL_FS_SPIFFS) {
        u32_t t = 0, u = 0;
        if (SPIFFS_info(fs->sfs, &t, &u) == SPIFFS_OK) {
            *total = t;
            *free_bytes = t > u ? t - u : 0;
        }
    }
}

/* ===========================================================================
 *  Operations
 * ======================================================================== */

static void add_entry(cJSON *arr, const char *name, bool dir, double size)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddBoolToObject(o, "dir", dir);
    cJSON_AddNumberToObject(o, "size", size);
    cJSON_AddItemToArray(arr, o);
}

/* SPIFFS has flat names ("/a/b.txt"): list what is under `path`, with the
   next path component of deeper names shown as a directory. */
static void spiffs_list(dut_fs_t *fs, const char *path, cJSON *arr)
{
    char prefix[FS_PATH_MAX];
    snprintf(prefix, sizeof(prefix), "%s%s", path, path[strlen(path) - 1] == '/' ? "" : "/");
    const size_t plen = strlen(prefix);
    spiffs_DIR d;
    struct spiffs_dirent e;
    struct spiffs_dirent *pe;
    int n = 0;
    if (SPIFFS_opendir(fs->sfs, "/", &d) == NULL) {
        return;
    }
    while ((pe = SPIFFS_readdir(&d, &e)) != NULL && n < LIST_MAX) {
        const char *name = (const char *)pe->name;
        if (strncmp(name, prefix, plen) != 0 || name[plen] == '\0') {
            continue;
        }
        const char *rest = name + plen;
        const char *slash = strchr(rest, '/');
        if (slash == NULL) {
            add_entry(arr, rest, false, pe->size);
            n++;
            continue;
        }
        char sub[SPIFFS_OBJ_NAME_LEN];
        const size_t sl = (size_t)(slash - rest) < sizeof(sub) - 1 ? (size_t)(slash - rest) : sizeof(sub) - 1;
        memcpy(sub, rest, sl);
        sub[sl] = '\0';
        bool seen = false;
        const cJSON *it;
        cJSON_ArrayForEach(it, arr) {
            seen |= strcmp(cJSON_GetObjectItemCaseSensitive(it, "name")->valuestring, sub) == 0;
        }
        if (!seen) {
            add_entry(arr, sub, true, 0);
            n++;
        }
    }
    SPIFFS_closedir(&d);
}

cJSON *dut_fs_list(dut_fs_t *fs, const char *path, int *status, char *err, size_t err_len)
{
    if (!path_ok(path)) {
        *status = set_err(err, err_len, 400, "path must start with /");
        return NULL;
    }
    cJSON *arr = cJSON_CreateArray();
    *status = 0;
    if (fs->kind == DPL_FS_FAT) {
        char p[FS_PATH_MAX];
        fat_path(fs, path, p, sizeof(p));
        FF_DIR *dir = calloc(1, sizeof(FF_DIR));
        FILINFO *fi = calloc(1, sizeof(FILINFO));
        FRESULT fr = (dir && fi) ? f_opendir(dir, p) : FR_NOT_ENOUGH_CORE;
        if (fr == FR_OK) {
            int n = 0;
            while (f_readdir(dir, fi) == FR_OK && fi->fname[0] != '\0' && n++ < LIST_MAX) {
                add_entry(arr, fi->fname, (fi->fattrib & AM_DIR) != 0, (double)fi->fsize);
            }
            f_closedir(dir);
        } else {
            *status = fat_status(fr, err, err_len);
        }
        free(dir);
        free(fi);
    } else if (fs->kind == DPL_FS_LITTLEFS) {
        lfs_dir_t *dir = malloc(sizeof(lfs_dir_t));
        struct lfs_info *info = malloc(sizeof(struct lfs_info));
        int e = (dir && info) ? lfs_dir_open(fs->lfs, dir, path) : LFS_ERR_NOMEM;
        if (e == 0) {
            int n = 0;
            while (lfs_dir_read(fs->lfs, dir, info) > 0 && n < LIST_MAX) {
                if (strcmp(info->name, ".") != 0 && strcmp(info->name, "..") != 0) {
                    add_entry(arr, info->name, info->type == LFS_TYPE_DIR, info->size);
                    n++;
                }
            }
            lfs_dir_close(fs->lfs, dir);
        } else {
            *status = lfs_status(e, err, err_len);
        }
        free(dir);
        free(info);
    } else {
        spiffs_list(fs, path, arr);
    }
    if (*status != 0) {
        cJSON_Delete(arr);
        return NULL;
    }
    return arr;
}

int dut_fs_stat(dut_fs_t *fs, const char *path, size_t *size, bool *dir, char *err, size_t err_len)
{
    if (!path_ok(path)) {
        return set_err(err, err_len, 400, "path must start with /");
    }
    *size = 0;
    *dir = false;
    if (fs->kind == DPL_FS_FAT) {
        char p[FS_PATH_MAX];
        FILINFO fi;
        fat_path(fs, path, p, sizeof(p));
        const FRESULT fr = f_stat(p, &fi);
        if (fr == FR_OK) {
            *size = fi.fsize;
            *dir = (fi.fattrib & AM_DIR) != 0;
        }
        return fat_status(fr, err, err_len);
    }
    if (fs->kind == DPL_FS_LITTLEFS) {
        struct lfs_info *info = malloc(sizeof(*info));
        const int e = info ? lfs_stat(fs->lfs, path, info) : LFS_ERR_NOMEM;
        if (e == 0) {
            *size = info->size;
            *dir = info->type == LFS_TYPE_DIR;
        }
        free(info);
        return lfs_status(e, err, err_len);
    }
    spiffs_stat s;
    const s32_t e = SPIFFS_stat(fs->sfs, path, &s);
    if (e == SPIFFS_OK) {
        *size = s.size;
    }
    return spiffs_status(e, err, err_len);
}

int dut_fs_read(dut_fs_t *fs, const char *path, uint8_t *buf, size_t size, char *err, size_t err_len)
{
    if (fs->kind == DPL_FS_FAT) {
        char p[FS_PATH_MAX];
        FIL *f = heap_caps_calloc(1, sizeof(FIL), MALLOC_CAP_DEFAULT);
        if (f == NULL || !fat_path(fs, path, p, sizeof(p))) {
            heap_caps_free(f);
            return set_err(err, err_len, f ? 400 : 503, f ? "bad path" : "out of memory");
        }
        FRESULT fr = f_open(f, p, FA_READ);
        UINT br = 0;
        if (fr == FR_OK) {
            fr = f_read(f, buf, size, &br);
            f_close(f);
            if (fr == FR_OK && br != size) {
                fr = FR_DISK_ERR;
            }
        }
        heap_caps_free(f);
        return fat_status(fr, err, err_len);
    }
    if (fs->kind == DPL_FS_LITTLEFS) {
        lfs_file_t *f = calloc(1, sizeof(lfs_file_t));
        int e = f ? lfs_file_open(fs->lfs, f, path, LFS_O_RDONLY) : LFS_ERR_NOMEM;
        if (e == 0) {
            const lfs_ssize_t n = lfs_file_read(fs->lfs, f, buf, size);
            e = n < 0 ? (int)n : ((size_t)n == size ? 0 : LFS_ERR_IO);
            lfs_file_close(fs->lfs, f);
        }
        free(f);
        return lfs_status(e, err, err_len);
    }
    const spiffs_file fh = SPIFFS_open(fs->sfs, path, SPIFFS_O_RDONLY, 0);
    if (fh < 0) {
        return spiffs_status(fh, err, err_len);
    }
    const s32_t n = SPIFFS_read(fs->sfs, fh, buf, size);
    SPIFFS_close(fs->sfs, fh);
    return spiffs_status(n < 0 ? n : ((size_t)n == size ? 0 : SPIFFS_ERR_INTERNAL), err, err_len);
}

int dut_fs_write(dut_fs_t *fs, const char *path, const uint8_t *data, size_t len, char *err, size_t err_len)
{
    if (!path_ok(path) || strcmp(path, "/") == 0) {
        return set_err(err, err_len, 400, "path must name a file (/dir/name)");
    }
    if (fs->kind == DPL_FS_FAT) {
        char p[FS_PATH_MAX];
        FIL *f = heap_caps_calloc(1, sizeof(FIL), MALLOC_CAP_DEFAULT);
        if (f == NULL) {
            return set_err(err, err_len, 503, "out of memory");
        }
        fat_path(fs, path, p, sizeof(p));
        FRESULT fr = f_open(f, p, FA_CREATE_ALWAYS | FA_WRITE);
        if (fr == FR_OK) {
            UINT bw = 0;
            fr = f_write(f, data, len, &bw);
            const FRESULT fc = f_close(f);
            if (fr == FR_OK && bw != len) {
                fr = FR_DENIED;                         /* volume full */
            }
            if (fr == FR_OK) {
                fr = fc;
            }
        }
        heap_caps_free(f);
        return fat_status(fr, err, err_len);
    }
    if (fs->kind == DPL_FS_LITTLEFS) {
        lfs_file_t *f = calloc(1, sizeof(lfs_file_t));
        int e = f ? lfs_file_open(fs->lfs, f, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) : LFS_ERR_NOMEM;
        if (e == 0) {
            const lfs_ssize_t n = lfs_file_write(fs->lfs, f, data, len);
            const int ec = lfs_file_close(fs->lfs, f);
            e = n < 0 ? (int)n : ec;
        }
        free(f);
        return lfs_status(e, err, err_len);
    }
    const spiffs_file fh = SPIFFS_open(fs->sfs, path, SPIFFS_O_CREAT | SPIFFS_O_TRUNC | SPIFFS_O_RDWR, 0);
    if (fh < 0) {
        return spiffs_status(fh, err, err_len);
    }
    const s32_t n = SPIFFS_write(fs->sfs, fh, (void *)data, len);
    const s32_t ec = SPIFFS_close(fs->sfs, fh);
    return spiffs_status(n < 0 ? n : ec, err, err_len);
}

int dut_fs_remove(dut_fs_t *fs, const char *path, char *err, size_t err_len)
{
    if (!path_ok(path) || strcmp(path, "/") == 0) {
        return set_err(err, err_len, 400, "not the root");
    }
    if (fs->kind == DPL_FS_FAT) {
        char p[FS_PATH_MAX];
        fat_path(fs, path, p, sizeof(p));
        return fat_status(f_unlink(p), err, err_len);
    }
    if (fs->kind == DPL_FS_LITTLEFS) {
        return lfs_status(lfs_remove(fs->lfs, path), err, err_len);
    }
    return spiffs_status(SPIFFS_remove(fs->sfs, path), err, err_len);
}

int dut_fs_rename(dut_fs_t *fs, const char *from, const char *to, char *err, size_t err_len)
{
    if (!path_ok(from) || !path_ok(to) || strcmp(from, "/") == 0) {
        return set_err(err, err_len, 400, "from and to must be paths (not the root)");
    }
    if (fs->kind == DPL_FS_FAT) {
        char p[FS_PATH_MAX], q[FS_PATH_MAX];
        fat_path(fs, from, p, sizeof(p));
        fat_path(fs, to, q, sizeof(q));
        return fat_status(f_rename(p, q), err, err_len);
    }
    if (fs->kind == DPL_FS_LITTLEFS) {
        return lfs_status(lfs_rename(fs->lfs, from, to), err, err_len);
    }
    return spiffs_status(SPIFFS_rename(fs->sfs, from, to), err, err_len);
}

int dut_fs_mkdir(dut_fs_t *fs, const char *path, char *err, size_t err_len)
{
    if (!path_ok(path) || strcmp(path, "/") == 0) {
        return set_err(err, err_len, 400, "path required");
    }
    if (fs->kind == DPL_FS_FAT) {
        char p[FS_PATH_MAX];
        fat_path(fs, path, p, sizeof(p));
        return fat_status(f_mkdir(p), err, err_len);
    }
    if (fs->kind == DPL_FS_LITTLEFS) {
        return lfs_status(lfs_mkdir(fs->lfs, path), err, err_len);
    }
    return set_err(err, err_len, 501, "SPIFFS has no directories (a name with / in it is a file)");
}

/* ===========================================================================
 *  Format
 * ======================================================================== */

int dut_fs_format(const dut_bd_t *bd, const dut_fs_format_t *how, char *err, size_t err_len)
{
    if (how->kind == DPL_FS_LITTLEFS) {
        lfs_t *lfs = calloc(1, sizeof(lfs_t));
        struct lfs_config *c = calloc(1, sizeof(*c));
        int e = LFS_ERR_NOMEM;
        if (lfs && c) {
            lfs_config_init(c, bd, 4096, bd->size / 4096, how->lfs_prog, how->lfs_version,
                            how->lfs_name_max ? how->lfs_name_max : LFS_NAME_MAX);
            e = lfs_format(lfs, c);
            if (e == 0) {
                e = lfs_mount(lfs, c);              /* prove it mounts */
                if (e == 0) {
                    lfs_unmount(lfs);
                }
            }
        }
        free(lfs);
        free(c);
        return lfs_status(e, err, err_len);
    }
    if (how->kind == DPL_FS_FAT) {
        const uint32_t ss = how->fat_sector ? how->fat_sector : 512;
        char drive[4];
        int st = fat_attach(bd, ss, drive, err, err_len);
        if (st != 0) {
            return st;
        }
        const UINT work_len = 4096;
        void *work = malloc(work_len);
        const MKFS_PARM opt = { .fmt = FM_ANY | FM_SFD, .n_fat = 1, .align = 0, .n_root = 0, .au_size = 0 };
        st = work ? fat_status(f_mkfs(drive, &opt, work, work_len), err, err_len) : set_err(err, err_len, 503, "out of memory");
        free(work);
        fat_detach();
        return st;
    }
    if (how->kind == DPL_FS_SPIFFS) {
        dut_fs_t *fs = calloc(1, sizeof(*fs));
        if (fs == NULL) {
            return set_err(err, err_len, 503, "out of memory");
        }
        fs->bd = *bd;
        int st = spiffs_setup(fs, err, err_len);
        if (st == 0) {
            spiffs_do_mount(fs);                    /* SPIFFS_format wants a configured, unmounted fs */
            SPIFFS_unmount(fs->sfs);
            st = spiffs_status(SPIFFS_format(fs->sfs), err, err_len);
            if (st == 0) {
                st = spiffs_status(spiffs_do_mount(fs), err, err_len);
                SPIFFS_unmount(fs->sfs);
            }
            spiffs_free(fs);
        }
        free(fs);
        return st;
    }
    return set_err(err, err_len, 400, "fs: littlefs, fat or spiffs");
}
