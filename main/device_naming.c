#include "device_naming.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "device_naming";

/* ─────────────────────────────────────────────
 *  NVS helpers
 * ───────────────────────────────────────────── */

static esp_err_t nvs_open_rw(nvs_handle_t *h)
{
    return nvs_open(DEVICE_NVS_NAMESPACE, NVS_READWRITE, h);
}

static esp_err_t nvs_open_ro(nvs_handle_t *h)
{
    return nvs_open(DEVICE_NVS_NAMESPACE, NVS_READONLY, h);
}

/* Build the NVS key for a per-device friendly name.
   busid can contain characters like '-' which NVS keys cannot, so short
   busids are hex-encoded: "name_" + hex(busid). NVS keys are limited to
   15 characters, which that encoding exceeds for any busid longer than
   5 bytes (e.g. the port path "1-1.3.4.4" needs 23) — nvs_set_str then
   failed and the name endpoint answered 500 for every device deeper than
   one hub. Longer busids use "name_" + FNV-1a32(busid) (13 characters);
   the hex form is kept for short busids so existing stored names keep
   working. */
static void device_name_key(const char *busid, char *key_buf, size_t key_size)
{
    size_t busid_len = strlen(busid);
    if (busid_len == 0 || key_size < 16) {
        key_buf[0] = '\0';
        return;
    }
    if (5 + busid_len * 2 <= 15) {      /* NVS_KEY_NAME_MAX_SIZE - 1 */
        memcpy(key_buf, "name_", 5);
        for (size_t i = 0; i < busid_len; i++) {
            snprintf(key_buf + 5 + i * 2, 3, "%02x", (unsigned char)busid[i]);
        }
        key_buf[5 + busid_len * 2] = '\0';
        return;
    }
    uint32_t h = 2166136261u;           /* FNV-1a 32-bit */
    for (const char *q = busid; *q; q++) {
        h = (h ^ (unsigned char)*q) * 16777619u;
    }
    snprintf(key_buf, key_size, "name_%08x", (unsigned)h);
}

/* The chip in the default hostname: the IDF target without its "esp32"
 * prefix ("s31", "s3", "p4"; plain "esp32" stays), plus "v<major revision>"
 * on the ESP32-P4, whose v1 and v3 silicon differ.  Lower case, DNS-safe. */
static void chip_token(char *buf, size_t buf_size)
{
    const char *t = CONFIG_IDF_TARGET;
    if (strncmp(t, "esp32", 5) == 0 && t[5] != '\0') {
        t += 5;
    }
    size_t n = 0;
    for (; t[n] != '\0' && n + 1 < buf_size; n++) {
        const char c = t[n];
        buf[n] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a')
               : ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) ? c : '-';
    }
    buf[n] = '\0';
#if CONFIG_IDF_TARGET_ESP32P4
    esp_chip_info_t info;
    esp_chip_info(&info);           /* revision = major * 100 + minor */
    snprintf(buf + n, buf_size - n, "v%u", (unsigned)(info.revision / 100));
#endif
}

/* The hostname older firmware defaulted to ("usbip-" + the last three
 * base-MAC bytes). */
static bool legacy_default_hostname(char *buf, size_t buf_size)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_BASE) != ESP_OK) {
        return false;
    }
    snprintf(buf, buf_size, "usbip-%02x%02x%02x", mac[3], mac[4], mac[5]);
    return true;
}

/* The default hostname: "usbip-<chip>-<last three base-MAC bytes>", e.g.
 * usbip-s31-d19bee or usbip-p4v3-0a1b2c (well under the 63-character DNS
 * label limit).  ESP_MAC_BASE is always 6 bytes; esp_efuse_mac_get_default()
 * returns an 8-byte EUI-64 on 802.15.4 chips such as the ESP32-S31. */
static void default_hostname(char *buf, size_t buf_size)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_BASE) != ESP_OK) {
        strlcpy(buf, "usbip-000000", buf_size);     /* the old format's fallback */
        return;
    }
    char chip[24];
    chip_token(chip, sizeof(chip));
    char name[64];
    snprintf(name, sizeof(name), "usbip-%s-%02x%02x%02x", chip, mac[3], mac[4], mac[5]);
    strlcpy(buf, name, buf_size);
}

/* A stored hostname equal to the old default was never chosen by anyone
 * (it was saved back from an older firmware's default): clear it, once, so
 * the new default applies.  Returns true when stored is that old default. */
static bool migrate_legacy_hostname(const char *stored)
{
    char legacy[DEVICE_NAME_MAX_LEN];
    if (!legacy_default_hostname(legacy, sizeof(legacy)) || strcmp(stored, legacy) != 0) {
        return false;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open_rw(&h);
    if (err == ESP_OK) {
        err = nvs_erase_key(h, DEVICE_NVS_KEY_HOSTNAME);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    char now[DEVICE_NAME_MAX_LEN];
    default_hostname(now, sizeof(now));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Stored hostname \"%s\" was the old default: cleared, now \"%s\"", stored, now);
    } else {
        ESP_LOGW(TAG, "Stored hostname \"%s\" is the old default; clearing it failed (%s), using \"%s\"",
                 stored, esp_err_to_name(err), now);
    }
    return true;
}

/* ─────────────────────────────────────────────
 *  Public API
 * ───────────────────────────────────────────── */

esp_err_t device_naming_get_hostname(char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size < 1) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open_ro(&h);
    if (err != ESP_OK) {
        /* NVS not available — use default */
        default_hostname(buf, buf_size);
        return ESP_OK;
    }

    size_t len = buf_size;
    err = nvs_get_str(h, DEVICE_NVS_KEY_HOSTNAME, buf, &len);
    nvs_close(h);

    if (err == ESP_OK && strlen(buf) > 0 && !migrate_legacy_hostname(buf)) {
        ESP_LOGI(TAG, "Using stored hostname: \"%s\"", buf);
        return ESP_OK;
    }

    /* No stored hostname — fall back to MAC-based default */
    default_hostname(buf, buf_size);
    return ESP_OK;
}

esp_err_t device_naming_set_hostname(const char *hostname)
{
    if (hostname == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open_rw(&h);
    if (err != ESP_OK) {
        return err;
    }

    if (strlen(hostname) == 0) {
        /* Clear the stored hostname so get_hostname() falls back to default */
        err = nvs_erase_key(h, DEVICE_NVS_KEY_HOSTNAME);
    } else {
        err = nvs_set_str(h, DEVICE_NVS_KEY_HOSTNAME, hostname);
    }

    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Hostname set to: \"%s\"", strlen(hostname) ? hostname : "(default)");
    }
    return err;
}

esp_err_t device_naming_get_board_id(char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size < 1) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open_ro(&h);
    if (err != ESP_OK) {
        buf[0] = '\0';
        return err;
    }

    size_t len = buf_size;
    err = nvs_get_str(h, DEVICE_NVS_KEY_BOARD_ID, buf, &len);
    nvs_close(h);

    if (err == ESP_OK) {
        /* Found a stored value */
        return ESP_OK;
    }

    /* Not found — return empty string */
    buf[0] = '\0';
    return ESP_OK;
}

esp_err_t device_naming_set_board_id(const char *board_id)
{
    if (board_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open_rw(&h);
    if (err != ESP_OK) {
        return err;
    }

    if (strlen(board_id) == 0) {
        err = nvs_erase_key(h, DEVICE_NVS_KEY_BOARD_ID);
    } else {
        err = nvs_set_str(h, DEVICE_NVS_KEY_BOARD_ID, board_id);
    }

    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Board ID set to: \"%s\"", strlen(board_id) ? board_id : "(cleared)");
    }
    return err;
}

esp_err_t device_naming_get_device_name(const char *busid, char *buf, size_t buf_size)
{
    if (busid == NULL || buf == NULL || buf_size < 1) {
        return ESP_ERR_INVALID_ARG;
    }

    char key[64];
    device_name_key(busid, key, sizeof(key));
    if (key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open_ro(&h);
    if (err != ESP_OK) {
        buf[0] = '\0';
        return ESP_ERR_NVS_NOT_FOUND;
    }

    size_t len = buf_size;
    err = nvs_get_str(h, key, buf, &len);
    nvs_close(h);

    return err;
}

esp_err_t device_naming_set_device_name(const char *busid, const char *name)
{
    if (busid == NULL || name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char key[64];
    device_name_key(busid, key, sizeof(key));
    if (key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open_rw(&h);
    if (err != ESP_OK) {
        return err;
    }

    if (strlen(name) == 0) {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;   /* clearing a name that was never set is a no-op */
        }
    } else {
        err = nvs_set_str(h, key, name);
    }

    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Device \"%s\" name set to: \"%s\"", busid, strlen(name) ? name : "(cleared)");
    }
    return err;
}

esp_err_t device_naming_init(void)
{
    /* Nothing needed at init time beyond what the per-call functions do.
       The NVS partition is already initialised by main.c before we are
       called. */
    return ESP_OK;
}
