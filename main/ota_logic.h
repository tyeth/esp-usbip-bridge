#ifndef OTA_LOGIC_H
#define OTA_LOGIC_H

/*
 * OTA firmware updates: the checks made on an uploaded app image before any
 * of it is written to flash, with no ESP-IDF dependencies so they can be
 * table-tested on a host (see tools/ota_logic_test.c).  The byte offsets are
 * those of esp_image_header_t (24 bytes), esp_image_segment_header_t (8) and
 * esp_app_desc_t (256), which an IDF app image starts with.  esp_ota_end()
 * still verifies the whole image (checksum, SHA-256, chip revision).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bytes needed by ota_img_check(): image header + first segment header +
   app description. */
#define OTA_IMG_CHECK_LEN   (24 + 8 + 256)

#define OTA_IMG_MAGIC       0xE9
#define OTA_APP_DESC_MAGIC  0xABCD5432u
#define OTA_MAX_SEGMENTS    16

typedef enum {
    OTA_IMG_OK = 0,
    OTA_IMG_SHORT,          /* fewer than OTA_IMG_CHECK_LEN bytes given */
    OTA_IMG_TOO_SMALL,      /* the whole image is smaller than its headers */
    OTA_IMG_TOO_BIG,        /* larger than the OTA slot */
    OTA_IMG_BAD_MAGIC,      /* not an ESP image (no 0xE9) */
    OTA_IMG_BAD_SEGMENTS,   /* segment count 0 or over 16 */
    OTA_IMG_WRONG_CHIP,     /* built for another chip */
    OTA_IMG_NO_APP_DESC,    /* an ESP image, but not an app (e.g. bootloader.bin) */
    OTA_IMG_WRONG_PROJECT,  /* another project's app */
} ota_img_result_t;

typedef struct {
    uint16_t chip_id;
    uint8_t segment_count;
    uint32_t secure_version;
    char version[33];
    char project_name[33];
    char time[17];
    char date[17];
    char idf_ver[33];
} ota_img_info_t;

/* Check the first len bytes of an app image of image_size bytes for an OTA
   slot of slot_size bytes on chip chip_id.  project, when not NULL or "",
   must equal the image's project name.  info (may be NULL) is filled as far
   as the image could be parsed. */
ota_img_result_t ota_img_check(const uint8_t *buf, size_t len, size_t image_size, size_t slot_size,
                               uint16_t chip_id, const char *project, ota_img_info_t *info);

/* Short machine-readable name ("bad_magic") and a sentence for people. */
const char *ota_img_result_name(ota_img_result_t r);
const char *ota_img_result_text(ota_img_result_t r);

/* A boolean query flag: "reboot", "reboot=1", "reboot=true" or "reboot=yes"
   anywhere in query (the part after '?', may be NULL) is true;
   "reboot=0" / "false" / "no" or no such key is false. */
bool ota_query_flag(const char *query, const char *name);

#endif
