#ifndef ESP_FLASHER_H
#define ESP_FLASHER_H

/*
 * Flash an ESP chip from the bridge (esptool write-flash, no host tools):
 * esp-serial-flasher with a port over the device's USB serial function
 * (USB-Serial/JTAG, or a CP210x/CH34x/FTDI/CDC bridge with the DTR/RTS
 * auto-reset circuit).  Runs as a rescue job.
 *
 * Job args: {"images": [{"file": "<staged name>", "offset": 65536 | "0x10000"}],
 *            "before": "default_reset" | "usb_reset" | "no_reset",
 *            "after": "hard_reset" | "no_reset",
 *            "baud": 460800, "erase_all": false, "verify": true,
 *            "stub": true, "port": 0}
 */

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "rescue_job.h"

int esp_flasher_job(rescue_job_t *job, const char *busid, const cJSON *args);
/* Connect and report chip, MAC, flash size, security info (no writes). */
int esp_flasher_info_job(rescue_job_t *job, const char *busid, const cJSON *args);
/* Within another job: start an ESP32-S2/S3 board's TinyUF2.  Its partition
   table has the UF2 bootloader as the factory app, which the second-stage
   bootloader runs when otadata is blank: connect, check the table, erase
   otadata, reset.  TinyUF2 selects the new app again after a UF2 copy. */
int esp_flasher_boot_uf2(rescue_job_t *job, const char *busid);

/* A connection for other modules (the DUT flash browser): connect as the
   flash job does (same args: before, baud, stub, port), then read and write
   the flash until closed.  NULL when it failed (the job says why). */
typedef struct espf espf_t;
espf_t *esp_flasher_open(rescue_job_t *job, const char *busid, const cJSON *args, bool *encrypted);
uint32_t esp_flasher_flash_size(const espf_t *f);
const char *esp_flasher_chip(const espf_t *f);
int esp_flasher_read(espf_t *f, uint32_t addr, uint8_t *buf, uint32_t len);
/* Erase the 4 KB sectors [addr, addr + len) and write them, MD5-verified. */
int esp_flasher_write(espf_t *f, rescue_job_t *job, uint32_t addr, const uint8_t *buf, uint32_t len);
/* "after" as the flash job takes it (hard_reset by default), then free. */
void esp_flasher_close(espf_t *f, rescue_job_t *job, const cJSON *args);

#endif
