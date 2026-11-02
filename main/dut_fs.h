#ifndef DUT_FS_H
#define DUT_FS_H

/*
 * File systems inside a region of a device's flash, for the DUT flash
 * browser (dut_flash.c): FAT (FatFS), LittleFS (vendored littlefs) and SPIFFS
 * (ESP-IDF's SPIFFS core with the IDF/Arduino defaults).  They run against a
 * block device that dut_flash.c backs with a RAM shadow of the region; what
 * they change reaches the flash when dut_flash.c flushes the shadow.
 *
 * Calls return 0 or an HTTP-like status (400, 404, 409, 501, 502, 507) with
 * a message in err.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "dut_part_logic.h"

/* Offsets are relative to the region. */
typedef struct {
    void *ctx;
    uint32_t size;
    int (*read)(void *ctx, uint32_t off, void *buf, uint32_t len);
    /* Replace bytes (FAT sectors). */
    int (*write)(void *ctx, uint32_t off, const void *buf, uint32_t len);
    /* NOR programming: the result is the AND of old and new bytes. */
    int (*program)(void *ctx, uint32_t off, const void *buf, uint32_t len);
    /* 4 KB units to 0xFF. */
    int (*erase)(void *ctx, uint32_t off, uint32_t len);
} dut_bd_t;

typedef struct dut_fs dut_fs_t;

/* LittleFS mounts with lfs_prog as read/prog size and the on-disk version
   pinned to what the superblock says (no upgrade of a device's FS). */
int dut_fs_mount(const dut_bd_t *bd, const dpl_fs_info_t *info, uint32_t lfs_prog, dut_fs_t **out,
                 char *err, size_t err_len);
void dut_fs_unmount(dut_fs_t *fs);
void dut_fs_usage(dut_fs_t *fs, double *total, double *free_bytes);
/* An array of {name, dir, size}, or NULL with err. */
cJSON *dut_fs_list(dut_fs_t *fs, const char *path, int *status, char *err, size_t err_len);
int dut_fs_stat(dut_fs_t *fs, const char *path, size_t *size, bool *dir, char *err, size_t err_len);
int dut_fs_read(dut_fs_t *fs, const char *path, uint8_t *buf, size_t size, char *err, size_t err_len);
/* Create or replace a file. */
int dut_fs_write(dut_fs_t *fs, const char *path, const uint8_t *data, size_t len, char *err, size_t err_len);
int dut_fs_remove(dut_fs_t *fs, const char *path, char *err, size_t err_len);
int dut_fs_rename(dut_fs_t *fs, const char *from, const char *to, char *err, size_t err_len);
int dut_fs_mkdir(dut_fs_t *fs, const char *path, char *err, size_t err_len);

typedef struct {
    dpl_fs_kind_t kind;         /* FAT, LITTLEFS or SPIFFS */
    uint32_t lfs_prog;          /* LittleFS read/prog size */
    uint32_t lfs_name_max;      /* LittleFS name_max (arduino-esp32: 64) */
    uint32_t lfs_version;       /* on-disk version, 0x00020000 reads everywhere */
    uint32_t fat_sector;        /* FAT sector size (512 as Adafruit_SPIFlash / TinyUSB use) */
} dut_fs_format_t;

int dut_fs_format(const dut_bd_t *bd, const dut_fs_format_t *how, char *err, size_t err_len);

#endif
