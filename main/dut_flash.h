#ifndef DUT_FLASH_H
#define DUT_FLASH_H

/*
 * A device's own flash, through its bootloader: the partitions or regions
 * the bootloader can reach, reading them into staged files, and the FAT,
 * LittleFS or SPIFFS inside them (list, read, write, delete, rename, mkdir,
 * format).  ESP over esp-serial-flasher (partition table at 0x8000), RP2040
 * and RP2350 over PICOBOOT (the RP2350 partition table, arduino-pico's
 * layout words, or a scan for file systems), SAMD over SAM-BA (internal
 * flash only, read-only).
 *
 * A session is one rescue job ("dut_flash"): it connects once, maps the
 * flash, runs the ops it was given and, with keep_open_s, waits for more
 * (another POST adds ops to the open session).  File system changes are made
 * in a RAM shadow of the region and written back sector by sector (read,
 * modify, erase, write) at the end of each op that changes something.
 *
 * POST body: {"ops": [{"op": ...}, ...], "platform": "esp|rp2|samd" (auto
 * from the USB identity when clear), "keep_open_s": 0..900, "after":
 * "hard_reset|no_reset", plus the flasher's connect args (baud, before,
 * flash_size for RP2040).  Ops:
 *   partitions                                    re-map the flash
 *   read_region  {part | offset+size, file?}      to a staged file
 *   fs_info      {part}                           file system, usage, root listing
 *   list         {part, path}
 *   read         {part, path, file?}              file to a staged file
 *   write        {part, path, file | text}        staged file or text to a file
 *   delete       {part, path}   mkdir {part, path}   rename {part, from, to}
 *   format       {part, fs: littlefs|fat|spiffs, confirm (when not blank)}
 * part: a region name from the map, or its index.  Results are appended to
 * the job's result.ops; the map is result.regions.
 */

#include "cJSON.h"
#include "rescue_job.h"

/* POST /api/usb/devices/{busid}/dutflash: start a session, or add the ops
   to the one open on busid.  Returns an HTTP status (202) and *out. */
int dut_flash_request(const char *busid, const cJSON *args, cJSON **out);
/* GET: the open session on busid, if any. */
int dut_flash_status(const char *busid, cJSON **out);
/* POST .../dutflash/close: end the session (the device is reset per "after"). */
int dut_flash_close(const char *busid, cJSON **out);
/* The job (also startable as rescue job kind "dut_flash"). */
int dut_flash_job(rescue_job_t *job, const char *busid, const cJSON *args);

#endif
