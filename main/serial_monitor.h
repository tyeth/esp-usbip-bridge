#ifndef SERIAL_MONITOR_H
#define SERIAL_MONITOR_H

/*
 * Serial monitor: a task per monitored device keeps reading its serial
 * function into a 64 KB ring buffer that clients poll with a cursor, and
 * sends what they type.  It follows the device through a reset or a
 * re-enumeration (same busid), so a boot log after a reset is captured.
 * The device stays reserved for rescue while it runs (USB/IP imports are
 * refused); line and reset actions still work on it, a console peek or a
 * job stops it.  At most two monitors run at once.
 */

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

/* POST {serial_baud?, dtr?, rts?, port?}: start (or restart) the monitor. */
int serial_monitor_start(const char *busid, const cJSON *args, cJSON **out);
/* POST: stop it. */
int serial_monitor_stop_api(const char *busid, cJSON **out);
/* GET ?since=<cursor>&max=<bytes>: new output since the cursor. */
int serial_monitor_poll(const char *busid, uint64_t since, int max, cJSON **out);
/* POST {text?, hex?}: send bytes to the device. */
int serial_monitor_send(const char *busid, const cJSON *args, cJSON **out);

void serial_monitor_stop(const char *busid);
bool serial_monitor_active(const char *busid);
/* The owner tag a running monitor holds the device under. */
#define SERIAL_MONITOR_OWNER "rescue:monitor"

#endif
