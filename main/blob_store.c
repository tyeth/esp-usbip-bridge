#include "blob_store.h"

#include <ctype.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define BLOB_SLOTS          16
#define BLOB_TOTAL_MAX      (12u * 1024 * 1024)
#define BLOB_PSRAM_RESERVE  (1024u * 1024)  /* left for everything else */

typedef struct {
    char name[BLOB_NAME_MAX];
    uint8_t *data;
    size_t len;
    int refs;
    bool pending;               /* being uploaded: not listed, not readable */
    int64_t created_us;
} blob_t;

static blob_t s_blobs[BLOB_SLOTS];
static size_t s_reserved;           /* capture buffers (blob_reserve) */
static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_init_lock = portMUX_INITIALIZER_UNLOCKED;

static void lock(void)
{
    if (s_mutex == NULL) {
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        taskENTER_CRITICAL(&s_init_lock);
        if (s_mutex == NULL) {
            s_mutex = m;
            m = NULL;
        }
        taskEXIT_CRITICAL(&s_init_lock);
        if (m != NULL) {
            vSemaphoreDelete(m);
        }
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mutex);
}

bool blob_name_valid(const char *name)
{
    if (name == NULL || name[0] == '\0' || name[0] == '.' || strlen(name) >= BLOB_NAME_MAX) {
        return false;
    }
    for (const char *p = name; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-') {
            return false;
        }
    }
    return true;
}

static blob_t *find_locked(const char *name, bool pending)
{
    for (int i = 0; i < BLOB_SLOTS; i++) {
        if (s_blobs[i].data != NULL && s_blobs[i].pending == pending && strcmp(s_blobs[i].name, name) == 0) {
            return &s_blobs[i];
        }
    }
    return NULL;
}

static size_t used_locked(void)
{
    size_t n = s_reserved;
    for (int i = 0; i < BLOB_SLOTS; i++) {
        if (s_blobs[i].data != NULL) {
            n += s_blobs[i].len;
        }
    }
    return n;
}

static size_t space_locked(void)
{
    const size_t used = used_locked();
    const size_t cap_left = used < BLOB_TOTAL_MAX ? BLOB_TOTAL_MAX - used : 0;
    const size_t free_ps = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    const size_t ps_left = free_ps > BLOB_PSRAM_RESERVE ? free_ps - BLOB_PSRAM_RESERVE : 0;
    return cap_left < ps_left ? cap_left : ps_left;
}

void *blob_reserve(size_t size, const char *owner)
{
    (void)owner;
    lock();
    void *buf = NULL;
    if (size > 0 && size <= space_locked()) {
        buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf != NULL) {
            s_reserved += size;
        }
    }
    unlock();
    return buf;
}

void blob_unreserve(void *buf, size_t size)
{
    if (buf == NULL) {
        return;
    }
    lock();
    heap_caps_free(buf);
    s_reserved = s_reserved >= size ? s_reserved - size : 0;
    unlock();
}

size_t blob_space(void)
{
    lock();
    const size_t n = space_locked();
    unlock();
    return n;
}

esp_err_t blob_begin(const char *name, size_t size, uint8_t **buf)
{
    if (!blob_name_valid(name) || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    const blob_t *old = find_locked(name, false);
    if ((old != NULL && old->refs > 0) || find_locked(name, true) != NULL) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (size > space_locked() + (old ? old->len : 0)) {
        unlock();
        return ESP_ERR_NO_MEM;
    }
    blob_t *b = NULL;
    for (int i = 0; i < BLOB_SLOTS && b == NULL; i++) {
        if (s_blobs[i].data == NULL) {
            b = &s_blobs[i];
        }
    }
    uint8_t *data = b ? heap_caps_malloc(size, MALLOC_CAP_SPIRAM) : NULL;
    if (data == NULL) {
        unlock();
        return ESP_ERR_NO_MEM;
    }
    memset(b, 0, sizeof(*b));
    strlcpy(b->name, name, sizeof(b->name));
    b->data = data;
    b->len = size;
    b->pending = true;
    unlock();
    *buf = data;
    return ESP_OK;
}

void blob_commit(const char *name, size_t len)
{
    lock();
    blob_t *b = find_locked(name, true);
    if (b != NULL) {
        blob_t *old = find_locked(name, false);
        if (old != NULL) {          /* refs == 0: checked in blob_begin, only the HTTP task replaces */
            heap_caps_free(old->data);
            memset(old, 0, sizeof(*old));
        }
        b->len = len;
        b->pending = false;
        b->created_us = esp_timer_get_time();
    }
    unlock();
}

void blob_abort(const char *name)
{
    lock();
    blob_t *b = find_locked(name, true);
    if (b != NULL) {
        heap_caps_free(b->data);
        memset(b, 0, sizeof(*b));
    }
    unlock();
}

esp_err_t blob_put(const char *name, const uint8_t *data, size_t len)
{
    uint8_t *buf = NULL;
    const esp_err_t err = blob_begin(name, len ? len : 1, &buf);
    if (err != ESP_OK) {
        return err;
    }
    memcpy(buf, data, len);
    blob_commit(name, len);
    return ESP_OK;
}

bool blob_acquire(const char *name, const uint8_t **data, size_t *len)
{
    lock();
    blob_t *b = find_locked(name, false);
    if (b != NULL) {
        b->refs++;
        *data = b->data;
        *len = b->len;
    }
    unlock();
    return b != NULL;
}

void blob_release(const char *name)
{
    lock();
    blob_t *b = find_locked(name, false);
    if (b != NULL && b->refs > 0) {
        b->refs--;
    }
    unlock();
}

esp_err_t blob_delete(const char *name)
{
    lock();
    blob_t *b = find_locked(name, false);
    esp_err_t err = ESP_OK;
    if (b == NULL) {
        err = ESP_ERR_NOT_FOUND;
    } else if (b->refs > 0) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        heap_caps_free(b->data);
        memset(b, 0, sizeof(*b));
    }
    unlock();
    return err;
}

cJSON *blob_list_json(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(j, "files");
    const int64_t now = esp_timer_get_time();
    lock();
    for (int i = 0; i < BLOB_SLOTS; i++) {
        const blob_t *b = &s_blobs[i];
        if (b->data == NULL || b->pending) {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", b->name);
        cJSON_AddNumberToObject(o, "size", (double)b->len);
        cJSON_AddNumberToObject(o, "age_s", (double)((now - b->created_us) / 1000000));
        cJSON_AddBoolToObject(o, "in_use", b->refs > 0);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddNumberToObject(j, "used", (double)used_locked());
    cJSON_AddNumberToObject(j, "capture_reserved", (double)s_reserved);
    cJSON_AddNumberToObject(j, "space", (double)space_locked());
    unlock();
    cJSON_AddStringToObject(j, "note", "held in PSRAM, lost on reboot");
    return j;
}
