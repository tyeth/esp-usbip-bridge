#ifndef BUSID_OWNER_H
#define BUSID_OWNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Who is using a device: a USB/IP client that imported it, or the bridge's
 * own rescue tools (line control, resets, console peek).  The two exclude
 * each other: rescue actions are refused while a client has the device
 * imported, and an import is answered "busy" while a rescue action runs.
 * Several USB/IP sessions on one busid are counted, as before.
 */

typedef enum {
    BUSID_OWNER_NONE = 0,
    BUSID_OWNER_USBIP,
    BUSID_OWNER_RESCUE,
} busid_owner_kind_t;

typedef struct {
    busid_owner_kind_t kind;
    int sessions;               /* USB/IP sessions on the busid */
    char peer[48];              /* address of the (latest) USB/IP client, or the rescue action */
    int64_t since_us;           /* esp_timer clock when the owner took it */
} busid_owner_info_t;

/* Take the busid for `kind`.  USB/IP sessions stack; a rescue action is
   exclusive.  `what` is the client address or the action name.  Returns
   false (with the current owner in `current`, may be NULL) when held by the
   other kind. */
bool busid_owner_try_begin(const char *busid, busid_owner_kind_t kind, const char *what,
                           busid_owner_info_t *current);
void busid_owner_end(const char *busid, busid_owner_kind_t kind);
/* The current owner; false when nobody holds the busid. */
bool busid_owner_get(const char *busid, busid_owner_info_t *out);

#endif
