/*
 * Host test for main/dut_part_logic.c (no ESP-IDF needed).  LittleFS images
 * are made here with the vendored littlefs; the other fixtures come from
 * ESP-IDF's own generators (gen_esp32part, spiffsgen, fatfsgen, wl_fatfsgen):
 *
 *   tools/dut_part_fixtures.sh /tmp/dut_fixtures     (needs IDF_PATH and its Python)
 *
 * Then, on any host with gcc:
 *
 *   gcc -std=gnu11 -Wall -Wextra -Werror -DLFS_MULTIVERSION -Imain -Icomponents/littlefs \
 *       main/dut_part_logic.c components/littlefs/lfs.c components/littlefs/lfs_util.c \
 *       tools/dut_part_logic_test.c -o /tmp/dut_part_logic_test && /tmp/dut_part_logic_test <fixture dir>
 *
 * The RP2350 GET_INFO answers are built from the word layout in the pico-sdk
 * (bootrom_constants.h PT_INFO_*, picobin.h PICOBIN_PARTITION_*): there is
 * no generator for them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dut_part_logic.h"
#include "lfs.h"

static int s_fail;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { printf("FAIL %s:%d: %s = 0x%llx, want 0x%llx\n", __FILE__, __LINE__, #a, _a, _b); s_fail++; } } while (0)

static uint8_t *load(const char *dir, const char *name, size_t *len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        printf("FAIL: cannot open fixture %s\n", path);
        s_fail++;
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*len);
    if (fread(b, 1, *len, f) != *len) {
        s_fail++;
    }
    fclose(f);
    return b;
}

/* ---- the ESP partition table (gen_esp32part.py) ------------------------ */

static void test_esp_table(const char *dir)
{
    size_t len;
    uint8_t *b = load(dir, "ptable.bin", &len);
    if (b == NULL) {
        return;
    }
    dpl_esp_part_t p[DPL_ESP_PT_MAX];
    bool md5 = false;
    const int n = dpl_esp_parse_table(b, len, p, DPL_ESP_PT_MAX, &md5);
    CHECK_EQ(n, 6);
    CHECK(md5);
    CHECK(strcmp(p[0].label, "nvs") == 0);
    CHECK_EQ(p[0].type, 1);
    CHECK_EQ(p[0].subtype, 2);
    CHECK_EQ(p[0].offset, 0x9000);
    CHECK(strcmp(p[2].label, "app0") == 0);
    CHECK_EQ(p[2].type, 0);
    CHECK_EQ(p[2].subtype, 0x10);
    CHECK(strcmp(p[4].label, "spiffs") == 0);
    CHECK_EQ(p[4].offset, 0x3D0000);
    CHECK_EQ(p[4].size, 0x20000);
    char sub[16];
    CHECK(strcmp(dpl_esp_subtype_name(0, 0x11, sub, sizeof(sub)), "ota_1") == 0);
    CHECK(strcmp(dpl_esp_subtype_name(1, 0x82, sub, sizeof(sub)), "spiffs") == 0);
    CHECK(strcmp(dpl_esp_type_name(1), "data") == 0);
    /* A blank sector is no table. */
    memset(b, 0xFF, len);
    CHECK_EQ(dpl_esp_parse_table(b, len, p, DPL_ESP_PT_MAX, &md5), -1);
    /* max is honoured */
    free(b);
    b = load(dir, "ptable.bin", &len);
    CHECK_EQ(dpl_esp_parse_table(b, len, p, 2, &md5), 2);
    free(b);
}

/* ---- LittleFS, made by littlefs itself ---------------------------------- */

#define RAM_BLOCK 4096
#define RAM_COUNT 32
static uint8_t s_ram[RAM_BLOCK * RAM_COUNT];

static int ram_read(const struct lfs_config *c, lfs_block_t b, lfs_off_t off, void *buf, lfs_size_t n)
{
    memcpy(buf, &s_ram[b * c->block_size + off], n);
    return 0;
}

static int ram_prog(const struct lfs_config *c, lfs_block_t b, lfs_off_t off, const void *buf, lfs_size_t n)
{
    const uint8_t *s = buf;
    for (lfs_size_t i = 0; i < n; i++) {
        s_ram[b * c->block_size + off + i] &= s[i];
    }
    return 0;
}

static int ram_erase(const struct lfs_config *c, lfs_block_t b)
{
    memset(&s_ram[b * c->block_size], 0xFF, c->block_size);
    return 0;
}

static int ram_sync(const struct lfs_config *c)
{
    (void)c;
    return 0;
}

static void make_lfs(uint32_t prog, uint32_t version, uint32_t name_max)
{
    memset(s_ram, 0xFF, sizeof(s_ram));
    struct lfs_config c = {
        .read = ram_read, .prog = ram_prog, .erase = ram_erase, .sync = ram_sync,
        .read_size = prog, .prog_size = prog, .block_size = RAM_BLOCK, .block_count = RAM_COUNT,
        .block_cycles = 500, .cache_size = prog > 512 ? prog : 512, .lookahead_size = 16,
        .name_max = name_max, .disk_version = version,
    };
    lfs_t lfs;
    CHECK_EQ(lfs_format(&lfs, &c), 0);
    CHECK_EQ(lfs_mount(&lfs, &c), 0);
    lfs_file_t f;
    CHECK_EQ(lfs_file_open(&lfs, &f, "/secrets.json", LFS_O_WRONLY | LFS_O_CREAT), 0);
    CHECK_EQ(lfs_file_write(&lfs, &f, "{}", 2), 2);
    CHECK_EQ(lfs_file_close(&lfs, &f), 0);
    CHECK_EQ(lfs_unmount(&lfs), 0);
}

static void test_lfs(void)
{
    const uint32_t progs[] = { 16, 128, 256 };
    for (size_t i = 0; i < sizeof(progs) / sizeof(progs[0]); i++) {
        make_lfs(progs[i], 0x00020000, 64);
        dpl_fs_info_t fs;
        const int found0 = dpl_lfs_superblock(&s_ram[0], RAM_BLOCK, &fs);
        dpl_fs_info_t fs1;
        const int found1 = dpl_lfs_superblock(&s_ram[RAM_BLOCK], RAM_BLOCK, &fs1);
        CHECK(found0 || found1);
        if (!found0) {
            fs = fs1;
        }
        CHECK_EQ(fs.kind, DPL_FS_LITTLEFS);
        CHECK_EQ(fs.version, 0x00020000);
        CHECK_EQ(fs.block_size, RAM_BLOCK);
        CHECK_EQ(fs.block_count, RAM_COUNT);
        CHECK_EQ(fs.name_max, 64);
        CHECK(fs.commit_align != 0 && fs.commit_align % progs[i] == 0);
        /* What the bridge would program with: the platform default when the
           commits line up with it, else something they line up with. */
        const uint32_t p = dpl_lfs_prog_size(fs.commit_align, 128);
        CHECK(fs.commit_align % p == 0);
        if (progs[i] == 128 || progs[i] == 256) {
            CHECK_EQ(p, 128);
        }
        dpl_fs_info_t d;
        CHECK_EQ(dpl_detect(&s_ram[0], &s_ram[RAM_BLOCK], &s_ram[(RAM_COUNT - 1) * RAM_BLOCK], RAM_BLOCK, 0x3d0000,
                            sizeof(s_ram), &d), DPL_FS_LITTLEFS);
    }
    /* On-disk 2.1, and only block 1 carrying the superblock. */
    make_lfs(256, 0x00020001, 255);
    dpl_fs_info_t fs;
    CHECK(dpl_lfs_superblock(&s_ram[0], RAM_BLOCK, &fs) || dpl_lfs_superblock(&s_ram[RAM_BLOCK], RAM_BLOCK, &fs));
    CHECK_EQ(fs.version, 0x00020001);
    CHECK_EQ(fs.name_max, 255);
    memcpy(&s_ram[RAM_BLOCK], &s_ram[0], RAM_BLOCK);
    memset(&s_ram[0], 0xFF, RAM_BLOCK);
    dpl_fs_info_t d;
    CHECK_EQ(dpl_detect(&s_ram[0], &s_ram[RAM_BLOCK], NULL, RAM_BLOCK, 0, sizeof(s_ram), &d), DPL_FS_LITTLEFS);
    CHECK_EQ(d.superblock, 1);
    /* The magic alone is not enough. */
    uint8_t junk[64];
    memset(junk, 0, sizeof(junk));
    memcpy(&junk[8], "littlefs", 8);
    CHECK(!dpl_lfs_superblock(junk, sizeof(junk), &fs));
    CHECK_EQ(dpl_lfs_prog_size(0, 256), 256);
    CHECK_EQ(dpl_lfs_prog_size(48, 128), 16);
    CHECK_EQ(dpl_lfs_prog_size(1024, 256), 256);
}

/* ---- FAT and wear levelling (fatfsgen.py / wl_fatfsgen.py) -------------- */

static void test_fat(const char *dir)
{
    size_t len;
    dpl_fs_info_t fs;
    uint8_t *b = load(dir, "fat512.img", &len);
    if (b != NULL) {
        CHECK(dpl_fat_bpb(b, len, &fs));
        CHECK_EQ(fs.block_size, 512);
        CHECK_EQ(fs.block_count, 2048);
        CHECK_EQ(fs.fat_bits, 12);
        CHECK_EQ(dpl_detect(b, b + 4096, b + len - 4096, 4096, 0x450000, (uint32_t)len, &fs), DPL_FS_FAT);
        free(b);
    }
    b = load(dir, "fat4k.img", &len);
    if (b != NULL) {
        CHECK(dpl_fat_bpb(b, len, &fs));
        CHECK_EQ(fs.block_size, 4096);
        CHECK_EQ(fs.block_count, 256);
        CHECK_EQ(fs.fat_bits, 12);
        free(b);
    }
    b = load(dir, "fat16.img", &len);
    if (b != NULL) {
        CHECK(dpl_fat_bpb(b, len, &fs));
        CHECK_EQ(fs.fat_bits, 16);
        CHECK_EQ((uint64_t)fs.block_size * fs.block_count, len);
        free(b);
    }
    b = load(dir, "fatwl.img", &len);
    if (b != NULL) {
        CHECK(!dpl_fat_bpb(b, len, &fs));                   /* sector 0 is WL's spare sector */
        CHECK(dpl_fat_bpb(b + 4096, 4096, &fs));
        CHECK(dpl_esp_wl(b + len - 4096, 4096, 0x450000, (uint32_t)len));
        CHECK(!dpl_esp_wl(b + len - 4096, 4096, 0x450000, (uint32_t)len * 2));
        CHECK_EQ(dpl_detect(b, b + 4096, b + len - 4096, 4096, 0x450000, (uint32_t)len, &fs), DPL_FS_FAT_WL);
        free(b);
    }
    /* No 0x55AA, no FAT. */
    uint8_t s[512];
    memset(s, 0, sizeof(s));
    s[0] = 0xEB;
    s[2] = 0x90;
    CHECK(!dpl_fat_bpb(s, sizeof(s), &fs));
}

/* ---- SPIFFS (spiffsgen.py) ----------------------------------------------- */

static void test_spiffs(const char *dir)
{
    size_t len;
    uint8_t *b = load(dir, "spiffs.bin", &len);
    if (b == NULL) {
        return;
    }
    dpl_fs_info_t fs;
    CHECK(dpl_spiffs(b, b + 4096, 4096, (uint32_t)len, &fs));
    CHECK_EQ(fs.block_count, len / 4096);
    CHECK_EQ(dpl_detect(b, b + 4096, b + len - 4096, 4096, 0x290000, (uint32_t)len, &fs), DPL_FS_SPIFFS);
    /* The magic carries the block count: a wrong partition size fails. */
    CHECK(!dpl_spiffs(b, b + 4096, 4096, (uint32_t)len * 2, &fs));
    free(b);
    uint8_t blank[4096];
    memset(blank, 0xFF, sizeof(blank));
    CHECK_EQ(dpl_detect(blank, blank, blank, sizeof(blank), 0, 0x20000, &fs), DPL_FS_BLANK);
    blank[100] = 0;
    CHECK_EQ(dpl_detect(blank, blank, blank, sizeof(blank), 0, 0x20000, &fs), DPL_FS_UNKNOWN);
}

/* ---- arduino-pico layout words (boards.txt values) ---------------------- */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void test_arduino_pico(void)
{
    uint8_t w[16];
    uint32_t fs, fe, ee, size;
    /* rpipico2w (RP2350), 4 MB with a 64 KB file system */
    put32(&w[0], 272556032);
    put32(&w[4], 272621568);
    put32(&w[8], 272621568);
    put32(&w[12], 4120576);
    CHECK(dpl_arduino_pico_layout(w, true, &fs, &fe, &ee, &size));
    CHECK_EQ(fs, 0x103EE000);
    CHECK_EQ(fe, 0x103FE000);
    CHECK_EQ(ee, 0x103FE000);
    CHECK_EQ(size, 4u << 20);
    /* rpipico2w, 4 MB without one */
    put32(&w[0], 272621568);
    put32(&w[4], 272621568);
    put32(&w[8], 272621568);
    put32(&w[12], 4186112);
    CHECK(dpl_arduino_pico_layout(w, true, &fs, &fe, &ee, &size));
    CHECK_EQ(fs, fe);
    /* rpipico (RP2040), 2 MB with 64 KB */
    put32(&w[0], 270462976);
    put32(&w[4], 270528512);
    put32(&w[8], 270528512);
    put32(&w[12], 2027520);
    CHECK(dpl_arduino_pico_layout(w, false, &fs, &fe, &ee, &size));
    CHECK_EQ(fe - fs, 65536);
    CHECK_EQ(size, 2u << 20);
    /* code there instead (any other firmware) */
    memset(w, 0xFF, sizeof(w));
    CHECK(!dpl_arduino_pico_layout(w, true, &fs, &fe, &ee, &size));
    put32(&w[0], 0x20001000);
    put32(&w[4], 0x10000201);
    put32(&w[8], 0x10000203);
    put32(&w[12], 0x10000205);
    CHECK(!dpl_arduino_pico_layout(w, false, &fs, &fe, &ee, &size));
}

/* ---- RP2350 GET_INFO partition table answers ---------------------------- */

static void test_rp2350(void)
{
    const uint32_t req = DPL_PT_INFO_PT_INFO | DPL_PT_INFO_LOCATION_AND_FLAGS | DPL_PT_INFO_NAME;
    /* Two partitions: sectors 0x2..0x1ff "main" (has name), 0x200..0x3ff no name. */
    const uint32_t loc0 = 0x2u | (0x1ffu << 13) | (0x3fu << 26), flags0 = 0x1000u | 0x20000u;
    const uint32_t loc1 = 0x200u | (0x3ffu << 13) | (0x3fu << 26), flags1 = 0;
    /* name: length 4, "main" -> bytes 04 'm' 'a' 'i' | 'n' 00 00 00 */
    const uint32_t name0 = 4u | ('m' << 8) | ('a' << 16) | ((uint32_t)'i' << 24), name1 = 'n';
    const uint32_t words[] = { req, 2u | 0x100u, 0, 0, loc0, flags0, name0, name1, loc1, flags1 };
    dpl_rp_part_t p[4];
    bool has = false;
    int n = dpl_rp2350_pt_info(words, sizeof(words) / 4, req, &has, p, 4);
    CHECK_EQ(n, 2);
    CHECK(has);
    CHECK_EQ(p[0].start, 0x2000);
    CHECK_EQ(p[0].end, 0x200000);
    CHECK(strcmp(p[0].name, "main") == 0);
    CHECK_EQ(p[1].start, 0x200000);
    CHECK_EQ(p[1].end, 0x400000);
    CHECK(p[1].name[0] == '\0');
    /* With a leading word count. */
    uint32_t counted[sizeof(words) / 4 + 1];
    counted[0] = sizeof(words) / 4;
    memcpy(&counted[1], words, sizeof(words));
    n = dpl_rp2350_pt_info(counted, sizeof(counted) / 4, req, &has, p, 4);
    CHECK_EQ(n, 2);
    CHECK(strcmp(p[0].name, "main") == 0);
    /* No partition table. */
    const uint32_t none[] = { req, 0, 0, 0 };
    n = dpl_rp2350_pt_info(none, 4, req, &has, p, 4);
    CHECK_EQ(n, 0);
    CHECK(!has);
    /* Garbage. */
    const uint32_t junk[] = { 0xdeadbeef, 0x12345678, 1, 2 };
    CHECK_EQ(dpl_rp2350_pt_info(junk, 4, req, &has, p, 4), -1);
    /* Truncated: stops without reading past the end. */
    n = dpl_rp2350_pt_info(words, 7, req, &has, p, 4);
    CHECK_EQ(n, 0);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "fixtures";
    test_esp_table(dir);
    test_lfs();
    test_fat(dir);
    test_spiffs(dir);
    test_arduino_pico();
    test_rp2350();
    if (s_fail) {
        printf("%d failure(s)\n", s_fail);
        return 1;
    }
    printf("dut_part_logic: all passed\n");
    return 0;
}
