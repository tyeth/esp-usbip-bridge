#ifndef SAMBA_FLASHER_H
#define SAMBA_FLASHER_H

/*
 * bossac, from the bridge: program a SAMD21/SAMD51 (and SAME5x) through a
 * SAM-BA bootloader over USB CDC, the way BOSSA 1.9 does with the Arduino
 * extensions that the Arduino and Adafruit SAMD bootloaders advertise
 * ("[Arduino:XYZ]" in the V# answer): X = erase from an offset, Y = copy a
 * RAM buffer to flash, Z = CRC16 of a flash range (verify).  The board is
 * sent to its bootloader with the 1200-baud touch when it is running an
 * app.  Runs as a rescue job.
 *
 * Job args: {"file": "<staged .bin>", "offset": "0x2000" (default: 0x2000 on
 *            SAMD21, 0x4000 on SAMD51), "erase": true, "verify": true,
 *            "reset": true, "enter_bootloader": true}
 */

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "rescue_job.h"

int samba_flasher_job(rescue_job_t *job, const char *busid, const cJSON *args);
/* Connect and report the bootloader version, chip and flash geometry. */
int samba_info_job(rescue_job_t *job, const char *busid, const cJSON *args);

/* A connection for other modules (the DUT flash browser): connected to the
   SAM-BA monitor (1200-baud touch first unless enter_bootloader is false).
   Read-only: the Arduino X command erases from an address to the end of the
   flash, so there is no sector rewrite.  NULL when it failed. */
typedef struct samba samba_t;
samba_t *samba_open(rescue_job_t *job, const char *busid, const cJSON *args);
void samba_geometry(const samba_t *s, uint32_t *flash_size, uint32_t *page_size, uint32_t *app_offset,
                    const char **family);
int samba_read(samba_t *s, uint32_t addr, uint8_t *buf, uint32_t len);
/* reset: SYSRESETREQ back to the app. */
void samba_close(samba_t *s, bool reset);

#endif
