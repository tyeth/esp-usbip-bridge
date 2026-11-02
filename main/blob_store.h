#ifndef BLOB_STORE_H
#define BLOB_STORE_H

/*
 * Staged files for device rescue, held in PSRAM: firmware images to flash,
 * UF2 files and files to copy to or read from a device's mass storage.
 * Uploaded with PUT /api/rescue/files/{name} (streamed by http_server.c),
 * downloaded with GET, listed and deleted through the controller API.
 * Contents are lost on reboot.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#define BLOB_NAME_MAX   64
/* Staged files together, at most (PSRAM permitting) */
#define BLOB_TOTAL_MAX  (12u * 1024 * 1024)

/* One PSRAM budget for staged files and serial capture buffers: a capture
   reserves its ring here so neither can starve the other.  reserve returns
   the PSRAM (or NULL when it does not fit); release frees it. */
void *blob_reserve(size_t size, const char *owner);
void blob_unreserve(void *buf, size_t size);

bool blob_name_valid(const char *name);
/* Bytes that one more blob may take (PSRAM headroom and the store cap). */
size_t blob_space(void);

/* Allocate `size` bytes for `name` (replacing an existing blob of that name
   once committed).  ESP_ERR_INVALID_STATE while the old one is in use,
   ESP_ERR_NO_MEM when it does not fit. */
esp_err_t blob_begin(const char *name, size_t size, uint8_t **buf);
void blob_commit(const char *name, size_t len);
void blob_abort(const char *name);
/* Store a copy (job outputs). */
esp_err_t blob_put(const char *name, const uint8_t *data, size_t len);
/* Take the bytes (they stay valid and unchanged until blob_release). */
bool blob_acquire(const char *name, const uint8_t **data, size_t *len);
void blob_release(const char *name);
esp_err_t blob_delete(const char *name);
cJSON *blob_list_json(void);

#endif
