/*
 * Host table test for main/ota_logic.c (no ESP-IDF needed):
 *
 *   gcc -std=c11 -Wall -Wextra -Werror -Imain main/ota_logic.c tools/ota_logic_test.c \
 *       -o /tmp/ota_logic_test && /tmp/ota_logic_test
 *
 * Real build outputs can be checked too, each with the result name expected
 * (chip 0x0020 = ESP32-S31, project esp32p4_usbip_bridge, 4 MB slot):
 *
 *   /tmp/ota_logic_test build/esp32p4_usbip_bridge.bin ok \
 *       build/bootloader/bootloader.bin not_an_app \
 *       build/partition_table/partition-table.bin bad_magic
 *
 * The images below are built byte by byte from the documented layouts of
 * esp_image_header_t, esp_image_segment_header_t and esp_app_desc_t, not from
 * the offsets in ota_logic.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ota_logic.h"

static int s_fail;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { printf("FAIL %s:%d: %s = %lld, want %lld\n", __FILE__, __LINE__, #a, _a, _b); s_fail++; } } while (0)
#define CHECK_STR(a, b) do { if (strcmp((a), (b)) != 0) { \
    printf("FAIL %s:%d: %s = \"%s\", want \"%s\"\n", __FILE__, __LINE__, #a, (a), (b)); s_fail++; } } while (0)

#define CHIP_S31  0x0020
#define CHIP_P4   0x0012
#define SLOT      (4u * 1024 * 1024)
#define PROJECT   "esp32p4_usbip_bridge"

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* An app image head: header (24), first segment header (8), app desc (256). */
static void make_app(uint8_t *img, uint16_t chip, uint8_t segments, const char *project, const char *version)
{
    memset(img, 0xAA, 512);
    img[0] = 0xE9;                      /* magic */
    img[1] = segments;                  /* segment_count */
    img[2] = 2;                         /* spi_mode DIO */
    img[3] = 0x4F;                      /* speed / size */
    put_le32(img + 4, 0x40000000u);     /* entry_addr */
    img[8] = 0xEE;                      /* wp_pin */
    img[12] = (uint8_t)chip;            /* chip_id, little-endian */
    img[13] = (uint8_t)(chip >> 8);
    img[23] = 1;                        /* hash_appended */
    put_le32(img + 24, 0x3C000020u);    /* segment load_addr (DROM) */
    put_le32(img + 28, 0x10000u);       /* segment data_len */
    uint8_t *d = img + 32;
    memset(d, 0, 256);
    put_le32(d, 0xABCD5432u);           /* magic_word */
    put_le32(d + 4, 3);                 /* secure_version */
    strncpy((char *)d + 16, version, 32);
    strncpy((char *)d + 48, project, 32);
    memcpy(d + 80, "12:34:56", 8);
    memcpy(d + 96, "Nov  2 2026", 11);
    memcpy(d + 112, "v6.2-dev-1234-gce2100de", 23);
}

static void test_accepts_app(void)
{
    uint8_t img[512];
    ota_img_info_t info;
    make_app(img, CHIP_S31, 6, PROJECT, "b0f0954-dirty");
    CHECK_EQ(ota_img_check(img, sizeof(img), 1100000, SLOT, CHIP_S31, PROJECT, &info), OTA_IMG_OK);
    CHECK_EQ(info.chip_id, CHIP_S31);
    CHECK_EQ(info.segment_count, 6);
    CHECK_EQ(info.secure_version, 3);
    CHECK_STR(info.project_name, PROJECT);
    CHECK_STR(info.version, "b0f0954-dirty");
    CHECK_STR(info.time, "12:34:56");
    CHECK_STR(info.date, "Nov  2 2026");
    CHECK_STR(info.idf_ver, "v6.2-dev-1234-gce2100de");
    /* exactly the header bytes are enough; info may be NULL */
    CHECK_EQ(ota_img_check(img, OTA_IMG_CHECK_LEN, 1100000, SLOT, CHIP_S31, PROJECT, NULL), OTA_IMG_OK);
    /* no project to compare: any project goes */
    make_app(img, CHIP_S31, 6, "hello_world", "1.0");
    CHECK_EQ(ota_img_check(img, sizeof(img), 200000, SLOT, CHIP_S31, NULL, NULL), OTA_IMG_OK);
    CHECK_EQ(ota_img_check(img, sizeof(img), 200000, SLOT, CHIP_S31, "", NULL), OTA_IMG_OK);
    /* the whole slot is allowed */
    make_app(img, CHIP_S31, 6, PROJECT, "x");
    CHECK_EQ(ota_img_check(img, sizeof(img), SLOT, SLOT, CHIP_S31, PROJECT, NULL), OTA_IMG_OK);
}

static void test_32_char_fields(void)
{
    /* version and project fill all 32 bytes with no terminator */
    uint8_t img[512];
    ota_img_info_t info;
    make_app(img, CHIP_S31, 6, PROJECT, "v");
    memset(img + 32 + 16, 'v', 32);
    memset(img + 32 + 48, 'p', 32);
    char p32[33];
    memset(p32, 'p', 32);
    p32[32] = '\0';
    CHECK_EQ(ota_img_check(img, sizeof(img), 300000, SLOT, CHIP_S31, p32, &info), OTA_IMG_OK);
    CHECK_EQ(strlen(info.version), 32);
    CHECK_EQ(strlen(info.project_name), 32);
}

static void test_rejects(void)
{
    uint8_t img[512];
    ota_img_info_t info;

    make_app(img, CHIP_S31, 6, PROJECT, "x");
    CHECK_EQ(ota_img_check(img, sizeof(img), SLOT + 1, SLOT, CHIP_S31, PROJECT, NULL), OTA_IMG_TOO_BIG);
    CHECK_EQ(ota_img_check(img, sizeof(img), OTA_IMG_CHECK_LEN - 1, SLOT, CHIP_S31, PROJECT, NULL), OTA_IMG_TOO_SMALL);
    CHECK_EQ(ota_img_check(img, OTA_IMG_CHECK_LEN - 1, 1100000, SLOT, CHIP_S31, PROJECT, NULL), OTA_IMG_SHORT);
    CHECK_EQ(ota_img_check(NULL, 0, 1100000, SLOT, CHIP_S31, PROJECT, NULL), OTA_IMG_SHORT);

    make_app(img, CHIP_S31, 6, PROJECT, "x");
    img[0] = 0xFF;                       /* erased flash / random file */
    CHECK_EQ(ota_img_check(img, sizeof(img), 1100000, SLOT, CHIP_S31, PROJECT, NULL), OTA_IMG_BAD_MAGIC);
    memcpy(img, "PK\x03\x04", 4);        /* a zip */
    CHECK_EQ(ota_img_check(img, sizeof(img), 1100000, SLOT, CHIP_S31, PROJECT, NULL), OTA_IMG_BAD_MAGIC);

    make_app(img, CHIP_S31, 0, PROJECT, "x");
    CHECK_EQ(ota_img_check(img, sizeof(img), 1100000, SLOT, CHIP_S31, PROJECT, NULL), OTA_IMG_BAD_SEGMENTS);
    make_app(img, CHIP_S31, 17, PROJECT, "x");
    CHECK_EQ(ota_img_check(img, sizeof(img), 1100000, SLOT, CHIP_S31, PROJECT, NULL), OTA_IMG_BAD_SEGMENTS);
    make_app(img, CHIP_S31, 16, PROJECT, "x");
    CHECK_EQ(ota_img_check(img, sizeof(img), 1100000, SLOT, CHIP_S31, PROJECT, NULL), OTA_IMG_OK);

    /* a P4 build of the same project */
    make_app(img, CHIP_P4, 6, PROJECT, "x");
    CHECK_EQ(ota_img_check(img, sizeof(img), 1100000, SLOT, CHIP_S31, PROJECT, &info), OTA_IMG_WRONG_CHIP);
    CHECK_EQ(info.chip_id, CHIP_P4);

    /* a bootloader: an ESP image whose first segment is code, no app desc */
    make_app(img, CHIP_S31, 3, PROJECT, "x");
    memset(img + 32, 0x13, 256);
    CHECK_EQ(ota_img_check(img, sizeof(img), 30000, SLOT, CHIP_S31, PROJECT, NULL), OTA_IMG_NO_APP_DESC);
    /* app desc magic present but the first segment too short to hold it */
    make_app(img, CHIP_S31, 6, PROJECT, "x");
    put_le32(img + 28, 200);
    CHECK_EQ(ota_img_check(img, sizeof(img), 1100000, SLOT, CHIP_S31, PROJECT, NULL), OTA_IMG_NO_APP_DESC);

    make_app(img, CHIP_S31, 6, "hello_world", "1.0");
    CHECK_EQ(ota_img_check(img, sizeof(img), 200000, SLOT, CHIP_S31, PROJECT, &info), OTA_IMG_WRONG_PROJECT);
    CHECK_STR(info.project_name, "hello_world");
}

static void test_names(void)
{
    CHECK_STR(ota_img_result_name(OTA_IMG_OK), "ok");
    CHECK_STR(ota_img_result_name(OTA_IMG_NO_APP_DESC), "not_an_app");
    CHECK_STR(ota_img_result_name(OTA_IMG_WRONG_CHIP), "wrong_chip");
    for (int r = OTA_IMG_OK; r <= OTA_IMG_WRONG_PROJECT; r++) {
        CHECK(strcmp(ota_img_result_name((ota_img_result_t)r), "unknown") != 0);
        CHECK(strcmp(ota_img_result_text((ota_img_result_t)r), "unknown result") != 0);
    }
}

static void test_query_flag(void)
{
    CHECK(ota_query_flag("reboot=1", "reboot"));
    CHECK(ota_query_flag("reboot", "reboot"));
    CHECK(ota_query_flag("force=1&reboot=true", "reboot"));
    CHECK(ota_query_flag("reboot=yes&force=0", "reboot"));
    CHECK(!ota_query_flag("reboot=0", "reboot"));
    CHECK(!ota_query_flag("reboot=no", "reboot"));
    CHECK(!ota_query_flag("reboot=", "reboot"));
    CHECK(!ota_query_flag("noreboot=1", "reboot"));
    CHECK(!ota_query_flag("rebooted=1", "reboot"));
    CHECK(!ota_query_flag("x=reboot", "reboot"));
    CHECK(!ota_query_flag("reboot=10", "reboot"));
    CHECK(!ota_query_flag(NULL, "reboot"));
    CHECK(!ota_query_flag("", "reboot"));
    CHECK(!ota_query_flag("force=1", "reboot"));
    CHECK(ota_query_flag("reboot=1&force", "force"));
    CHECK(ota_query_flag("a=1&&force=1", "force"));
}

/* argv: pairs of <file> <expected result name> */
static void check_files(int argc, char **argv)
{
    for (int i = 1; i + 1 < argc; i += 2) {
        FILE *f = fopen(argv[i], "rb");
        if (f == NULL) {
            printf("FAIL cannot open %s\n", argv[i]);
            s_fail++;
            continue;
        }
        uint8_t head[OTA_IMG_CHECK_LEN];
        const size_t n = fread(head, 1, sizeof(head), f);
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fclose(f);
        ota_img_info_t info;
        const ota_img_result_t r = ota_img_check(head, n, (size_t)size, SLOT, CHIP_S31, PROJECT, &info);
        printf("%s: %ld bytes -> %s (%s)%s%s\n", argv[i], size, ota_img_result_name(r), ota_img_result_text(r),
               r == OTA_IMG_OK ? ", version " : "", r == OTA_IMG_OK ? info.version : "");
        if (strcmp(ota_img_result_name(r), argv[i + 1]) != 0) {
            printf("FAIL %s: want %s\n", argv[i], argv[i + 1]);
            s_fail++;
        }
    }
}

int main(int argc, char **argv)
{
    test_accepts_app();
    test_32_char_fields();
    test_rejects();
    test_names();
    test_query_flag();
    check_files(argc, argv);
    if (s_fail) {
        printf("%d check(s) failed\n", s_fail);
        return 1;
    }
    printf("ota_logic: all checks passed\n");
    return 0;
}
