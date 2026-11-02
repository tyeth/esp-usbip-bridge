#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

/*
 * Firmware updates over the network: an app image streamed into the next OTA
 * slot (PUT /api/ota, http_server.c), slot and image status (GET /api/ota)
 * and the first-boot check that confirms a new image or rolls it back.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

typedef struct {
    int status;             /* HTTP status for a failure */
    const char *error;      /* short machine-readable reason */
    char message[160];
} ota_update_err_t;

/* Call in app_main once esp_netif is initialised: if the running image is pending verification,
   start the task that confirms it once the bridge is up (app_main finished,
   a network address, its own HTTP server answering GET /ping) or rolls it
   back CONFIG_USBIP_OTA_CONFIRM_TIMEOUT_S after boot. */
void ota_update_boot_check_start(void);
/* Call at the end of app_main, once every service has started. */
void ota_update_app_started(void);

/* Start an upload of image_size bytes.  head holds the image's first bytes
   (at least OTA_IMG_CHECK_LEN, or all of a smaller image); they are checked
   but not written: pass them to ota_update_write() as well.  force skips the
   project-name check. */
esp_err_t ota_update_begin(size_t image_size, const uint8_t *head, size_t head_len, bool force,
                           ota_update_err_t *e);
esp_err_t ota_update_write(const void *data, size_t len, ota_update_err_t *e);
/* Verify the written image and make it the boot partition.  Returns the
   result object (caller frees) or NULL with *e set. */
cJSON *ota_update_end(ota_update_err_t *e);
void ota_update_abort(const char *why);

/* GET /api/ota */
cJSON *ota_update_status_json(void);
/* POST /api/ota/boot: boot "factory", "ota_0" or "ota_1" next. */
esp_err_t ota_update_set_boot(const char *label, ota_update_err_t *e);
/* For /api/info: running partition label and its OTA state. */
void ota_update_add_summary(cJSON *obj);

#endif
