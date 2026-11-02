#include "ota_logic.h"

#include <string.h>

/* esp_image_header_t */
#define HDR_MAGIC           0
#define HDR_SEGMENT_COUNT   1
#define HDR_CHIP_ID         12
#define HDR_LEN             24
/* esp_image_segment_header_t */
#define SEG_DATA_LEN        4
#define SEG_LEN             8
/* esp_app_desc_t, after the first segment header */
#define DESC                (HDR_LEN + SEG_LEN)
#define DESC_MAGIC          0
#define DESC_SECURE_VERSION 4
#define DESC_VERSION        16
#define DESC_PROJECT_NAME   48
#define DESC_TIME           80
#define DESC_DATE           96
#define DESC_IDF_VER        112
#define DESC_LEN            256

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* A fixed-size, possibly unterminated field into a terminated string. */
static void field(char *out, size_t out_size, const uint8_t *src, size_t src_len)
{
    size_t n = 0;
    while (n < src_len && n + 1 < out_size && src[n] != '\0') {
        out[n] = (char)src[n];
        n++;
    }
    out[n] = '\0';
}

ota_img_result_t ota_img_check(const uint8_t *buf, size_t len, size_t image_size, size_t slot_size,
                               uint16_t chip_id, const char *project, ota_img_info_t *info)
{
    ota_img_info_t local;
    if (info == NULL) {
        info = &local;
    }
    memset(info, 0, sizeof(*info));
    if (image_size > slot_size) {
        return OTA_IMG_TOO_BIG;
    }
    if (image_size < OTA_IMG_CHECK_LEN) {
        return OTA_IMG_TOO_SMALL;
    }
    if (buf == NULL || len < OTA_IMG_CHECK_LEN) {
        return OTA_IMG_SHORT;
    }
    if (buf[HDR_MAGIC] != OTA_IMG_MAGIC) {
        return OTA_IMG_BAD_MAGIC;
    }
    info->segment_count = buf[HDR_SEGMENT_COUNT];
    info->chip_id = (uint16_t)(buf[HDR_CHIP_ID] | (buf[HDR_CHIP_ID + 1] << 8));
    if (info->segment_count == 0 || info->segment_count > OTA_MAX_SEGMENTS) {
        return OTA_IMG_BAD_SEGMENTS;
    }
    if (info->chip_id != chip_id) {
        return OTA_IMG_WRONG_CHIP;
    }
    /* The app description is the start of the first segment (DROM). */
    const uint8_t *d = buf + DESC;
    if (le32(buf + HDR_LEN + SEG_DATA_LEN) < DESC_LEN || le32(d + DESC_MAGIC) != OTA_APP_DESC_MAGIC) {
        return OTA_IMG_NO_APP_DESC;
    }
    info->secure_version = le32(d + DESC_SECURE_VERSION);
    field(info->version, sizeof(info->version), d + DESC_VERSION, 32);
    field(info->project_name, sizeof(info->project_name), d + DESC_PROJECT_NAME, 32);
    field(info->time, sizeof(info->time), d + DESC_TIME, 16);
    field(info->date, sizeof(info->date), d + DESC_DATE, 16);
    field(info->idf_ver, sizeof(info->idf_ver), d + DESC_IDF_VER, 32);
    if (project != NULL && project[0] != '\0' && strcmp(project, info->project_name) != 0) {
        return OTA_IMG_WRONG_PROJECT;
    }
    return OTA_IMG_OK;
}

const char *ota_img_result_name(ota_img_result_t r)
{
    switch (r) {
        case OTA_IMG_OK:            return "ok";
        case OTA_IMG_SHORT:         return "short";
        case OTA_IMG_TOO_SMALL:     return "too_small";
        case OTA_IMG_TOO_BIG:       return "too_big";
        case OTA_IMG_BAD_MAGIC:     return "bad_magic";
        case OTA_IMG_BAD_SEGMENTS:  return "bad_segments";
        case OTA_IMG_WRONG_CHIP:    return "wrong_chip";
        case OTA_IMG_NO_APP_DESC:   return "not_an_app";
        case OTA_IMG_WRONG_PROJECT: return "wrong_project";
    }
    return "unknown";
}

const char *ota_img_result_text(ota_img_result_t r)
{
    switch (r) {
        case OTA_IMG_OK:            return "image accepted";
        case OTA_IMG_SHORT:         return "the image ended before its headers";
        case OTA_IMG_TOO_SMALL:     return "too small to be an app image";
        case OTA_IMG_TOO_BIG:       return "larger than the OTA slot";
        case OTA_IMG_BAD_MAGIC:     return "not an ESP app image (no 0xE9 magic byte)";
        case OTA_IMG_BAD_SEGMENTS:  return "bad segment count in the image header";
        case OTA_IMG_WRONG_CHIP:    return "built for another chip";
        case OTA_IMG_NO_APP_DESC:   return "an ESP image without an app description (a bootloader?)";
        case OTA_IMG_WRONG_PROJECT: return "another project's app (add ?force=1 to write it anyway)";
    }
    return "unknown result";
}

bool ota_query_flag(const char *query, const char *name)
{
    if (query == NULL || name == NULL || name[0] == '\0') {
        return false;
    }
    const size_t nlen = strlen(name);
    const char *p = query;
    while (*p != '\0') {
        const size_t klen = strcspn(p, "=&");
        if (klen == nlen && strncmp(p, name, nlen) == 0) {
            if (p[klen] != '=') {
                return true;                    /* bare "reboot" */
            }
            const char *v = p + klen + 1;
            const size_t vlen = strcspn(v, "&");
            return (vlen == 1 && v[0] == '1') ||
                   (vlen == 4 && strncmp(v, "true", 4) == 0) ||
                   (vlen == 3 && strncmp(v, "yes", 3) == 0);
        }
        p += strcspn(p, "&");
        if (*p == '&') {
            p++;
        }
    }
    return false;
}
