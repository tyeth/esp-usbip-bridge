#ifndef SERIAL_LINK_H
#define SERIAL_LINK_H

/*
 * A device's serial function (CDC-ACM, CP210x, CH34x, FTDI) driven by the
 * bridge itself through its USB host backend: line coding, DTR/RTS, and a
 * byte stream with a persistent bulk IN and coalesced bulk OUT.  Used by the
 * console peek, the serial monitor, the ESP flasher port and SAM-BA.
 *
 * One link is used by one task at a time.  The baud/DTR/RTS last set on a
 * device are kept per enumeration (serial_link_last_set_json) because most
 * chips cannot read them back.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "rescue_logic.h"
#include "usb_backend.h"

#define SERIAL_LINK_IN_CHUNK    512     /* a multiple of every FS bulk MPS */
#define SERIAL_LINK_TX_MAX      4096    /* CONFIG_USBIP_MAX_TRANSFER: one bulk OUT */

typedef struct {
    char busid[32];
    uint32_t devnum;
    uint16_t vid, pid;
    int port;                   /* serial port / CDC function index */
    rescue_config_t cfg;
    rescue_caps_t caps;
    cJSON *log;                 /* optional: control requests are appended here */
    bool keep_claims;           /* close does not release the device's interfaces (a sibling
                                   port of a composite device is still reading) */

    /* persistent bulk IN */
    usb_backend_req_t in_req;
    SemaphoreHandle_t in_sem;
    bool in_pending;
    int in_status;              /* result of the last completed IN */
    size_t in_done_len;
    uint8_t in_buf[SERIAL_LINK_IN_CHUNK];
    size_t rx_off, rx_len;      /* unread bytes in in_buf */
    uint32_t claim_failures_at_open;

    /* coalesced bulk OUT */
    uint8_t *tx;
    size_t tx_len;
} serial_link_t;

/* Look the device up and classify it (no bus traffic).  Returns 0, or an
   HTTP-style status (404 absent, 409 virtual / no serial function) with
   msg set. */
int serial_link_open(serial_link_t *l, const char *busid, int port, char *msg, size_t msg_len);
/* Re-attach to the same busid after the device re-enumerated (any pending
   IN is dropped).  Returns 0 or 404. */
int serial_link_reopen(serial_link_t *l, char *msg, size_t msg_len);
/* Cancel the pending IN, release the claimed interfaces. */
void serial_link_close(serial_link_t *l);

/* CP210x enable / CH34x setup (once per enumeration); call before line or
   data traffic.  Returns 0 or a negative errno. */
int serial_link_prepare(serial_link_t *l);
/* Start a new data session with both bulk toggles at DATA0 on both sides:
   CLEAR_FEATURE(ENDPOINT_HALT) on the bulk IN and OUT endpoints.  Each
   session releases its interfaces at close, so the next claim starts the
   host's pipes at DATA0 while the device's toggles carry on; the first
   packet each way can then look like a retransmission and be dropped.
   Call it before the first bulk transfer of a link that owns the device
   (not on one shared with a running capture: it would flush that capture's
   IN).  Returns 0 or the first failing request's errno. */
int serial_link_sync_toggles(serial_link_t *l);
int serial_link_set_baud(serial_link_t *l, uint32_t baud);
int serial_link_set_lines(serial_link_t *l, bool dtr, bool rts);
/* The DTR/RTS last set by the bridge (false when never set). */
void serial_link_get_lines(const serial_link_t *l, bool *dtr, bool *rts);
int serial_link_ctrl(serial_link_t *l, const rescue_ctrl_t *r, uint8_t *in, size_t in_cap, size_t *in_len);

/* Queue bytes for the device; sent in bulk OUTs of up to 4 KB when the
   buffer fills, on flush, and before every read. */
int serial_link_write(serial_link_t *l, const uint8_t *data, size_t len);
int serial_link_flush(serial_link_t *l, uint32_t timeout_ms);
/* Up to cap bytes, waiting at most timeout_ms for the first.  Returns the
   count (0 on timeout) or a negative errno (the device went away, ...). */
int serial_link_read(serial_link_t *l, uint8_t *buf, size_t cap, uint32_t timeout_ms);
/* Drop bytes already received (the IN stays posted). */
void serial_link_discard_input(serial_link_t *l);
/* A failed read or write, explained (names host-channel exhaustion). */
void serial_link_explain(const serial_link_t *l, int st, const char *what, char *msg, size_t msg_len);

/* {baud, dtr, rts, note} for a busid at its current enumeration. */
cJSON *serial_link_last_set_json(const char *busid, uint32_t devnum);

const char *serial_link_errno_name(int st);

#endif
