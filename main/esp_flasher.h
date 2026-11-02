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

#endif
