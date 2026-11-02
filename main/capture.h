#ifndef CAPTURE_H
#define CAPTURE_H

/*
 * Serial capture: the bridge records the UART output of its devices into
 * PSRAM so it can be read later, downloaded as a timestamped log, or
 * streamed live (TCP 3241), without a USB/IP client attached.  HIL runs can
 * then flash, reset and watch a DUT through the bridge alone.
 *
 * - Per busid, one task captures every serial function of the device (each
 *   CDC function of a composite device, each port of a CP2105/FT2232) into
 *   its own ring.  It follows the device through resets and
 *   re-enumeration on the same port path, including a different VID:PID
 *   (bootloader vs app) or a different number of ports, and writes a
 *   timestamped marker for each epoch start, drop, return and baud change.
 * - Buffers: per port `size` (default capture.default_size), all captures
 *   together at most capture.total_limit, and both drawn from the PSRAM
 *   budget shared with the staged files, so one device can take all of it.
 * - Timestamps: bridge uptime in ms per received chunk; every answer
 *   carries uptime_ms so a client can convert to wall-clock time.
 * - Baud: a fixed rate, a list to try in order, or "auto" (UART bridges
 *   only: native USB CDC ignores the rate).  The rates are scored on the
 *   text received and the first good one is kept; a stream whose output
 *   turns to garbage (the ROM vs the app rate) is probed again.
 * - Arming: an armed port is captured whenever a device is on it, also
 *   after a bridge reboot (NVS) and after a rescue job releases it.  A
 *   running capture holds the device for rescue: USB/IP imports are
 *   refused meanwhile; line and reset actions still work.
 * - Sources: "usb" (the device's USB serial functions).  "gpio_uart" (a
 *   bridge UART on GPIO pins, for DUTs without USB serial) is part of the
 *   API shape and answers 501 until implemented.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#define CAPTURE_STREAM_PORT 3241

void capture_init(void);

/* POST {size?, baud?: int | [int...] | "auto", dtr?, rts?, arm?, source?} */
int capture_start(const char *busid, const cJSON *args, cJSON **out);
/* POST {disarm?, clear?} */
int capture_stop(const char *busid, const cJSON *args, cJSON **out);
/* GET: output of one port since a cursor; lines adds [{t_ms, text}]. */
int capture_poll(const char *busid, int port, uint64_t since, int max, bool lines, cJSON **out);
/* POST {port?, text? | hex?} */
int capture_send(const char *busid, int port, const cJSON *args, cJSON **out);
/* GET /api/capture: limits, budget, armed ports and every capture. */
cJSON *capture_status_json(void);
/* POST /api/capture/config {total_limit?, default_size?} (saved in NVS) */
int capture_set_config(const cJSON *args, cJSON **out);

bool capture_active(const char *busid);
/* Stop a running capture (armed ports are restarted by the watcher later). */
void capture_halt(const char *busid);

/* Render a port's output from *cursor as text, at most cap-1 bytes, with
   "[  12.345] " uptime prefixes at line starts when timestamps is set.
   Advances *cursor.  Returns the length (0 when nothing new), -1 when there
   is no such capture.  *dropped is set when the cursor fell behind the ring. */
int capture_render(const char *busid, int port, uint64_t *cursor, bool timestamps,
                   char *out, size_t cap, bool *dropped);
/* Where the current epoch of a port starts, for "from=epoch" readers. */
bool capture_cursor_info(const char *busid, int port, uint64_t *oldest, uint64_t *epoch_start, uint64_t *total);

/* The TCP live stream server (capture_stream.c). */
void capture_stream_start(void);

#endif
