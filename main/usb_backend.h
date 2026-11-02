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
/* Debug (issue #37): dump the next `count` control executions on device address dev_addr. */
void usb_backend_arm_ep0_dump(int dev_addr, int count);

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
    const char *error_detail;       /* channel-level error of a failed transfer, or NULL (issue #37) */
};

/* Queue a transfer.  Never blocks; done() follows from the backend task,
   also for immediate failures (-ENODEV, -ENOMEM, ...). */
void usb_backend_submit(usb_backend_req_t *req);

/* Retire a queued or in-flight request: done() is called with -ECONNRESET,
   or with the real result if the transfer completed first.  Safe to call
   more than once, and after done(). */
void usb_backend_cancel(usb_backend_req_t *req);

/* Diagnostics (issue #37): one control transfer to a connected device, submitted through
   usb_backend_submit() exactly like a forwarded usbip EP0 request, and waited for. Returns
   the errno status; in_buf holds the bytes received (in_len); error_detail is the channel
   error behind a failure. */
int usb_backend_debug_control(const char busid[32], const usb_setup_packet_t *setup,
                              uint8_t *in_buf, size_t in_capacity,
                              size_t *in_len, const char **error_detail);

/* One transfer to a connected device through usb_backend_submit(), waited
   for (control when endpoint_addr is 0, else bulk/interrupt).  A transfer
   still pending after timeout_ms is cancelled (and its completion waited
   for).  Returns 0 or a negative errno; -EIO from a bulk/interrupt transfer
   can mean its interface could not be claimed (see usb_backend_get_claims). */
int usb_backend_transfer_sync(const char busid[32], uint8_t endpoint_addr,
                              const usb_setup_packet_t *setup,
                              const uint8_t *out_data, size_t out_len,
                              uint8_t *in_buf, size_t in_capacity, uint32_t timeout_ms,
                              size_t *in_len, const char **error_detail);

/* Copy of the device's active configuration descriptor (wTotalLength bytes,
   at most cap), taken when it was exported.  Returns the length, 0 if none. */
size_t usb_backend_get_config_descriptor(const char busid[32], uint8_t *buf, size_t cap);

bool usb_backend_is_interrupt_endpoint(const char busid[32], uint8_t ep_num, uint8_t direction);

/* The client's USB/IP session for a device has ended: release its claimed
   interfaces (and their host channels) until the next session uses them. */
void usb_backend_session_ended(const char busid[32]);

/* Snapshot of the external hubs currently attached. */
size_t usb_backend_get_hubs(usb_backend_hub_t *out_hubs, size_t max_hubs);

/* A control transfer to a hub that the bridge manages locally (not exported): the hub's own
   default pipe through the host library, waited for. Setup/data are as on the wire. Returns
   ESP_OK with the data stage in in_buf (in_len bytes), or an error with err_class naming
   the failing step (open, submit, timeout, or the transfer status). */
esp_err_t usb_backend_hub_control(uint8_t hub_addr, const usb_setup_packet_t *setup,
                                  uint8_t *in_buf, size_t in_capacity,
                                  size_t *in_len, const char **err_class);
/* The hub-driver record whose port path is `path` (e.g. "1-1.3"). */
bool usb_backend_hub_addr_by_path(const char path[32], uint8_t *addr);

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
    int parked_intr_bound;          /* parked interrupt listeners holding channels (phase 3) */
    int rotor_active;
    int promotions;
    int yields_req;
    int yields_done;
    int cap_refusals;
    int reserve_refusals;
    /* Heap telemetry (bytes; -1 unknown): total free and minimum-ever free
       (MALLOC_CAP_DEFAULT), DMA-capable free and largest free DMA block
       (MALLOC_CAP_DMA, what the host transfer allocator uses). */
    int heap_free;
    int heap_min;
    int dma_free;
    int dma_largest;
    /* In-flight transfers now (sum over endpoints) and peak since boot (leak detector). */
    int inflight_now;
    int inflight_peak;
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
