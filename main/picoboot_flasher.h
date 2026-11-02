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

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "rescue_job.h"

int picoboot_flasher_job(rescue_job_t *job, const char *busid, const cJSON *args);
/* Enter BOOTSEL if needed and report the chip, PICOBOOT interface and flash. */
int picoboot_info_job(rescue_job_t *job, const char *busid, const cJSON *args);

/* A connection for other modules (the DUT flash browser): BOOTSEL (entered
   unless enter_bootsel is false), exclusive access, XIP off.  Addresses are
   XIP addresses (0x10000000..).  NULL when it failed (the job says why). */
typedef struct pb_session pb_session_t;
pb_session_t *picoboot_open(rescue_job_t *job, const char *busid, const cJSON *args);
bool picoboot_is_rp2350(const pb_session_t *p);
int picoboot_read(pb_session_t *p, uint32_t addr, uint8_t *buf, uint32_t len);
/* Erase, write and read back whole 4 KB sectors. */
int picoboot_write(pb_session_t *p, uint32_t addr, const uint8_t *buf, uint32_t len);
/* PC_GET_INFO (RP2350 only): bType, dParams, `len` bytes of answer. */
int picoboot_get_info(pb_session_t *p, uint8_t type, const uint32_t params[3], uint8_t *buf, uint32_t len);
void picoboot_close(pb_session_t *p, bool reboot);

#endif
