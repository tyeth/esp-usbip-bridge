#ifndef DEVICE_RESCUE_H
#define DEVICE_RESCUE_H

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

#include "rescue_logic.h"
#include "serial_link.h"
#include "usb_backend.h"

/*
 * Per-device "interrogate and rescue" tools, run by the bridge itself with
 * no host tools: identity and interfaces from the cached descriptors, serial
 * line control (CDC-ACM, CP210x, CH34x, FTDI), canned reset sequences (ESP
 * classic and USB-Serial/JTAG, 1200-baud touch, RP2040/RP2350 reset
 * interface and PICOBOOT) and a short console peek.  Port power is reported,
 * never changed.  Every action is refused (409) while a USB/IP client has
 * the device imported, and an import is refused while an action runs.
 *
 * Each call returns an HTTP status and sets *out to the JSON body.  They run
 * on the HTTP server task (one request at a time), which they block for at
 * most about 12 s (the console capture limit).
 */

/* GET: identity, interfaces, owner, port power, serial capabilities and the
   reset modes that apply.  No bus traffic. */
int device_rescue_info(const char *busid, cJSON **out);

/* POST {port?, baud?, dtr?, rts?}: set line coding and/or DTR/RTS. */
int device_rescue_serial(const char *busid, const cJSON *args, cJSON **out);

/* POST {mode, port?, wait_ms?}: run a reset sequence (see GET .../rescue for
   the modes; "bootloader" picks the one that fits). */
int device_rescue_reset(const char *busid, const cJSON *args, cJSON **out);

/* POST {port?, serial_baud?, pokes?, settle_s?, capture_s?, dtr?, rts?,
   max_bytes?}: open the serial function, send pokes, capture the output. */
int device_rescue_console(const char *busid, const cJSON *args, cJSON **out);

/* For the flashing jobs: run a DTR/RTS reset table on an open link (failures
   from its commit step on set *dropped instead of failing), and wait up to
   wait_ms for busid to come back with a new devnum (*now is what came back). */
int device_rescue_run_line_seq(serial_link_t *l, rescue_mode_t mode, bool *dropped);
bool device_rescue_wait_reenumeration(const char *busid, uint32_t devnum, int wait_ms,
                                      usbip_backend_device_t *now, bool *dropped);

#endif
