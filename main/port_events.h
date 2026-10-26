#ifndef PORT_EVENTS_H
#define PORT_EVENTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Port state transition log (manual / external / device changes).

   Hub port snapshots are polled and diffed. Every change is attributed:
     "api"      a power change the API commanded on this port within the action window
                (via "restore" when it was the saved-state restore, never mistaken for manual)
     "device"   a connection or enable change with no commanded cause (replug, drop)
     "external" a power change the API did not command (hand-toggled switch, upstream
                glitch), or an over-current change */

#define PORT_EVENT_LOG_LEN 64

typedef enum {
    PORT_EVENT_SRC_API = 0,     /* an API power command for the port */
    PORT_EVENT_SRC_RESTORE,     /* the saved-state restore policy */
} port_event_src_t;

typedef struct {
    uint32_t t_ms;              /* bridge uptime */
    char port[32];
    char what[16];              /* power_on, power_off, connected, disconnected, enabled, disabled, overcurrent, overcurrent_cleared */
    char from[8];
    char to[8];
    char attribution[12];       /* api, device, external */
    char via[12];               /* "restore" for auto-restore, else empty */
    bool mismatch;              /* the port's power differs from its saved state after this change */
} port_event_t;

typedef struct {
    uint32_t t_ms;
    char what[16];
    char attribution[12];
} port_last_change_t;

void port_events_init(void);

/* Record that the API (or the restore policy) is about to change this port's power.
   Power changes on the port within the action window are attributed to it. */
void port_events_note_action(const char *port_path, port_event_src_t src);

/* Newest first. Returns the number written (at most max). */
size_t port_events_get(port_event_t *out, size_t max);

void port_events_counters(uint32_t *external_power_changes, uint32_t *device_replugs);

/* The most recent change on a port. False when none has been seen. */
bool port_events_last_change(const char *port_path, port_last_change_t *out);

#endif
