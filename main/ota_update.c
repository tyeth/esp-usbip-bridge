#include "ota_update.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "ota_logic.h"
#include "version_autogen.h"

static const char *TAG = "ota";

/* First-boot check of a new image */
typedef enum {
    CONFIRM_NOT_NEEDED = 0,     /* factory, or an image confirmed on an earlier boot */
    CONFIRM_WAITING,
    CONFIRMED,
    CONFIRM_FAILED,             /* timed out: marked invalid, rebooting */
} confirm_state_t;

static volatile confirm_state_t s_confirm;
static volatile bool s_seen_ip;
static volatile bool s_seen_http;
static volatile bool s_app_started;
static int64_t s_confirm_ms;

/* The upload in progress (one at a time: the HTTP server is one task) */
static bool s_active;
static esp_ota_handle_t s_handle;
static const esp_partition_t *s_part;
static size_t s_size;
static size_t s_written;
static int64_t s_t0;
static ota_img_info_t s_info;

/* The last upload since boot, for GET /api/ota */
static struct {
    bool any;
    bool ok;
    char partition[17];
    size_t size;
    size_t written;
    int ms;
    char version[33];
    char error[24];
    char message[96];
} s_last;

static void set_err(ota_update_err_t *e, int status, const char *error, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void set_err(ota_update_err_t *e, int status, const char *error, const char *fmt, ...)
{
    e->status = status;
    e->error = error;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->message, sizeof(e->message), fmt, ap);
    va_end(ap);
}

static void record_last(bool ok, const char *error, const char *message)
{
    s_last.any = true;
    s_last.ok = ok;
    strlcpy(s_last.partition, s_part ? s_part->label : "", sizeof(s_last.partition));
    s_last.size = s_size;
    s_last.written = s_written;
    s_last.ms = (int)((esp_timer_get_time() - s_t0) / 1000);
    strlcpy(s_last.version, s_info.version, sizeof(s_last.version));
    strlcpy(s_last.error, error ? error : "", sizeof(s_last.error));
    strlcpy(s_last.message, message ? message : "", sizeof(s_last.message));
}

/* ─────────────────────────────────────────────
 *  First boot of a new image: confirm or roll back
 * ───────────────────────────────────────────── */

static bool netif_has_ip(esp_netif_t *netif, void *ctx)
{
    (void)ctx;
    esp_netif_ip_info_t ip;
    return esp_netif_is_netif_up(netif) && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0;
}

/* GET /ping from our own HTTP server over loopback. */
static bool http_answers(void)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(80),
        .sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) },
    };
    bool ok = false;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        static const char req[] = "GET /ping HTTP/1.0\r\n\r\n";
        if (send(fd, req, sizeof(req) - 1, 0) == (int)(sizeof(req) - 1)) {
            char resp[16] = { 0 };
            size_t got = 0;
            while (got < sizeof(resp) - 1) {
                const int n = recv(fd, resp + got, sizeof(resp) - 1 - got, 0);
                if (n <= 0) {
                    break;
                }
                got += (size_t)n;
            }
            ok = strncmp(resp, "HTTP/1.1 200", 12) == 0;
        }
    }
    close(fd);
    return ok;
}

static void confirm_task(void *arg)
{
    (void)arg;
    const int64_t deadline_us = (int64_t)CONFIG_USBIP_OTA_CONFIRM_TIMEOUT_S * 1000000;
    while (esp_timer_get_time() < deadline_us) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!s_seen_ip) {
            s_seen_ip = esp_netif_find_if(netif_has_ip, NULL) != NULL;
        }
        if (s_seen_ip && !s_seen_http) {
            s_seen_http = http_answers();
        }
        if (s_app_started && s_seen_ip && s_seen_http) {
            const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            if (err == ESP_OK) {
                s_confirm_ms = esp_timer_get_time() / 1000;
                s_confirm = CONFIRMED;
                ESP_LOGI(TAG, "new image confirmed %lld ms after boot (network up, HTTP answering)",
                         (long long)s_confirm_ms);
                vTaskDelete(NULL);
                return;
            }
            ESP_LOGE(TAG, "esp_ota_mark_app_valid_cancel_rollback: %s", esp_err_to_name(err));
        }
    }
    s_confirm = CONFIRM_FAILED;
    ESP_LOGE(TAG, "new image not up after %d s (started %d, ip %d, http %d): rolling back",
             CONFIG_USBIP_OTA_CONFIRM_TIMEOUT_S, s_app_started, s_seen_ip, s_seen_http);
    const esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    /* Only returns on failure (no other image to go back to): stay up. */
    ESP_LOGE(TAG, "rollback failed: %s; keeping this image", esp_err_to_name(err));
    vTaskDelete(NULL);
}

void ota_update_app_started(void)
{
    s_app_started = true;
}

void ota_update_boot_check_start(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    if (invalid != NULL) {
        ESP_LOGW(TAG, "%s holds an image that was rolled back", invalid->label);
    }
    esp_ota_img_states_t state;
    if (running == NULL || esp_ota_get_state_partition(running, &state) != ESP_OK ||
            state != ESP_OTA_IMG_PENDING_VERIFY) {
        s_confirm = CONFIRM_NOT_NEEDED;
        return;
    }
    s_confirm = CONFIRM_WAITING;
    ESP_LOGW(TAG, "%s is a new image pending verification: confirming once the network and HTTP are up "
             "(rollback after %d s)", running->label, CONFIG_USBIP_OTA_CONFIRM_TIMEOUT_S);
    if (xTaskCreate(confirm_task, "ota_confirm", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot start the confirm task; the image stays pending and rolls back on the next reset");
    }
}

/* ─────────────────────────────────────────────
 *  Upload
 * ───────────────────────────────────────────── */

esp_err_t ota_update_begin(size_t image_size, const uint8_t *head, size_t head_len, bool force,
                           ota_update_err_t *e)
{
    if (s_active) {
        ota_update_abort("superseded");
    }
    memset(&s_info, 0, sizeof(s_info));
    s_size = image_size;
    s_written = 0;
    s_t0 = esp_timer_get_time();
    s_part = esp_ota_get_next_update_partition(NULL);
    if (s_part == NULL) {
        set_err(e, 501, "no_ota_slots",
                "this bridge's partition table has no OTA slots: it needs the one wired migration flash (README)");
        record_last(false, e->error, e->message);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_confirm == CONFIRM_WAITING) {
        set_err(e, 409, "pending_verify",
                "the running image is still pending verification; try again once it has confirmed itself");
        record_last(false, e->error, e->message);
        return ESP_ERR_INVALID_STATE;
    }
    const ota_img_result_t r = ota_img_check(head, head_len, image_size, s_part->size, CONFIG_IDF_FIRMWARE_CHIP_ID,
                                             force ? NULL : esp_app_get_description()->project_name, &s_info);
    if (r != OTA_IMG_OK) {
        set_err(e, r == OTA_IMG_TOO_BIG ? 413 : 400, ota_img_result_name(r), "%s", ota_img_result_text(r));
        if (r == OTA_IMG_TOO_BIG) {
            snprintf(e->message, sizeof(e->message), "%u bytes do not fit %s (%u bytes)",
                     (unsigned)image_size, s_part->label, (unsigned)s_part->size);
        } else if (r == OTA_IMG_WRONG_PROJECT) {
            snprintf(e->message, sizeof(e->message), "image is project \"%s\", this bridge runs \"%s\" (?force=1 writes it anyway)",
                     s_info.project_name, esp_app_get_description()->project_name);
        }
        record_last(false, e->error, e->message);
        return ESP_ERR_INVALID_ARG;
    }
    /* Sequential writes: each sector is erased as the data reaches it, so the
       upload does not stall on a multi-second erase before the first byte. */
    const esp_err_t err = esp_ota_begin(s_part, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
    if (err != ESP_OK) {
        set_err(e, err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE ? 409 : 500, "ota_begin", "esp_ota_begin: %s",
                esp_err_to_name(err));
        record_last(false, e->error, e->message);
        return err;
    }
    s_active = true;
    ESP_LOGI(TAG, "writing %u bytes to %s @0x%lx: %s %s (%s %s)", (unsigned)image_size, s_part->label,
             (unsigned long)s_part->address, s_info.project_name, s_info.version, s_info.date, s_info.time);
    return ESP_OK;
}

esp_err_t ota_update_write(const void *data, size_t len, ota_update_err_t *e)
{
    if (!s_active) {
        set_err(e, 500, "not_started", "no upload in progress");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_written + len > s_size) {
        len = s_size - s_written;
    }
    const esp_err_t err = esp_ota_write(s_handle, data, len);
    if (err != ESP_OK) {
        set_err(e, 500, "ota_write", "esp_ota_write at %u: %s", (unsigned)s_written, esp_err_to_name(err));
        ota_update_abort(e->message);
        return err;
    }
    s_written += len;
    return ESP_OK;
}

void ota_update_abort(const char *why)
{
    if (s_active) {
        esp_ota_abort(s_handle);
        s_active = false;
        ESP_LOGW(TAG, "upload to %s aborted after %u of %u bytes: %s", s_part->label, (unsigned)s_written,
                 (unsigned)s_size, why ? why : "");
        record_last(false, "aborted", why);
    }
}

static void add_info(cJSON *o, const ota_img_info_t *i)
{
    cJSON_AddStringToObject(o, "project", i->project_name);
    cJSON_AddStringToObject(o, "version", i->version);
    cJSON_AddStringToObject(o, "date", i->date);
    cJSON_AddStringToObject(o, "time", i->time);
    cJSON_AddStringToObject(o, "idf", i->idf_ver);
}

cJSON *ota_update_end(ota_update_err_t *e)
{
    if (!s_active) {
        set_err(e, 500, "not_started", "no upload in progress");
        return NULL;
    }
    s_active = false;
    if (s_written != s_size) {
        esp_ota_abort(s_handle);
        set_err(e, 400, "short", "upload ended after %u of %u bytes", (unsigned)s_written, (unsigned)s_size);
        record_last(false, e->error, e->message);
        return NULL;
    }
    /* Checksum, SHA-256 and chip revision of the whole image. */
    esp_err_t err = esp_ota_end(s_handle);
    if (err != ESP_OK) {
        set_err(e, err == ESP_ERR_OTA_VALIDATE_FAILED ? 400 : 500, "verify_failed", "esp_ota_end: %s",
                esp_err_to_name(err));
        record_last(false, e->error, e->message);
        return NULL;
    }
    err = esp_ota_set_boot_partition(s_part);
    if (err != ESP_OK) {
        set_err(e, 500, "set_boot", "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        record_last(false, e->error, e->message);
        return NULL;
    }
    record_last(true, NULL, "written and verified; boots on the next restart");
    ESP_LOGI(TAG, "%s verified (%u bytes, %d ms); it boots on the next restart", s_part->label,
             (unsigned)s_size, s_last.ms);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddStringToObject(j, "partition", s_part->label);
    cJSON_AddNumberToObject(j, "address", s_part->address);
    cJSON_AddNumberToObject(j, "size", (double)s_size);
    cJSON_AddNumberToObject(j, "ms", s_last.ms);
    add_info(j, &s_info);
    cJSON_AddStringToObject(j, "boots", "on the next restart, pending verification (rolls back if it does not come up)");
    return j;
}

/* ─────────────────────────────────────────────
 *  Status
 * ───────────────────────────────────────────── */

static const char *state_name(esp_ota_img_states_t s)
{
    switch (s) {
        case ESP_OTA_IMG_NEW:            return "new";
        case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
        case ESP_OTA_IMG_VALID:          return "valid";
        case ESP_OTA_IMG_INVALID:        return "invalid";
        case ESP_OTA_IMG_ABORTED:        return "aborted";
        case ESP_OTA_IMG_UNDEFINED:      return "undefined";
    }
    return "unknown";
}

static const char *confirm_name(confirm_state_t c)
{
    switch (c) {
        case CONFIRM_NOT_NEEDED: return "not_needed";
        case CONFIRM_WAITING:    return "waiting";
        case CONFIRMED:          return "confirmed";
        case CONFIRM_FAILED:     return "failed_rolling_back";
    }
    return "unknown";
}

static void add_label(cJSON *o, const char *key, const esp_partition_t *p)
{
    if (p != NULL) {
        cJSON_AddStringToObject(o, key, p->label);
    } else {
        cJSON_AddNullToObject(o, key);
    }
}

/* The first 8 bytes (16 hex digits) of an image's ELF SHA-256, as idf.py
   prints it (esp_app_get_elf_sha256() stops at CONFIG_APP_RETRIEVE_LEN_ELF_SHA). */
static void elf_sha_prefix(const esp_app_desc_t *d, char out[17])
{
    for (int i = 0; i < 8; i++) {
        snprintf(out + 2 * i, 3, "%02x", d->app_elf_sha256[i]);
    }
}

static cJSON *partition_json(const esp_partition_t *p, const esp_partition_t *running,
                             const esp_partition_t *boot, const esp_partition_t *next)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "label", p->label);
    cJSON_AddNumberToObject(o, "address", p->address);
    cJSON_AddNumberToObject(o, "size", p->size);
    cJSON_AddBoolToObject(o, "running", p == running);
    cJSON_AddBoolToObject(o, "boot", p == boot);
    cJSON_AddBoolToObject(o, "next_update", p == next);
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(p, &st) == ESP_OK) {
        cJSON_AddStringToObject(o, "state", state_name(st));
    } else {
        cJSON_AddNullToObject(o, "state");   /* factory, or a slot otadata does not name */
    }
    esp_app_desc_t d;
    if (esp_ota_get_partition_description(p, &d) == ESP_OK) {
        cJSON *a = cJSON_AddObjectToObject(o, "app");
        cJSON_AddStringToObject(a, "project", d.project_name);
        cJSON_AddStringToObject(a, "version", d.version);
        cJSON_AddStringToObject(a, "date", d.date);
        cJSON_AddStringToObject(a, "time", d.time);
        cJSON_AddStringToObject(a, "idf", d.idf_ver);
        char sha[17];
        elf_sha_prefix(&d, sha);
        cJSON_AddStringToObject(a, "elf_sha256", sha);
    } else {
        cJSON_AddNullToObject(o, "app");
    }
    return o;
}

cJSON *ota_update_status_json(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "supported", next != NULL);
    cJSON_AddStringToObject(j, "fw_version", HARNESS_FW_VERSION);
    const esp_app_desc_t *me = esp_app_get_description();
    cJSON_AddStringToObject(j, "version", me->version);
    cJSON_AddStringToObject(j, "project", me->project_name);
    char sha[17];
    elf_sha_prefix(me, sha);
    cJSON_AddStringToObject(j, "elf_sha256", sha);
    add_label(j, "running", running);
    add_label(j, "boot", boot);
    add_label(j, "next_update", next);
    add_label(j, "last_invalid", invalid);
    cJSON *c = cJSON_AddObjectToObject(j, "confirm");
    cJSON_AddStringToObject(c, "state", confirm_name(s_confirm));
    cJSON_AddNumberToObject(c, "timeout_s", CONFIG_USBIP_OTA_CONFIRM_TIMEOUT_S);
    if (s_confirm != CONFIRM_NOT_NEEDED) {      /* the checks only run for a new image */
        cJSON_AddBoolToObject(c, "app_started", s_app_started);
        cJSON_AddBoolToObject(c, "network_up", s_seen_ip);
        cJSON_AddBoolToObject(c, "http_up", s_seen_http);
    }
    if (s_confirm == CONFIRMED) {
        cJSON_AddNumberToObject(c, "confirmed_after_ms", (double)s_confirm_ms);
    }
    cJSON *parts = cJSON_AddArrayToObject(j, "partitions");
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it != NULL; it = esp_partition_next(it)) {
        cJSON_AddItemToArray(parts, partition_json(esp_partition_get(it), running, boot, next));
    }
    esp_partition_iterator_release(it);
    if (s_active) {
        cJSON *u = cJSON_AddObjectToObject(j, "upload");
        cJSON_AddStringToObject(u, "partition", s_part->label);
        cJSON_AddNumberToObject(u, "size", (double)s_size);
        cJSON_AddNumberToObject(u, "written", (double)s_written);
    }
    if (s_last.any) {
        cJSON *l = cJSON_AddObjectToObject(j, "last_upload");
        cJSON_AddBoolToObject(l, "ok", s_last.ok);
        cJSON_AddStringToObject(l, "partition", s_last.partition);
        cJSON_AddNumberToObject(l, "size", (double)s_last.size);
        cJSON_AddNumberToObject(l, "written", (double)s_last.written);
        cJSON_AddNumberToObject(l, "ms", s_last.ms);
        cJSON_AddStringToObject(l, "version", s_last.version);
        if (!s_last.ok) {
            cJSON_AddStringToObject(l, "error", s_last.error);
        }
        cJSON_AddStringToObject(l, "message", s_last.message);
    }
    return j;
}

void ota_update_add_summary(cJSON *obj)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    add_label(obj, "running_partition", running);
    esp_ota_img_states_t st;
    if (running != NULL && esp_ota_get_state_partition(running, &st) == ESP_OK) {
        cJSON_AddStringToObject(obj, "ota_state", state_name(st));
    } else {
        cJSON_AddNullToObject(obj, "ota_state");
    }
}

esp_err_t ota_update_set_boot(const char *label, ota_update_err_t *e)
{
    if (label == NULL || label[0] == '\0') {
        set_err(e, 400, "partition", "give {\"partition\":\"factory|ota_0|ota_1\"}");
        return ESP_ERR_INVALID_ARG;
    }
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label);
    if (p == NULL) {
        set_err(e, 404, "no_partition", "no app partition \"%s\"", label);
        return ESP_ERR_NOT_FOUND;
    }
    if (s_active) {
        set_err(e, 409, "uploading", "an upload is in progress");
        return ESP_ERR_INVALID_STATE;
    }
    /* Verifies the image in p first; for an OTA slot it boots pending
       verification (rollback applies), factory clears otadata. */
    const esp_err_t err = esp_ota_set_boot_partition(p);
    if (err != ESP_OK) {
        set_err(e, err == ESP_ERR_OTA_VALIDATE_FAILED ? 409 : 500,
                err == ESP_ERR_OTA_VALIDATE_FAILED ? "no_valid_image" : "set_boot",
                "esp_ota_set_boot_partition(%s): %s", label, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "boot partition set to %s", label);
    return ESP_OK;
}
