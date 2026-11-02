#ifndef DUT_PART_LOGIC_H
#define DUT_PART_LOGIC_H

/*
 * Pure parsing for the DUT partition and file system browser (no ESP-IDF):
 * ESP partition tables, the RP2350 partition table as PICOBOOT GET_INFO
 * returns it, arduino-pico's flash layout words, and detection of FAT,
 * LittleFS, SPIFFS and ESP-IDF wear levelling from a region's first and
 * last blocks.  Detection goes by content, never by label or subtype (an
 * Arduino "spiffs" partition usually holds LittleFS).
 *
 * Host test: tools/dut_part_logic_test.c.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 *  ESP partition table (0x8000, 32-byte entries)
 * ------------------------------------------------------------------------ */

#define DPL_ESP_PT_OFFSET   0x8000u
#define DPL_ESP_PT_SIZE     0xC00u
#define DPL_ESP_PT_MAX      32

typedef struct {
    char label[17];
    uint8_t type, subtype;
    uint32_t offset, size;
    uint32_t flags;             /* bit 0: encrypted, bit 1: readonly */
} dpl_esp_part_t;

/* Entries until the end marker or the MD5 row.  Returns the count, or -1
   when the first entry is not a partition (no table).  *has_md5: the table
   ends with an MD5 row (0xEBEB). */
int dpl_esp_parse_table(const uint8_t *buf, size_t len, dpl_esp_part_t *out, int max, bool *has_md5);
/* "app", "data", "bootloader", "partition_table" or NULL. */
const char *dpl_esp_type_name(uint8_t type);
/* "factory", "ota_0", "nvs", "fat", "spiffs", "littlefs", ... or NULL. */
const char *dpl_esp_subtype_name(uint8_t type, uint8_t subtype, char *buf, size_t buf_len);

/* ---------------------------------------------------------------------------
 *  RP2040 / RP2350
 * ------------------------------------------------------------------------ */

#define DPL_RP_FLASH_BASE   0x10000000u
#define DPL_RP_FLASH_MAX    0x01000000u     /* 16 MB XIP window */
#define DPL_RP_LAYOUT_ADDR  0x10002ff0u     /* arduino-pico: FS_START, FS_END, EEPROM_START, FLASH_LENGTH */

/* arduino-pico links four words at flash + 0x2ff0 (all XIP addresses).
   Returns false when they are not a plausible layout.  fs_start == fs_end
   means no file system.  The flash ends 4 KB (RP2040) or 8 KB (RP2350)
   after eeprom_start (tools/makeboards.py: delta). */
bool dpl_arduino_pico_layout(const uint8_t words[16], bool rp2350, uint32_t *fs_start, uint32_t *fs_end,
                             uint32_t *eeprom_start, uint32_t *flash_size);

/* PICOBOOT GET_INFO (0x8b) for the partition table, bType 2. */
#define DPL_PT_INFO_PT_INFO             0x0001u
#define DPL_PT_INFO_LOCATION_AND_FLAGS  0x0010u
#define DPL_PT_INFO_ID                  0x0020u
#define DPL_PT_INFO_NAME                0x0080u
#define DPL_RP_PART_MAX                 16

typedef struct {
    uint32_t start, end;        /* byte offsets from the flash start, end exclusive */
    uint32_t permissions;       /* bits 26-31 of the location word */
    uint32_t flags;
    bool has_id;
    uint64_t id;
    char name[40];
} dpl_rp_part_t;

/* `w` is the GET_INFO answer (little-endian words already decoded).  The
   bootrom may prefix a word count; both forms are accepted.  Returns the
   number of partitions, -1 when the answer cannot be parsed.  *has_table
   says whether a partition table is present. */
int dpl_rp2350_pt_info(const uint32_t *w, size_t nw, uint32_t requested, bool *has_table,
                       dpl_rp_part_t *out, int max);

/* ---------------------------------------------------------------------------
 *  File systems
 * ------------------------------------------------------------------------ */

typedef enum {
    DPL_FS_UNKNOWN = 0,
    DPL_FS_BLANK,               /* erased (0xFF) */
    DPL_FS_FAT,
    DPL_FS_LITTLEFS,
    DPL_FS_SPIFFS,
    DPL_FS_FAT_WL,              /* FAT behind ESP-IDF wear levelling */
} dpl_fs_kind_t;

typedef struct {
    dpl_fs_kind_t kind;
    uint32_t block_size;        /* LittleFS block, FAT sector, SPIFFS block */
    uint32_t block_count;       /* LittleFS blocks, FAT total sectors, SPIFFS blocks */
    /* LittleFS */
    uint32_t version;           /* on-disk version (0x00020001 = 2.1) */
    uint32_t name_max, file_max, attr_max;
    uint32_t superblock;        /* 0 or 1: the block that held it */
    uint32_t commit_align;      /* the first commit's end: a multiple of the writer's prog_size */
    /* FAT */
    uint8_t fat_bits;           /* 12, 16, 32 */
    uint32_t cluster_size;
    char label[12];
    /* SPIFFS */
    uint32_t page_size;
} dpl_fs_info_t;

const char *dpl_fs_name(dpl_fs_kind_t kind);
bool dpl_blank(const uint8_t *buf, size_t len);
/* LittleFS superblock (metadata block 0 or 1 of the file system). */
bool dpl_lfs_superblock(const uint8_t *block, size_t len, dpl_fs_info_t *out);
/* FAT boot sector (needs 512 bytes). */
bool dpl_fat_bpb(const uint8_t *sector, size_t len, dpl_fs_info_t *out);
/* SPIFFS (IDF defaults: 4 KB blocks, 256-byte pages, 16-bit object ids, magic
   with length): blocks 0 and 1 carry 0x20140529 ^ page ^ (count - bix). */
bool dpl_spiffs(const uint8_t *block0, const uint8_t *block1, size_t len, uint32_t region_size,
                dpl_fs_info_t *out);
/* ESP-IDF wear levelling: its config is in the last sector of the partition. */
bool dpl_esp_wl(const uint8_t *last_sector, size_t len, uint32_t part_offset, uint32_t part_size);

/* Everything above, in order: LittleFS (block 0 or 1), FAT, wear levelling
   (last block), SPIFFS, blank.  block1/last may be NULL.  Returns the kind. */
dpl_fs_kind_t dpl_detect(const uint8_t *block0, const uint8_t *block1, const uint8_t *last, size_t len,
                         uint32_t region_offset, uint32_t region_size, dpl_fs_info_t *out);

/* LittleFS prog_size for writing: the platform's usual value when the first
   commit is aligned to it, otherwise the largest power of two (16..512) the
   commit end is aligned to. */
uint32_t dpl_lfs_prog_size(uint32_t commit_align, uint32_t platform_default);

#endif
