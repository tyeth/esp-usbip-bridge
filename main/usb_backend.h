#ifndef USB_BACKEND_H
#define USB_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "usb/usb_types_ch9.h"

#include "usbip_protocol.h"

typedef struct {
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
} usbip_backend_interface_t;

#include "sdkconfig.h"

#define USBIP_MAX_ENDPOINTS CONFIG_USBIP_MAX_ENDPOINTS

typedef struct {
    uint8_t address;
    uint8_t attributes;
    uint16_t max_packet_size;
    uint8_t interval;
} usbip_backend_endpoint_t;

typedef struct {
    bool present;
    char path[256];
    char busid[32];
    uint32_t busnum;
    uint32_t devnum;
    uint32_t speed;
    uint16_t id_vendor;
    uint16_t id_product;
    uint16_t bcd_device;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t configuration_value;
    uint8_t num_configurations;
    uint8_t num_interfaces;
    usbip_backend_interface_t interfaces[USBIP_MAX_INTERFACES];
    uint8_t num_endpoints;
    usbip_backend_endpoint_t endpoints[USBIP_MAX_ENDPOINTS];

    /* Topology and descriptor details (zero/empty for virtual devices) */
    uint8_t dev_addr;               /* USB device address */
    uint8_t parent_hub_addr;        /* 0 when on the root port */
    uint8_t parent_port;            /* port number on the parent hub, 0 on the root port */
    uint16_t max_power_ma;          /* from bMaxPower of the active configuration */
    char manufacturer[64];
    char product[64];
    char serial[64];
} usbip_backend_device_t;

#define USB_BACKEND_MAX_HUBS 8

/* An external hub managed by the USB Host Library (hubs are not exported). */
typedef struct {
    uint8_t addr;
    uint8_t parent_hub_addr;        /* 0 when on the root port */
    uint8_t parent_port;
    char path[32];                  /* Linux style port path, e.g. "1-1" or "1-1.4" */
    uint16_t id_vendor;
    uint16_t id_product;
    char manufacturer[64];
    char product[64];
} usb_backend_hub_t;

esp_err_t usb_backend_start(void);
size_t usb_backend_get_devices(usbip_backend_device_t *out_devices, size_t max_devices);
bool usb_backend_get_device_by_busid(const char busid[32], usbip_backend_device_t *out_device);

/* An asynchronous transfer request.  The caller fills the public fields and
   submits it; the backend task calls done() exactly once, when the transfer
   has completed, failed, or been cancelled.  The request and its buffers
   must stay valid until then.  No task waits on a transfer, so a request the
   device does not answer (a pending read) costs only this struct and its
   buffer, like a pending URB on a Linux host. */
typedef struct usb_backend_req usb_backend_req_t;
typedef void (*usb_backend_done_cb_t)(usb_backend_req_t *req, int status, size_t in_len);

struct usb_backend_req {
    /* Filled by the caller */
    char busid[32];
    uint8_t endpoint_addr;          /* 0 = control (setup is used), 0x8N IN, 0x0N OUT */
    usb_setup_packet_t setup;
    const uint8_t *out_data;
    size_t out_len;
    uint8_t *in_data;
    size_t in_capacity;
    usb_backend_done_cb_t done;     /* called from the backend task: status is 0 or a negative errno */
    void *ctx;                      /* for the caller */

    /* Owned by the backend from usb_backend_submit() until done() */
    volatile bool cancel;           /* set by usb_backend_cancel() */
    struct usb_backend_req *next;
    uint32_t seq;
    int status;
};

/* Queue a transfer.  Never blocks; done() follows from the backend task,
   also for immediate failures (-ENODEV, -ENOMEM, ...). */
void usb_backend_submit(usb_backend_req_t *req);

/* Retire a queued or in-flight request: done() is called with -ECONNRESET,
   or with the real result if the transfer completed first.  Safe to call
   more than once, and after done(). */
void usb_backend_cancel(usb_backend_req_t *req);

bool usb_backend_is_interrupt_endpoint(const char busid[32], uint8_t ep_num, uint8_t direction);

/* The client's USB/IP session for a device has ended: release its claimed
   interfaces (and their host channels) until the next session uses them. */
void usb_backend_session_ended(const char busid[32]);

/* Snapshot of the external hubs currently attached. */
size_t usb_backend_get_hubs(usb_backend_hub_t *out_hubs, size_t max_hubs);

/* Claim accounting: which interfaces and endpoints each connected device
   holds (each claimed endpoint pins one DWC host channel; each connected
   device also holds one EP0 channel, and each external hub one interrupt
   channel), plus claim-failure history.  The HCD's own per-channel table is
   not reachable from here, so totals derived from these counts are
   estimates, not an authoritative channel ledger. */
typedef struct {
    char busid[32];
    uint8_t claimed_interfaces[USBIP_MAX_INTERFACES]; /* interface numbers */
    int num_claimed_interfaces;
    int num_claimed_endpoints;  /* endpoints of claimed interfaces (data pipes) */
    int in_flight;              /* transfers currently in flight */
    bool release_pending;
    uint32_t claim_failures;    /* usb_host_interface_claim() failures */
    char last_claim_error[24];  /* esp_err_to_name() of the last failure, "" when none */
    bool last_claim_was_exhaustion; /* ESP_ERR_NOT_SUPPORTED: how hcd_pipe_alloc
                                       reports "No more HCD channels available" */
} usb_backend_claims_device_t;

typedef struct {
    int connected_devices;      /* real devices open (one EP0 channel each) */
    int external_hubs;          /* each holds one interrupt channel */
    int claimed_interfaces;
    int claimed_endpoints;
    uint32_t exhaustion_events; /* claim failures that were channel exhaustion */
    int64_t last_exhaustion_us; /* esp_timer clock; 0 = never */
    char last_exhaustion_busid[32];
    /* Authoritative ledger from the HCD itself (usb_host_channel_stats());
       -1 when the running host library does not provide it. */
    int hcd_channels_total;
    int hcd_channels_allocated;
    /* Pipes queued for a free channel under dynamic binding; -1 unknown. */
    int hcd_channel_waiters;
    /* Parked bulk IN listener and rotation counters (usb_host_parked_stats());
       every field is -1 when unknown. rotor_active is 0/1. */
    int parked_bound;
    int rotor_active;
    int promotions;
    int yields_req;
    int yields_done;
    int cap_refusals;
    int reserve_refusals;
    int rot_passes;
    int rej_not_bound;
    int rej_not_parked;
    int rej_pause;
    int rej_halt_wait;
    int rej_state;
    int rej_not_exec;
    int rej_urb_null;
    int rej_too_big;
    int halt_noop;
} usb_backend_claims_totals_t;

size_t usb_backend_get_claims(usb_backend_claims_device_t *out, size_t max,
                              usb_backend_claims_totals_t *totals);

#endif
