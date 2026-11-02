#ifndef PICOBOOT_FLASHER_H
#define PICOBOOT_FLASHER_H

/*
 * picotool load, from the bridge: write a UF2 (or a raw .bin at an address)
 * to an RP2040/RP2350 over its PICOBOOT interface, verify it by reading it
 * back and reboot.  A device that is not in BOOTSEL is sent there first
 * (reset interface, else the 1200-baud touch).  Runs as a rescue job.
 *
 * Job args: {"file": "<staged name>", "address": "0x10000000" (raw .bin only),
 *            "verify": true, "reboot": true, "enter_bootsel": true,
 *            "force_family": false}
 */

#include "cJSON.h"
#include "rescue_job.h"

int picoboot_flasher_job(rescue_job_t *job, const char *busid, const cJSON *args);
/* Enter BOOTSEL if needed and report the chip, PICOBOOT interface and flash. */
int picoboot_info_job(rescue_job_t *job, const char *busid, const cJSON *args);

#endif
