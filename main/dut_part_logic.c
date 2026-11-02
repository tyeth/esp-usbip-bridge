#include "dut_part_logic.h"

#include <stdio.h>
#include <string.h>

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static bool pow2(uint32_t v)
{
    return v != 0 && (v & (v - 1)) == 0;
}

/* ===========================================================================
 *  ESP partition table
 * ======================================================================== */

int dpl_esp_parse_table(const uint8_t *buf, size_t len, dpl_esp_part_t *out, int max, bool *has_md5)
{
    int n = 0;
    *has_md5 = false;
    for (size_t off = 0; off + 32 <= len; off += 32) {
        const uint8_t *e = &buf[off];
        if (e[0] == 0xEB && e[1] == 0xEB) {
            *has_md5 = true;
            break;
        }
        if (e[0] != 0xAA || e[1] != 0x50) {
            break;                                  /* 0xFFFF: end of the table */
        }
        if (n < max) {
            dpl_esp_part_t *p = &out[n];
            p->type = e[2];
            p->subtype = e[3];
            p->offset = le32(&e[4]);
            p->size = le32(&e[8]);
            memcpy(p->label, &e[12], 16);
            p->label[16] = '\0';
            for (int i = 0; i < 16 && p->label[i]; i++) {
                if ((unsigned char)p->label[i] < ' ' || (unsigned char)p->label[i] > '~') {
                    p->label[i] = '?';
                }
            }
            p->flags = le32(&e[28]);
        }
        n++;
    }
    if (n == 0) {
        return -1;
    }
    return n > max ? max : n;
}

const char *dpl_esp_type_name(uint8_t type)
{
    switch (type) {
    case 0x00: return "app";
    case 0x01: return "data";
    case 0x02: return "bootloader";
    case 0x03: return "partition_table";
    default: return NULL;
    }
}

const char *dpl_esp_subtype_name(uint8_t type, uint8_t subtype, char *buf, size_t buf_len)
{
    if (type == 0x00) {
        if (subtype == 0x00) {
            return "factory";
        }
        if (subtype >= 0x10 && subtype <= 0x1f) {
            snprintf(buf, buf_len, "ota_%u", (unsigned)(subtype - 0x10));
            return buf;
        }
        if (subtype == 0x20) {
            return "test";
        }
        return NULL;
    }
    if (type == 0x01) {
        switch (subtype) {
        case 0x00: return "ota";
        case 0x01: return "phy";
        case 0x02: return "nvs";
        case 0x03: return "coredump";
        case 0x04: return "nvs_keys";
        case 0x05: return "efuse";
        case 0x06: return "undefined";
        case 0x80: return "esphttpd";
        case 0x81: return "fat";
        case 0x82: return "spiffs";
        case 0x83: return "littlefs";
        default: return NULL;
        }
    }
    if (type == 0x02) {
        return subtype == 0x00 ? "primary" : subtype == 0x01 ? "ota" : subtype == 0x02 ? "recovery" : NULL;
    }
    return NULL;
}

/* ===========================================================================
 *  RP2040 / RP2350
 * ======================================================================== */

bool dpl_arduino_pico_layout(const uint8_t words[16], uint32_t *fs_start, uint32_t *fs_end,
                             uint32_t *eeprom_start, uint32_t *flash_size)
{
    const uint32_t fs = le32(&words[0]), fe = le32(&words[4]), ee = le32(&words[8]), len = le32(&words[12]);
    if (fs < DPL_RP_FLASH_BASE || fs > fe || fe > ee || (fs & 0xfff) || (fe & 0xfff) || (ee & 0xfff)) {
        return false;
    }
    if (ee + 0x1000u - DPL_RP_FLASH_BASE > DPL_RP_FLASH_MAX || len == 0 || len > ee - DPL_RP_FLASH_BASE) {
        return false;
    }
    *fs_start = fs;
    *fs_end = fe;
    *eeprom_start = ee;
    *flash_size = ee + 0x1000u - DPL_RP_FLASH_BASE;
    return true;
}

int dpl_rp2350_pt_info(const uint32_t *w, size_t nw, uint32_t requested, bool *has_table,
                       dpl_rp_part_t *out, int max)
{
    *has_table = false;
    if (nw < 2) {
        return -1;
    }
    /* [count,] supported flags (a subset of what was asked, with PT_INFO). */
    size_t pos = 0;
    if ((w[0] & ~requested) != 0 || !(w[0] & DPL_PT_INFO_PT_INFO)) {
        pos = 1;
        if ((w[1] & ~requested) != 0 || !(w[1] & DPL_PT_INFO_PT_INFO)) {
            return -1;
        }
        if (w[0] + 1 < nw) {
            nw = w[0] + 1;          /* the words the bootrom says it filled */
        }
    }
    const uint32_t fields = w[pos++];
    if (pos + 3 > nw) {
        return -1;
    }
    const uint32_t count = w[pos] & 0xff;
    *has_table = (w[pos] & 0x100) != 0;
    pos += 3;                       /* count, unpartitioned location, unpartitioned flags */
    int n = 0;
    for (uint32_t i = 0; i < count; i++) {
        dpl_rp_part_t p;
        memset(&p, 0, sizeof(p));
        if (fields & DPL_PT_INFO_LOCATION_AND_FLAGS) {
            if (pos + 2 > nw) {
                break;
            }
            const uint32_t loc = w[pos++];
            p.flags = w[pos++];
            p.start = (loc & 0x1fff) * 4096u;
            p.end = (((loc >> 13) & 0x1fff) + 1) * 4096u;
            p.permissions = loc >> 26;
        }
        if ((fields & DPL_PT_INFO_ID) && (p.flags & 1)) {
            if (pos + 2 > nw) {
                break;
            }
            p.has_id = true;
            p.id = (uint64_t)w[pos] | (uint64_t)w[pos + 1] << 32;
            pos += 2;
        }
        if ((fields & DPL_PT_INFO_NAME) && (p.flags & 0x1000)) {
            if (pos >= nw) {
                break;
            }
            const uint8_t len = (uint8_t)(w[pos] & 0x7f);
            const size_t words = (1u + len + 3u) / 4u;
            if (pos + words > nw) {
                break;
            }
            for (uint8_t k = 0; k < len && k + 1 < sizeof(p.name); k++) {
                const size_t b = 1u + k;
                p.name[k] = (char)((w[pos + b / 4] >> (8 * (b % 4))) & 0xff);
            }
            pos += words;
        }
        if (n < max) {
            out[n++] = p;
        }
    }
    return n;
}

/* ===========================================================================
 *  File systems
 * ======================================================================== */

const char *dpl_fs_name(dpl_fs_kind_t kind)
{
    switch (kind) {
    case DPL_FS_BLANK: return "blank";
    case DPL_FS_FAT: return "fat";
    case DPL_FS_LITTLEFS: return "littlefs";
    case DPL_FS_SPIFFS: return "spiffs";
    case DPL_FS_FAT_WL: return "fat_wl";
    default: return "unknown";
    }
}

bool dpl_blank(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != 0xFF) {
            return false;
        }
    }
    return len > 0;
}

/* The first commit of a metadata block: its end (aligned to the writer's
   prog_size), or 0. */
static uint32_t lfs_first_commit_end(const uint8_t *block, size_t len)
{
    uint32_t ptag = 0xffffffffu;
    size_t off = 4;                                     /* after the revision count */
    while (off + 4 <= len) {
        const uint32_t tag = be32(&block[off]) ^ ptag;
        if (tag & 0x80000000u) {
            return 0;                                   /* not a valid tag */
        }
        const uint32_t size = tag & 0x3ff;
        const size_t dsize = 4 + (size == 0x3ff ? 0 : size);
        const uint32_t type = (tag >> 20) & 0x7ff;
        if ((type & 0x700) == 0x500 && type != 0x5ff) {  /* CRC (not FCRC) ends the commit */
            return off + dsize <= len ? (uint32_t)(off + dsize) : 0;
        }
        ptag = tag;
        off += dsize;
    }
    return 0;
}

bool dpl_lfs_superblock(const uint8_t *block, size_t len, dpl_fs_info_t *out)
{
    if (len < 44 || memcmp(&block[8], "littlefs", 8) != 0) {
        return false;
    }
    const uint32_t name_tag = be32(&block[4]) ^ 0xffffffffu;
    if (name_tag != 0x0ff00008u) {                      /* SUPERBLOCK, id 0, size 8 */
        return false;
    }
    const uint32_t struct_tag = be32(&block[16]) ^ name_tag;
    if (((struct_tag >> 20) & 0x7ff) != 0x201 || ((struct_tag >> 10) & 0x3ff) != 0 || (struct_tag & 0x3ff) < 24) {
        return false;                                   /* INLINESTRUCT, id 0, >= 24 bytes */
    }
    memset(out, 0, sizeof(*out));
    out->kind = DPL_FS_LITTLEFS;
    out->version = le32(&block[20]);
    out->block_size = le32(&block[24]);
    out->block_count = le32(&block[28]);
    out->name_max = le32(&block[32]);
    out->file_max = le32(&block[36]);
    out->attr_max = le32(&block[40]);
    out->commit_align = lfs_first_commit_end(block, len);
    if ((out->version >> 16) != 2 || out->block_size < 128 || !pow2(out->block_size) || out->block_count < 2) {
        return false;
    }
    return true;
}

bool dpl_fat_bpb(const uint8_t *s, size_t len, dpl_fs_info_t *out)
{
    if (len < 512 || !((s[0] == 0xEB && s[2] == 0x90) || s[0] == 0xE9) || s[510] != 0x55 || s[511] != 0xAA) {
        return false;
    }
    const uint32_t bps = le16(&s[11]);
    const uint32_t spc = s[13];
    const uint32_t rsvd = le16(&s[14]);
    const uint32_t nfats = s[16];
    const uint32_t root_ents = le16(&s[17]);
    const uint32_t tot = le16(&s[19]) ? le16(&s[19]) : le32(&s[32]);
    const uint32_t fatsz = le16(&s[22]) ? le16(&s[22]) : le32(&s[36]);
    if (bps < 512 || bps > 4096 || !pow2(bps) || !pow2(spc) || rsvd == 0 || nfats < 1 || nfats > 2 || tot == 0 ||
            fatsz == 0) {
        return false;
    }
    const uint32_t root_secs = (root_ents * 32 + bps - 1) / bps;
    const uint32_t meta = rsvd + nfats * fatsz + root_secs;
    if (meta >= tot) {
        return false;
    }
    const uint32_t clusters = (tot - meta) / spc;
    memset(out, 0, sizeof(*out));
    out->kind = DPL_FS_FAT;
    out->block_size = bps;
    out->block_count = tot;
    out->cluster_size = bps * spc;
    out->fat_bits = clusters < 4085 ? 12 : clusters < 65525 ? 16 : 32;
    const uint8_t *lab = out->fat_bits == 32 ? &s[71] : &s[43];
    const uint8_t sig = out->fat_bits == 32 ? s[66] : s[38];
    if (sig == 0x29) {                                  /* extended boot signature: the label is valid */
        memcpy(out->label, lab, 11);
        out->label[11] = '\0';
        for (int i = 10; i >= 0 && out->label[i] == ' '; i--) {
            out->label[i] = '\0';
        }
        for (int i = 0; out->label[i]; i++) {
            if ((unsigned char)out->label[i] < ' ' || (unsigned char)out->label[i] > '~') {
                out->label[i] = '?';
            }
        }
    }
    return true;
}

#define SPIFFS_BLOCK    4096u
#define SPIFFS_PAGE     256u
#define SPIFFS_MAGIC_AT (SPIFFS_PAGE - 4u)              /* second last 16-bit entry of the lookup page */

static uint16_t spiffs_magic(uint32_t count, uint32_t bix)
{
    return (uint16_t)(0x20140529u ^ SPIFFS_PAGE ^ (count - bix));
}

bool dpl_spiffs(const uint8_t *block0, const uint8_t *block1, size_t len, uint32_t region_size, dpl_fs_info_t *out)
{
    if (len < SPIFFS_PAGE || region_size < 2 * SPIFFS_BLOCK) {
        return false;
    }
    const uint32_t count = region_size / SPIFFS_BLOCK;
    if (le16(&block0[SPIFFS_MAGIC_AT]) != spiffs_magic(count, 0)) {
        return false;
    }
    /* A block being erased or garbage collected has no magic: block 1 is
       only a second opinion when it has one. */
    if (block1 != NULL && le16(&block1[SPIFFS_MAGIC_AT]) != 0xffff &&
            le16(&block1[SPIFFS_MAGIC_AT]) != spiffs_magic(count, 1)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->kind = DPL_FS_SPIFFS;
    out->block_size = SPIFFS_BLOCK;
    out->block_count = count;
    out->page_size = SPIFFS_PAGE;
    return true;
}

bool dpl_esp_wl(const uint8_t *s, size_t len, uint32_t part_offset, uint32_t part_size)
{
    if (len < 36) {
        return false;
    }
    /* WL_Config_s: start_addr, full_mem_size, page_size, sector_size, updaterate, wr_size, version, temp_buff_size, crc */
    const uint32_t start = le32(&s[0]), full = le32(&s[4]), page = le32(&s[8]), sector = le32(&s[12]);
    const uint32_t version = le32(&s[24]);
    return (start == 0 || start == part_offset) && full == part_size && page == 4096 &&
           (sector == 512 || sector == 4096) && version >= 1 && version <= 16;
}

dpl_fs_kind_t dpl_detect(const uint8_t *block0, const uint8_t *block1, const uint8_t *last, size_t len,
                         uint32_t region_offset, uint32_t region_size, dpl_fs_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (dpl_lfs_superblock(block0, len, out)) {
        out->superblock = 0;
        return out->kind;
    }
    if (block1 != NULL && dpl_lfs_superblock(block1, len, out)) {
        out->superblock = 1;
        return out->kind;
    }
    if (dpl_fat_bpb(block0, len, out)) {
        return out->kind;
    }
    if (last != NULL && dpl_esp_wl(last, len, region_offset, region_size)) {
        memset(out, 0, sizeof(*out));
        out->kind = DPL_FS_FAT_WL;
        out->block_size = 4096;
        return out->kind;
    }
    if (dpl_spiffs(block0, block1, len, region_size, out)) {
        return out->kind;
    }
    memset(out, 0, sizeof(*out));
    out->kind = dpl_blank(block0, len) && (block1 == NULL || dpl_blank(block1, len)) ? DPL_FS_BLANK : DPL_FS_UNKNOWN;
    return out->kind;
}

uint32_t dpl_lfs_prog_size(uint32_t commit_align, uint32_t platform_default)
{
    if (commit_align == 0 || (platform_default && commit_align % platform_default == 0)) {
        return platform_default ? platform_default : 16;
    }
    uint32_t p = 512;
    while (p > 16 && commit_align % p != 0) {
        p >>= 1;
    }
    return p;
}
