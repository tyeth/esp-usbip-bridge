#include "usb_backend.h"

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "sdkconfig.h"
#include "usb/usb_host.h"
#include "usb/usb_host_hub.h"
#include "usb/usb_types_stack.h"

#include "virtual_device.h"

#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_USBIP_S3_USB_OTG_DEVKIT_POWER
#include "driver/gpio.h"
#endif

#if CONFIG_IDF_TARGET_ESP32P4 && CONFIG_USBIP_P4HIL_USB_POWER_ENABLE
#include "harness_io_expander.h"
#endif

#define USB_BACKEND_EVENT_QUEUE_LEN 16
#define USB_BACKEND_TASK_STACK 8192
#define USB_BACKEND_TASK_PRIORITY 9
#define USB_BACKEND_DAEMON_TASK_STACK 4096
#define USB_BACKEND_DAEMON_TASK_PRIORITY 10

/* Transfers in flight at the USB host (every kind, all devices). */
#define USB_BACKEND_MAX_INFLIGHT CONFIG_USBIP_MAX_INFLIGHT_XFERS
/* Of those, bulk/interrupt IN transfers may hold at most this many, so
   pending reads (which a device NAKs until it has data, possibly for ever)
   can never starve OUT data and control requests. */
#define USB_BACKEND_RESERVED_XFERS 8
#define USB_BACKEND_MAX_IN_INFLIGHT (USB_BACKEND_MAX_INFLIGHT - USB_BACKEND_RESERVED_XFERS)
/* Pending IN transfers per endpoint; further reads wait in the request
   queue until one completes. */
#define USB_BACKEND_MAX_IN_PER_EP CONFIG_USBIP_MAX_IN_XFERS_PER_ENDPOINT
/* Software deadline for a control transfer (the host library ignores
   timeout_ms); 0 = none.  Bulk and interrupt transfers have no deadline:
   as on a Linux host they stay pending until the device answers, the
   client unlinks them, or the device is gone. */
#define USB_BACKEND_CTRL_TIMEOUT_MS CONFIG_USBIP_CTRL_XFER_TIMEOUT_MS
/* How long a cancelled transfer may take to come back before its caller is
   answered anyway and the host transfer is kept until it completes (an
   orphan; the cancel normally takes a few ms). */
#define USB_BACKEND_CANCEL_WAIT_MS 1000
/* Transfers retired by another transfer's halt/flush of the same endpoint
   (or EP0) are submitted again, at most this many times. */
#define USB_BACKEND_MAX_RESUBMITS 8
/* Poll period of the backend task: control deadlines, cancel waits and EP0
   retries are checked this often; completions wake it at once. */
#define USB_BACKEND_POLL_MS 10
/* Endpoints halted and flushed in one pass of the backend task. */
#define USB_BACKEND_MAX_FLUSHED_PER_PASS 16

#ifndef USB_CLASS_HUB
#define USB_CLASS_HUB 0x09
#endif

typedef enum {
    USB_BACKEND_EVENT_NEW_DEV = 1,
    USB_BACKEND_EVENT_DEV_GONE = 2,
    USB_BACKEND_EVENT_RELEASE = 3,
} usb_backend_event_type_t;

typedef struct {
    usb_backend_event_type_t type;
    union {
        uint8_t address;
        usb_device_handle_t dev_hdl;
        char busid[32];
    } u;
} usb_backend_event_t;

/* A transfer in flight at the USB host.  The backend task admits requests
   from the waiting queue into these, in order, and owns them; other tasks
   only read the in-flight list under state_mutex.  The request is detached
   (req = NULL) once its caller has been answered while the host transfer
   is still in flight (an orphan): the host transfer is then freed when its
   callback fires. */
typedef struct xfer {
    struct xfer *next;              /* in-flight list, in submission order */
    usb_backend_req_t *req;         /* NULL = orphan */
    uint32_t seq;

    char busid[32];
    uint8_t endpoint_addr;          /* 0 for control, 0x8N for IN, 0x0N for OUT */
    usb_setup_packet_t setup;       /* only used when endpoint_addr == 0 */
    bool is_control;                /* EP0 control transfer */
    bool is_in;                     /* device-to-host (the data stage, for control) */
    size_t payload_len;             /* requested data length */
    usb_device_handle_t dev_hdl;    /* cached device handle for abort */

    usb_transfer_t *xfer;           /* host transfer, NULL until submitted */
    bool submitted;                 /* handed to the host library */
    volatile bool completed;        /* host callback has fired */
    bool aborted;                   /* cancelled by the caller, or past its deadline */
    bool cancel_sent;               /* host-library cancel, or halt/flush, requested */
    TickType_t cancel_tick;         /* when it was requested */
    bool has_deadline;
    TickType_t deadline;
    uint8_t resubmits;              /* resubmissions after a collateral flush */
} xfer_t;

typedef struct {
    bool in_use;
    bool claimed_interfaces[USBIP_MAX_INTERFACES]; /* includes a partial claim after failure */
    bool release_pending;           /* session ended with a transfer in flight: release when it is done */
    uint32_t halted_eps;            /* endpoints halted by a STALL (see ep_halt_bit()) */
    uint32_t claim_failures;        /* usb_host_interface_claim() failures on this device */
    esp_err_t last_claim_err;       /* ESP_OK when it has never failed */
    usb_device_handle_t dev_hdl;
    usbip_backend_device_t device;
    bool ep0_round_logged;          /* the control-IN MPS rounding quirk was logged once (issue #37) */
    uint8_t *config_desc;           /* copy of the active configuration descriptor (malloc'd), or NULL */
    uint16_t config_desc_len;
} usb_backend_device_slot_t;

typedef struct {
    SemaphoreHandle_t state_mutex;
    QueueHandle_t event_queue;
    TaskHandle_t task_hdl;

    /* Requests waiting to be admitted, oldest first (state_mutex). */
    usb_backend_req_t *waiting_head;
    usb_backend_req_t *waiting_tail;
    uint32_t next_seq;
    /* Transfers in flight, oldest first.  Written by the backend task only;
       read by other tasks under state_mutex. */
    xfer_t *inflight_head;
    xfer_t *inflight_tail;
    int num_inflight;
    int num_in_inflight;            /* bulk/interrupt IN among them */
    int inflight_peak;              /* leak detector: highest num_inflight seen (round 2m) */

    usb_host_client_handle_t client_hdl;
    usb_backend_device_slot_t devices[CONFIG_USBIP_MAX_DEVICES];

    /* Claim-failure accounting (state_mutex). */
    uint32_t exhaustion_events;
    int64_t last_exhaustion_us;
    char last_exhaustion_busid[32];
} usb_backend_state_t;

static const char *TAG = "usb_backend";
static usb_backend_state_t s_state;

#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_USBIP_S3_USB_OTG_DEVKIT_POWER
static esp_err_t usb_backend_s3_usb_otg_devkit_power_init(void)
{
    const uint64_t pin_mask = (1ULL << CONFIG_USBIP_S3_USB_BOOST_EN_GPIO) |
                              (1ULL << CONFIG_USBIP_S3_USB_DEV_VBUS_EN_GPIO) |
                              (1ULL << CONFIG_USBIP_S3_USB_LIMIT_EN_GPIO) |
                              (1ULL << CONFIG_USBIP_S3_USB_SEL_GPIO);

    gpio_config_t io_conf = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        return err;
    }

    gpio_set_level(CONFIG_USBIP_S3_USB_BOOST_EN_GPIO, 0);
    gpio_set_level(CONFIG_USBIP_S3_USB_DEV_VBUS_EN_GPIO, 1);
    gpio_set_level(CONFIG_USBIP_S3_USB_LIMIT_EN_GPIO, 1);
    gpio_set_level(CONFIG_USBIP_S3_USB_SEL_GPIO, 1);

    ESP_LOGD(TAG,
             "Configured ESP32-S3-USB-OTG host power GPIOs (%d,%d,%d,%d)",
             CONFIG_USBIP_S3_USB_BOOST_EN_GPIO,
             CONFIG_USBIP_S3_USB_DEV_VBUS_EN_GPIO,
             CONFIG_USBIP_S3_USB_LIMIT_EN_GPIO,
             CONFIG_USBIP_S3_USB_SEL_GPIO);
    return ESP_OK;
}
#endif

#if CONFIG_IDF_TARGET_ESP32P4 && CONFIG_USBIP_P4HIL_USB_POWER_ENABLE
static esp_err_t usb_backend_p4hil_usb_power_init(void)
{
    int exp_idx = CONFIG_USBIP_P4HIL_USB_POWER_EXPANDER_IDX;
    int pin     = CONFIG_USBIP_P4HIL_USB_POWER_EXPANDER_PIN;

    if (!harness_io_expander_is_initialized()) {
        ESP_LOGE(TAG, "IO expander not initialised, cannot enable USB power");
        return ESP_ERR_INVALID_STATE;
    }

    /* Set the expander pin as output, high */
    esp_err_t err = harness_io_expander_set_dir(exp_idx, (uint8_t)pin, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set Exp%d[%d] as output: %s",
                 exp_idx, pin, esp_err_to_name(err));
        return err;
    }

    err = harness_io_expander_write_pin(exp_idx, (uint8_t)pin, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set Exp%d[%d] high: %s",
                 exp_idx, pin, esp_err_to_name(err));
        return err;
    }

    ESP_LOGD(TAG,
             "P4HIL USB host power enabled on Exp%d[%d]",
             exp_idx, pin);
    return ESP_OK;
}
#endif

static uint32_t usb_speed_to_usbip(usb_speed_t speed)
{
    switch (speed) {
    case USB_SPEED_LOW:
        return 1;
    case USB_SPEED_FULL:
        return 2;
    case USB_SPEED_HIGH:
        return 3;
    default:
        return 0;
    }
}

static void parse_descriptors(const usb_config_desc_t *config_desc, usbip_backend_device_t *device)
{
    if (config_desc == NULL || device == NULL) {
        return;
    }

    const uint8_t *raw = (const uint8_t *)config_desc;
    const size_t total_len = config_desc->wTotalLength;

    uint8_t intf_count = 0;
    uint8_t ep_count = 0;
    for (size_t offset = 0; offset + 2 <= total_len;) {
        const uint8_t desc_len = raw[offset];
        const uint8_t desc_type = raw[offset + 1];

        if (desc_len < 2 || (offset + desc_len) > total_len) {
            break;
        }

        if (desc_type == USB_B_DESCRIPTOR_TYPE_INTERFACE && desc_len >= 9 && intf_count < USBIP_MAX_INTERFACES) {
            device->interfaces[intf_count].interface_class = raw[offset + 5];
            device->interfaces[intf_count].interface_subclass = raw[offset + 6];
            device->interfaces[intf_count].interface_protocol = raw[offset + 7];
            intf_count++;
        } else if (desc_type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && desc_len >= 7 && ep_count < USBIP_MAX_ENDPOINTS) {
            device->endpoints[ep_count].address = raw[offset + 2];
            device->endpoints[ep_count].attributes = raw[offset + 3];
            device->endpoints[ep_count].max_packet_size = raw[offset + 4] | (raw[offset + 5] << 8);
            device->endpoints[ep_count].interval = raw[offset + 6];
            ep_count++;
        }

        offset += desc_len;
    }

    device->num_interfaces = intf_count;
    device->num_endpoints = ep_count;
}

static bool is_hub_device(const usb_device_desc_t *dev_desc, const usbip_backend_device_t *device)
{
    if (dev_desc != NULL && dev_desc->bDeviceClass == USB_CLASS_HUB) {
        return true;
    }

    for (uint8_t i = 0; i < device->num_interfaces; i++) {
        if (device->interfaces[i].interface_class == USB_CLASS_HUB) {
            return true;
        }
    }

    return false;
}

/* Copy a UTF-16LE string descriptor into a printable ASCII string. */
static void str_desc_to_ascii(const usb_str_desc_t *desc, char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (desc == NULL || desc->bLength < 2) {
        return;
    }
    size_t n_chars = (desc->bLength - 2) / 2;
    size_t o = 0;
    for (size_t i = 0; i < n_chars && o + 1 < out_size; i++) {
        uint16_t c = desc->wData[i];
        if (c == 0) {
            break;    /* some devices NUL-pad their strings */
        }
        out[o++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    out[o] = '\0';
}

#define USB_BACKEND_MAX_HUB_DEPTH 6

/* Resolve the parent hub of a device and build its Linux style port path
   ("1-1" on the root port, "1-1.3" on port 3 of the hub on the root port).
   The root port is reported as port 1 of bus 1, like a Linux root hub.
   Walks parent handles up to the root port; hubs stay alive while they
   have children, so the handles are valid. */
static void resolve_topology(const usb_device_info_t *dev_info,
                             uint8_t *parent_hub_addr,
                             uint8_t *parent_port,
                             char *path, size_t path_size)
{
    uint8_t ports[USB_BACKEND_MAX_HUB_DEPTH];
    size_t depth = 0;
    *parent_hub_addr = 0;
    *parent_port = 0;

    usb_device_info_t info = *dev_info;
    while (info.parent.dev_hdl != NULL && depth < USB_BACKEND_MAX_HUB_DEPTH) {
        ports[depth++] = info.parent.port_num;
        usb_device_info_t parent_info;
        if (usb_host_device_info(info.parent.dev_hdl, &parent_info) != ESP_OK) {
            /* Parent unknown: fall back to an address busid */
            snprintf(path, path_size, "1-%u", dev_info->dev_addr);
            return;
        }
        if (depth == 1) {
            *parent_hub_addr = parent_info.dev_addr;
            *parent_port = info.parent.port_num;
        }
        info = parent_info;
    }

    size_t off = strlcpy(path, "1-1", path_size);
    while (depth > 0 && off < path_size) {
        off += snprintf(path + off, path_size - off, ".%u", ports[--depth]);
    }
}

static bool busid_matches_key(const char key[32], const char stored_busid[32])
{
    char expected[32] = {0};
    strlcpy(expected, stored_busid, sizeof(expected));
    return memcmp(key, expected, sizeof(expected)) == 0;
}

static int find_slot_by_busid_locked(const char busid[32])
{
    for (int i = 0; i < CONFIG_USBIP_MAX_DEVICES; i++) {
        if (!s_state.devices[i].in_use) {
            continue;
        }
        if (busid_matches_key(busid, s_state.devices[i].device.busid)) {
            return i;
        }
    }
    return -1;
}

static int find_slot_by_handle_locked(usb_device_handle_t dev_hdl)
{
    for (int i = 0; i < CONFIG_USBIP_MAX_DEVICES; i++) {
        if (!s_state.devices[i].in_use) {
            continue;
        }
        if (s_state.devices[i].dev_hdl == dev_hdl) {
            return i;
        }
    }
    return -1;
}

static int find_slot_by_devnum_locked(uint32_t devnum)
{
    for (int i = 0; i < CONFIG_USBIP_MAX_DEVICES; i++) {
        if (!s_state.devices[i].in_use) {
            continue;
        }
        if (s_state.devices[i].device.devnum == devnum) {
            return i;
        }
    }
    return -1;
}

static int find_free_slot_locked(void)
{
    for (int i = 0; i < CONFIG_USBIP_MAX_DEVICES; i++) {
        if (!s_state.devices[i].in_use) {
            return i;
        }
    }
    return -1;
}


static void clear_slot_locked(int slot)
{
    if (slot < 0 || slot >= CONFIG_USBIP_MAX_DEVICES) {
        return;
    }

    /* Pipe slots are allocated dynamically per URB and freed by the
       caller — no per-device pipe state to clean up here. */

    free(s_state.devices[slot].config_desc);
    memset(&s_state.devices[slot], 0, sizeof(s_state.devices[slot]));
}

static bool has_claimed_interfaces_locked(int slot)
{
    if (slot < 0 || slot >= CONFIG_USBIP_MAX_DEVICES || !s_state.devices[slot].in_use) {
        return false;
    }
    for (uint8_t i = 0; i < USBIP_MAX_INTERFACES; i++) {
        if (s_state.devices[slot].claimed_interfaces[i]) {
            return true;
        }
    }
    return false;
}

static void release_interfaces_locked(int slot)
{
    if (slot < 0 || slot >= CONFIG_USBIP_MAX_DEVICES || !s_state.devices[slot].in_use) {
        return;
    }
    usb_device_handle_t dev_hdl = s_state.devices[slot].dev_hdl;
    for (uint8_t i = 0; i < USBIP_MAX_INTERFACES; i++) {
        if (!s_state.devices[slot].claimed_interfaces[i]) {
            continue;
        }
        esp_err_t err = usb_host_interface_release(s_state.client_hdl, dev_hdl, i);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "usb_host_interface_release(%u) failed: %s", i, esp_err_to_name(err));
        } else {
            s_state.devices[slot].claimed_interfaces[i] = false;
        }
    }

    /* Pipe slots are allocated dynamically per URB — nothing to free here. */

}

/* Resolve an endpoint to its alternate-setting-zero interface. Interface
   numbers need not be contiguous, and num_interfaces is a descriptor count,
   not a bound on bInterfaceNumber. The claim bitmap bounds supported numbers. */
static int find_endpoint_interface_locked(int slot, uint8_t endpoint_addr)
{
    if (slot < 0 || slot >= CONFIG_USBIP_MAX_DEVICES || !s_state.devices[slot].in_use
        || (endpoint_addr & 0x0f) == 0) {
        return -1;
    }
    const usb_config_desc_t *cfg = NULL;
    if (usb_host_get_active_config_descriptor(s_state.devices[slot].dev_hdl, &cfg) != ESP_OK
        || cfg == NULL) {
        return -1;
    }
    for (int num = 0; num < USBIP_MAX_INTERFACES; num++) {
        int offset = 0;
        const usb_intf_desc_t *intf = usb_parse_interface_descriptor(cfg, num, 0, &offset);
        if (intf == NULL) {
            continue;
        }
        for (int i = 0; i < intf->bNumEndpoints; i++) {
            int ep_offset = offset;
            const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(intf, i, cfg->wTotalLength, &ep_offset);
            if (ep != NULL && ep->bEndpointAddress == endpoint_addr) {
                return num;
            }
        }
    }
    return -1;
}

static esp_err_t ensure_endpoint_interface_claimed_locked(int slot, uint8_t endpoint_addr)
{
    if (slot < 0 || slot >= CONFIG_USBIP_MAX_DEVICES || !s_state.devices[slot].in_use) {
        return ESP_ERR_INVALID_STATE;
    }
    const int num = find_endpoint_interface_locked(slot, endpoint_addr);
    if (num < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (s_state.devices[slot].claimed_interfaces[num]) {
        return ESP_OK;
    }

    usb_device_handle_t dev_hdl = s_state.devices[slot].dev_hdl;
    esp_err_t err = usb_host_interface_claim(s_state.client_hdl, dev_hdl, num, 0);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "usb_host_interface_claim(%d) failed: %s", num, esp_err_to_name(err));
        s_state.devices[slot].claim_failures++;
        s_state.devices[slot].last_claim_err = err;
        if (err == ESP_ERR_NOT_SUPPORTED) {
            /* hcd_pipe_alloc(): "No more HCD channels available" */
            s_state.exhaustion_events++;
            s_state.last_exhaustion_us = esp_timer_get_time();
            strlcpy(s_state.last_exhaustion_busid, s_state.devices[slot].device.busid,
                    sizeof(s_state.last_exhaustion_busid));
            ESP_LOGW(TAG, "%s: interface %d claim failed: no free host channel",
                     s_state.devices[slot].device.busid, num);
        }
        /* The host library unwinds this interface's partial endpoint allocation.
           Do not release other interfaces: they may have transfers in flight. */
        return err;
    }
    s_state.devices[slot].claimed_interfaces[num] = true;
    /* DMA footprint probe (round 2k): the next telemetry run attributes DMA pool loss to claims. */
    ESP_LOGW(TAG, "claimed interface %d (slot %d): dma_free=%u dma_largest=%u",
             (int)num, slot,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    return ESP_OK;
}

static void close_slot_locked(int slot)
{
    if (slot < 0 || slot >= CONFIG_USBIP_MAX_DEVICES || !s_state.devices[slot].in_use) {
        return;
    }

    release_interfaces_locked(slot);

    usb_device_handle_t dev_hdl = s_state.devices[slot].dev_hdl;
    if (dev_hdl != NULL) {
        esp_err_t err = usb_host_device_close(s_state.client_hdl, dev_hdl);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "usb_host_device_close failed: %s", esp_err_to_name(err));
        }
    }

    clear_slot_locked(slot);
}

static void usb_client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    usb_backend_state_t *state = (usb_backend_state_t *)arg;

    usb_backend_event_t evt;
    if (event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        ESP_LOGW(TAG, "USB device attached at address %u", event_msg->new_dev.address);
        evt.type = USB_BACKEND_EVENT_NEW_DEV;
        evt.u.address = event_msg->new_dev.address;
    } else if (event_msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        ESP_LOGW(TAG, "USB device detached");
        evt.type = USB_BACKEND_EVENT_DEV_GONE;
        evt.u.dev_hdl = event_msg->dev_gone.dev_hdl;
    } else {
        return;
    }

    if (xQueueSend(state->event_queue, &evt, 0) != pdTRUE) {
        ESP_LOGD(TAG, "Dropping USB event type=%d due to full queue", (int)evt.type);
    }
}

static void export_new_device(uint8_t address)
{
    usb_device_handle_t dev_hdl = NULL;
    esp_err_t err = usb_host_device_open(s_state.client_hdl, address, &dev_hdl);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "usb_host_device_open(%u) failed: %s", address, esp_err_to_name(err));
        return;
    }

    const usb_device_desc_t *dev_desc = NULL;
    err = usb_host_get_device_descriptor(dev_hdl, &dev_desc);
    if (err != ESP_OK || dev_desc == NULL) {
        ESP_LOGD(TAG, "usb_host_get_device_descriptor failed: %s", esp_err_to_name(err));
        usb_host_device_close(s_state.client_hdl, dev_hdl);
        return;
    }

    usb_device_info_t dev_info;
    err = usb_host_device_info(dev_hdl, &dev_info);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "usb_host_device_info failed: %s", esp_err_to_name(err));
        usb_host_device_close(s_state.client_hdl, dev_hdl);
        return;
    }

    usbip_backend_device_t device;
    memset(&device, 0, sizeof(device));

    device.present = true;
    device.busnum = 1;
    device.devnum = dev_info.dev_addr;
    device.dev_addr = dev_info.dev_addr;
    device.speed = usb_speed_to_usbip(dev_info.speed);
    str_desc_to_ascii(dev_info.str_desc_manufacturer, device.manufacturer, sizeof(device.manufacturer));
    str_desc_to_ascii(dev_info.str_desc_product, device.product, sizeof(device.product));
    str_desc_to_ascii(dev_info.str_desc_serial_num, device.serial, sizeof(device.serial));

    device.id_vendor = dev_desc->idVendor;
    device.id_product = dev_desc->idProduct;
    device.bcd_device = dev_desc->bcdDevice;
    device.device_class = dev_desc->bDeviceClass;
    device.device_subclass = dev_desc->bDeviceSubClass;
    device.device_protocol = dev_desc->bDeviceProtocol;
    device.num_configurations = dev_desc->bNumConfigurations;
    device.configuration_value = dev_info.bConfigurationValue;

    const usb_config_desc_t *config_desc = NULL;
    err = usb_host_get_active_config_descriptor(dev_hdl, &config_desc);
    if (err == ESP_OK && config_desc != NULL) {
        parse_descriptors(config_desc, &device);
        device.max_power_ma = (uint16_t)config_desc->bMaxPower * 2;
    }

    if (is_hub_device(dev_desc, &device)) {
        ESP_LOGW(TAG, "Hub detected at address=%u, managing locally (not exported)", address);
        usb_host_device_close(s_state.client_hdl, dev_hdl);
        return;
    }

    char port_path[32];
    resolve_topology(&dev_info, &device.parent_hub_addr, &device.parent_port,
                     port_path, sizeof(port_path));

    /* Keep a copy of the configuration descriptor for the rescue tools
       (interface numbers and endpoints), so they never call into the host
       library from another task. */
    uint8_t *config_copy = NULL;
    uint16_t config_copy_len = 0;
    if (config_desc != NULL && config_desc->wTotalLength > 0) {
        config_copy = malloc(config_desc->wTotalLength);
        if (config_copy != NULL) {
            memcpy(config_copy, config_desc, config_desc->wTotalLength);
            config_copy_len = config_desc->wTotalLength;
        }
    }

    xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);

    strlcpy(device.busid, port_path, sizeof(device.busid));
    snprintf(device.path, sizeof(device.path), "/esp-usb-host/%s", port_path);

    int existing_slot = find_slot_by_devnum_locked(device.devnum);
    if (existing_slot >= 0) {
        close_slot_locked(existing_slot);
    }
    existing_slot = find_slot_by_busid_locked(device.busid);
    if (existing_slot >= 0) {
        close_slot_locked(existing_slot);
    }

    const int free_slot = find_free_slot_locked();
    if (free_slot < 0) {
        xSemaphoreGive(s_state.state_mutex);
        ESP_LOGD(TAG, "No free export slots left (max=%d)", CONFIG_USBIP_MAX_DEVICES);
        free(config_copy);
        usb_host_device_close(s_state.client_hdl, dev_hdl);
        return;
    }

    s_state.devices[free_slot].in_use = true;
    s_state.devices[free_slot].dev_hdl = dev_hdl;
    s_state.devices[free_slot].device = device;
    memset(s_state.devices[free_slot].claimed_interfaces, 0,
           sizeof(s_state.devices[free_slot].claimed_interfaces));
    s_state.devices[free_slot].claim_failures = 0;
    s_state.devices[free_slot].last_claim_err = ESP_OK;
    s_state.devices[free_slot].config_desc = config_copy;
    s_state.devices[free_slot].config_desc_len = config_copy_len;

    /* Pipe slots are allocated dynamically per URB — no per-device
       pre-allocation needed. */

    xSemaphoreGive(s_state.state_mutex);

    ESP_LOGI(TAG,
             "Exporting USB device busid=%s vid=%04x pid=%04x (address %u)",
             device.busid,
             device.id_vendor,
             device.id_product,
             device.dev_addr);
}

static void remove_gone_device(usb_device_handle_t dev_hdl)
{
    xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);

    const int slot = find_slot_by_handle_locked(dev_hdl);
    if (slot >= 0) {
        char busid[32] = {0};
        strlcpy(busid, s_state.devices[slot].device.busid, sizeof(busid));
        close_slot_locked(slot);
        xSemaphoreGive(s_state.state_mutex);
        ESP_LOGW(TAG, "USB device disconnected: %s", busid);
        return;
    }

    xSemaphoreGive(s_state.state_mutex);
}

/* A client's USB/IP session for the device ended: release its interfaces,
   which frees their host pipes and DWC channels for other devices.  The host
   has 16 channels and every device's EP0 and each hub's interrupt endpoint
   hold one for good, so a few devices with many endpoints claimed (CDC + MSC
   + HID + MIDI) exhaust them and later claims fail. The next session claims
   each interface again, with fresh pipes at DATA0, on its first non-control
   transfer to that interface. A device with a transfer still in flight keeps
   its interfaces. */
static bool busid_in_flight_locked(const char busid[32])
{
    for (const xfer_t *x = s_state.inflight_head; x != NULL; x = x->next) {
        if (busid_matches_key(busid, x->busid)) {
            return true;
        }
    }
    return false;
}

static void release_session_device(const char busid[32])
{
    xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
    const int slot = find_slot_by_busid_locked(busid);
    if (has_claimed_interfaces_locked(slot)) {
        if (busid_in_flight_locked(busid)) {
            /* An orphan (a transfer the device never finished) is still in
               flight: release once it is retired, unless a new session
               has started by then. */
            s_state.devices[slot].release_pending = true;
            ESP_LOGD(TAG, "%s: session ended with a transfer in flight, interfaces kept", busid);
        } else {
            release_interfaces_locked(slot);
            s_state.devices[slot].halted_eps = 0;
            ESP_LOGI(TAG, "%s: session ended, interfaces released", busid);
        }
    }
    xSemaphoreGive(s_state.state_mutex);
}

static void process_backend_events(void)
{
    usb_backend_event_t evt;
    while (xQueueReceive(s_state.event_queue, &evt, 0) == pdTRUE) {
        if (evt.type == USB_BACKEND_EVENT_NEW_DEV) {
            export_new_device(evt.u.address);
        } else if (evt.type == USB_BACKEND_EVENT_DEV_GONE) {
            remove_gone_device(evt.u.dev_hdl);
        } else if (evt.type == USB_BACKEND_EVENT_RELEASE) {
            release_session_device(evt.u.busid);
        }
    }
}

/* The public entry points can run before usb_backend_start(): hub_ctl_init()
   starts its port-events poll task and the HTTP server comes up while the
   network is still negotiating.  Until the state mutex exists there is
   nothing to report (start creates it once and never clears it).  The
   static helpers that take the mutex run only in the backend task. */
static bool backend_started(void)
{
    return s_state.state_mutex != NULL;
}

/* Wake the backend task: it blocks in usb_host_client_handle_events(). */
static void wake_backend(void)
{
    if (s_state.client_hdl != NULL) {
        usb_host_client_unblock(s_state.client_hdl);
    }
}

void usb_backend_session_ended(const char busid[32])
{
    if (busid == NULL || s_state.event_queue == NULL) {
        return;
    }
    usb_backend_event_t evt = {
        .type = USB_BACKEND_EVENT_RELEASE,
    };
    strlcpy(evt.u.busid, busid, sizeof(evt.u.busid));
    if (xQueueSend(s_state.event_queue, &evt, 0) == pdTRUE) {
        wake_backend();
    }
}

/* Completion callback for all transfer types, run inside the backend
   task's usb_host_client_handle_events(): just flag the transfer, the
   task finishes it afterwards. */
static void xfer_done_cb(usb_transfer_t *transfer)
{
    xfer_t *x = (xfer_t *)transfer->context;
    x->completed = true;
}

static int map_transfer_status_to_errno(usb_transfer_status_t status)
{
    switch (status) {
    case USB_TRANSFER_STATUS_COMPLETED:
        return 0;
    case USB_TRANSFER_STATUS_TIMED_OUT:
        return -ETIMEDOUT;
    case USB_TRANSFER_STATUS_CANCELED:
        return -ECONNRESET;
    case USB_TRANSFER_STATUS_STALL:
        return -EPIPE;
    case USB_TRANSFER_STATUS_NO_DEVICE:
        return -ENODEV;
    case USB_TRANSFER_STATUS_ERROR:
        return -EPROTO;
    case USB_TRANSFER_STATUS_OVERFLOW:
        return -EOVERFLOW;
    default:
        return -EIO;
    }
}

/* Bit for an endpoint in usb_backend_device_slot_t.halted_eps: OUT
   endpoints in bits 0-15, IN endpoints in bits 16-31. */
static uint32_t ep_halt_bit(uint8_t endpoint_addr)
{
    return 1u << ((endpoint_addr & 0x0f) + ((endpoint_addr & 0x80) ? 16 : 0));
}

/* Collect the endpoints of the claimed (alternate setting 0) interfaces,
   or of one interface when intf_filter >= 0. */
static int collect_claimed_endpoints_locked(int dev_slot, int intf_filter,
                                            uint8_t *out, int max)
{
    int count = 0;
    const usb_config_desc_t *cfg = NULL;
    if (usb_host_get_active_config_descriptor(s_state.devices[dev_slot].dev_hdl, &cfg) != ESP_OK
        || cfg == NULL) {
        return 0;
    }
    for (int intf_num = 0; intf_num < USBIP_MAX_INTERFACES; intf_num++) {
        if (!s_state.devices[dev_slot].claimed_interfaces[intf_num]
            || (intf_filter >= 0 && intf_num != intf_filter)) {
            continue;
        }
        int offset = 0;
        const usb_intf_desc_t *intf = usb_parse_interface_descriptor(cfg, intf_num, 0, &offset);
        if (intf == NULL) {
            continue;
        }
        for (int i = 0; i < intf->bNumEndpoints && count < max; i++) {
            int ep_offset = offset;
            const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(intf, i, cfg->wTotalLength, &ep_offset);
            if (ep != NULL) {
                out[count++] = ep->bEndpointAddress;
            }
        }
    }
    return count;
}

/* Reset a host endpoint after a request that reset the device's data
   toggle for it (USB 2.0 9.4.5): halt (cancels a transfer in flight),
   flush, start the toggle again at DATA0 and make the pipe active.
   Runs in the backend task, like the abort path. */
static void reset_host_endpoint(usb_device_handle_t dev_hdl, uint8_t endpoint_addr)
{
    usb_host_endpoint_halt(dev_hdl, endpoint_addr);
    usb_host_endpoint_flush(dev_hdl, endpoint_addr);
    esp_err_t err = usb_host_endpoint_reset_toggle(dev_hdl, endpoint_addr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reset toggle failed: %s (ep=0x%02x)", esp_err_to_name(err), endpoint_addr);
    }
    usb_host_endpoint_clear(dev_hdl, endpoint_addr);
}

/* Linux usbip-host applies CLEAR_FEATURE(ENDPOINT_HALT), SET_INTERFACE and
   SET_CONFIGURATION to its own host controller as well (stub_rx.c
   tweak_*_cmd()).  Mirror that once the device has accepted the request:
   the device's toggles are back at DATA0, so ours must be too, and an
   endpoint halted by a STALL becomes usable again. */
static void apply_standard_request_to_host(const xfer_t *pipe)
{
    const usb_setup_packet_t *setup = &pipe->setup;
    const uint8_t std_out = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_STANDARD;
    int intf_filter;

    if (setup->bmRequestType == (std_out | USB_BM_REQUEST_TYPE_RECIP_ENDPOINT)
        && setup->bRequest == USB_B_REQUEST_CLEAR_FEATURE
        && setup->wValue == 0 /* ENDPOINT_HALT */) {
        intf_filter = -2;   /* the endpoint in wIndex */
    } else if (setup->bmRequestType == (std_out | USB_BM_REQUEST_TYPE_RECIP_INTERFACE)
               && setup->bRequest == USB_B_REQUEST_SET_INTERFACE) {
        intf_filter = setup->wIndex & 0xff;
    } else if (setup->bmRequestType == (std_out | USB_BM_REQUEST_TYPE_RECIP_DEVICE)
               && setup->bRequest == USB_B_REQUEST_SET_CONFIGURATION) {
        intf_filter = -1;   /* every interface */
    } else {
        return;
    }

    uint8_t eps[USBIP_MAX_ENDPOINTS];
    int num_eps = 0;

    xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
    const int dev_slot = find_slot_by_busid_locked(pipe->busid);
    if (dev_slot < 0) {
        xSemaphoreGive(s_state.state_mutex);
        return;
    }
    usb_backend_device_slot_t *dev = &s_state.devices[dev_slot];
    if (intf_filter == -2) {
        const uint8_t ep = setup->wIndex & 0xff;
        const int num = find_endpoint_interface_locked(dev_slot, ep);
        if (num >= 0 && dev->claimed_interfaces[num]) {
            eps[num_eps++] = ep;
        }
    } else {
        if (intf_filter >= 0 && (setup->wValue & 0xff) != 0) {
            ESP_LOGW(TAG, "%s: SET_INTERFACE %d alt %d; only alt 0 endpoints are mapped",
                     pipe->busid, intf_filter, setup->wValue & 0xff);
        }
        num_eps = collect_claimed_endpoints_locked(dev_slot, intf_filter, eps, USBIP_MAX_ENDPOINTS);
    }
    for (int i = 0; i < num_eps; i++) {
        dev->halted_eps &= ~ep_halt_bit(eps[i]);
    }
    /* Unclaimed endpoints have no host pipe; a later claim starts at DATA0. */
    const usb_device_handle_t dev_hdl = dev->dev_hdl;
    xSemaphoreGive(s_state.state_mutex);

    for (int i = 0; i < num_eps; i++) {
        ESP_LOGD(TAG, "%s: reset host endpoint 0x%02x (bRequest %u)",
                 pipe->busid, eps[i], setup->bRequest);
        reset_host_endpoint(dev_hdl, eps[i]);
    }
}

/* After a non-control transfer failed: a STALL leaves the host pipe halted
   until the client sends CLEAR_FEATURE(ENDPOINT_HALT), and URBs for the
   endpoint fail with -EPIPE meanwhile (as on Linux).  Any other error
   halts the pipe too, but the device is not halted, so the pipe is made
   active again straight away (the toggle is still right). */
static void note_transfer_failure(const xfer_t *pipe, usb_transfer_status_t usb_status)
{
    if (usb_status == USB_TRANSFER_STATUS_STALL) {
        xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
        const int dev_slot = find_slot_by_busid_locked(pipe->busid);
        if (dev_slot >= 0) {
            s_state.devices[dev_slot].halted_eps |= ep_halt_bit(pipe->endpoint_addr);
        }
        xSemaphoreGive(s_state.state_mutex);
    } else if (usb_status == USB_TRANSFER_STATUS_ERROR
               || usb_status == USB_TRANSFER_STATUS_OVERFLOW) {
        usb_host_endpoint_clear(pipe->dev_hdl, pipe->endpoint_addr);
    }
}

static bool endpoint_is_halted(const xfer_t *pipe)
{
    xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
    const int dev_slot = find_slot_by_busid_locked(pipe->busid);
    const bool halted = dev_slot >= 0
        && (s_state.devices[dev_slot].halted_eps & ep_halt_bit(pipe->endpoint_addr)) != 0;
    xSemaphoreGive(s_state.state_mutex);
    return halted;
}

static uint16_t get_endpoint_mps_locked(int dev_slot, uint8_t endpoint_addr)
{
    uint16_t mps = 64;  /* safe default */
    usb_device_handle_t dev = s_state.devices[dev_slot].dev_hdl;
    const usb_config_desc_t *cfg = NULL;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK || cfg == NULL) {
        return mps;
    }
    for (int intf_num = 0; intf_num < USBIP_MAX_INTERFACES; intf_num++) {
        int offset = 0;
        const usb_intf_desc_t *intf = usb_parse_interface_descriptor(cfg, intf_num, 0, &offset);
        if (intf == NULL) {
            continue;
        }
        for (int i = 0; i < intf->bNumEndpoints; i++) {
            int ep_offset = offset;
            const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(intf, i, cfg->wTotalLength, &ep_offset);
            if (ep != NULL && ep->bEndpointAddress == endpoint_addr) {
                return ep->wMaxPacketSize;
            }
        }
    }
    return mps;
}

/* EP0 max packet size of the device behind a handle (its real bMaxPacketSize0);
   64 when the host library cannot tell. */
static uint16_t ep0_mps_for(usb_device_handle_t dev_hdl, uint8_t *addr_out)
{
    usb_device_info_t info;
    if (addr_out != NULL) {
        *addr_out = 0;
    }
    if (dev_hdl != NULL && usb_host_device_info(dev_hdl, &info) == ESP_OK) {
        if (addr_out != NULL) {
            *addr_out = info.dev_addr;
        }
        if (info.bMaxPacketSize0 != 0) {
            return info.bMaxPacketSize0;
        }
    }
    return 64;
}

/* Submit one transfer to the host library.  Returns 0 on success (the
   transfer is in flight; the callback sets completed), -EAGAIN when EP0
   refused it while being recovered (try again next pass), or another
   negative errno for an immediate failure (no device, no memory, halted
   endpoint). */
static int prepare_and_submit_transfer(xfer_t *pipe)
{
    const usb_backend_req_t *req = pipe->req;

    xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
    const int dev_slot = find_slot_by_busid_locked(pipe->busid);
    if (dev_slot >= 0) {
        if (!pipe->is_control) {
            esp_err_t claim_err = ensure_endpoint_interface_claimed_locked(dev_slot, pipe->endpoint_addr);
            if (claim_err != ESP_OK) {
                xSemaphoreGive(s_state.state_mutex);
                return -EIO;
            }
            if (s_state.devices[dev_slot].halted_eps & ep_halt_bit(pipe->endpoint_addr)) {
                xSemaphoreGive(s_state.state_mutex);
                return -EPIPE;
            }
        }
        pipe->dev_hdl = s_state.devices[dev_slot].dev_hdl;
    } else {
        pipe->dev_hdl = NULL;
    }

    /* For IN bulk/interrupt, round up to MPS while we hold the mutex. */
    size_t xfer_len = pipe->is_control
        ? (USB_SETUP_PACKET_SIZE + pipe->payload_len)
        : pipe->payload_len;
    if (!pipe->is_control && pipe->is_in && pipe->payload_len > 0 && dev_slot >= 0) {
        uint16_t mps = get_endpoint_mps_locked(dev_slot, pipe->endpoint_addr);
        if (mps > 0 && (xfer_len % mps) != 0) {
            xfer_len = ((xfer_len + mps - 1) / mps) * mps;
        }
    }
    xSemaphoreGive(s_state.state_mutex);

    if (pipe->dev_hdl == NULL) {
        return -ENODEV;
    }
    if (pipe->payload_len > CONFIG_USBIP_MAX_TRANSFER) {
        return -EMSGSIZE;
    }
    /* EP0 IN (tyeth-ai-assisted/esp-usbip-bridge#37): the HAL needs IN transfer sizes to be
       multiples of the endpoint MPS. An unrounded data stage is a packet-level error for a
       device that answers with a longer packet than wLength (seen on ESP32-C5 USB-Serial/JTAG,
       bMaxPacketSize0 64, short GET_DESCRIPTOR -> EPROTO). Round the data stage up as the
       enumeration path does; the client still gets at most wLength (see complete_transfer). */
    if (pipe->is_control && pipe->is_in && pipe->payload_len > 0) {
        const uint16_t mps0 = ep0_mps_for(pipe->dev_hdl, NULL);
        const size_t rounded = ((pipe->payload_len + mps0 - 1) / mps0) * mps0;
        xfer_len = USB_SETUP_PACKET_SIZE + rounded;
    }

    usb_transfer_t *transfer = NULL;
    /* A zero-length packet (e.g. the PICOBOOT ACK of an IN command) still needs a buffer. */
    esp_err_t err = usb_host_transfer_alloc(xfer_len ? xfer_len : 4, 0, &transfer);
    if (err != ESP_OK || transfer == NULL) {
        /* Name the size: a silent -ENOMEM cost a debugging session. */
        ESP_LOGE(TAG, "usb_host_transfer_alloc(%u) failed: %s (ep=0x%02x)",
                 (unsigned)xfer_len, esp_err_to_name(err), pipe->endpoint_addr);
        return -ENOMEM;
    }

    if (pipe->is_control) {
        memcpy(transfer->data_buffer, &pipe->setup, USB_SETUP_PACKET_SIZE);
        if (!pipe->is_in && req->out_len > 0 && req->out_data != NULL) {
            memcpy(transfer->data_buffer + USB_SETUP_PACKET_SIZE,
                   req->out_data, req->out_len);
        }
    } else if (!pipe->is_in && req->out_len > 0 && req->out_data != NULL) {
        memcpy(transfer->data_buffer, req->out_data, req->out_len);
    }

    transfer->callback = xfer_done_cb;
    transfer->context = pipe;
    transfer->device_handle = pipe->dev_hdl;
    transfer->bEndpointAddress = pipe->endpoint_addr;
    transfer->num_bytes = xfer_len;
    transfer->timeout_ms = 0;   /* not enforced by the host library; deadlines are ours */

    if (pipe->is_control) {
        err = usb_host_transfer_submit_control(s_state.client_hdl, transfer);
    } else {
        err = usb_host_transfer_submit(transfer);
    }
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "submit failed: %s (ep=0x%02x)",
                 esp_err_to_name(err), pipe->endpoint_addr);
        usb_host_transfer_free(transfer);
        if (err != ESP_ERR_INVALID_STATE) {
            return -EIO;
        }
        /* A data pipe refuses URBs while it is halted (Linux answers
           -EPIPE).  The default pipe refuses them while the host library
           recovers it (after a STALL, an error or a cancel it stays halted
           until usbh_process() clears it) and once the device is gone; the
           device slot was found above, so retry until the deadline. */
        return pipe->is_control ? -EAGAIN : -EPIPE;
    }

    pipe->xfer = transfer;
    pipe->submitted = true;
    pipe->completed = false;
    pipe->cancel_sent = false;
    return 0;
}

/* Finish a transfer whose callback has fired: read the result, copy IN
   data to the request, free the host transfer, and return the errno for
   the caller. */
static const char *usb_status_name(usb_transfer_status_t s)
{
    switch (s) {
    case USB_TRANSFER_STATUS_COMPLETED: return "COMPLETED";
    case USB_TRANSFER_STATUS_ERROR: return "ERROR";
    case USB_TRANSFER_STATUS_TIMED_OUT: return "TIMED_OUT";
    case USB_TRANSFER_STATUS_CANCELED: return "CANCELED";
    case USB_TRANSFER_STATUS_STALL: return "STALL";
    case USB_TRANSFER_STATUS_OVERFLOW: return "OVERFLOW";
    case USB_TRANSFER_STATUS_SKIPPED: return "SKIPPED";
    case USB_TRANSFER_STATUS_NO_DEVICE: return "NO_DEVICE";
    default: return "?";
    }
}

static int complete_transfer(xfer_t *pipe, size_t *in_len)
{
    usb_transfer_t *transfer = pipe->xfer;
    usb_backend_req_t *req = pipe->req;
    const int status = map_transfer_status_to_errno(transfer->status);

    *in_len = 0;
    if (status != 0) {
        /* Error class visibility (issue #37): the transfer status plus the HCD's channel
           error (XCS_XACT, BNA, PKT_BBL, STALL), read before the transfer is freed. */
        const char *detail = usb_host_transfer_error_detail(transfer);
        if (req != NULL) {
            req->error_detail = detail;
        }
        if (transfer->status == USB_TRANSFER_STATUS_CANCELED) {
            ESP_LOGD(TAG, "transfer cancelled (ep=0x%02x)", pipe->endpoint_addr);
        } else {
            ESP_LOGW(TAG, "%s ep 0x%02x%s: transfer failed usb_status=%s errno=%d channel_error=%s",
                     pipe->busid, pipe->endpoint_addr, pipe->is_control ? " (control)" : "",
                     usb_status_name(transfer->status), status, detail);
        }
    }

    /* Copy IN data on success. */
    if (status == 0 && pipe->is_in && req != NULL && req->in_data != NULL
        && req->in_capacity > 0) {
        size_t bytes;
        if (pipe->is_control) {
            bytes = (transfer->actual_num_bytes > USB_SETUP_PACKET_SIZE)
                        ? (transfer->actual_num_bytes - USB_SETUP_PACKET_SIZE)
                        : 0;
            /* Never report more than the client asked for (wLength), even when the
               rounded data stage returned a longer packet (issue #37). */
            const size_t asked = (pipe->setup.wLength < pipe->payload_len)
                                     ? pipe->setup.wLength : pipe->payload_len;
            if (bytes > asked) {
                uint8_t dev_addr = 0;
                (void)ep0_mps_for(pipe->dev_hdl, &dev_addr);
                usb_backend_device_slot_t *dev = NULL;
                xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
                const int slot = find_slot_by_busid_locked(pipe->busid);
                if (slot >= 0 && !s_state.devices[slot].ep0_round_logged) {
                    s_state.devices[slot].ep0_round_logged = true;
                    dev = &s_state.devices[slot];
                }
                xSemaphoreGive(s_state.state_mutex);
                if (dev != NULL) {
                    ESP_LOGW(TAG, "%s (device address %u): control IN returned %u bytes for wLength %u; "
                             "data stage rounded to EP0 MPS, reporting %u (tyeth-ai-assisted/esp-usbip-bridge#37)",
                             pipe->busid, (unsigned)dev_addr, (unsigned)bytes, (unsigned)asked, (unsigned)asked);
                }
                bytes = asked;
            }
        } else {
            bytes = transfer->actual_num_bytes;
            if (bytes > pipe->payload_len) {
                bytes = pipe->payload_len;
            }
        }
        const size_t copy_len =
            (bytes > req->in_capacity) ? req->in_capacity : bytes;
        const uint8_t *src = pipe->is_control
            ? (transfer->data_buffer + USB_SETUP_PACKET_SIZE)
            : transfer->data_buffer;
        memcpy(req->in_data, src, copy_len);
        *in_len = copy_len;
    }

    usb_host_transfer_free(transfer);
    pipe->xfer = NULL;

    return status;
}

static int count_in_on_endpoint_locked(const char busid[32], uint8_t endpoint_addr)
{
    int n = 0;
    for (const xfer_t *x = s_state.inflight_head; x != NULL; x = x->next) {
        if (x->endpoint_addr == endpoint_addr && busid_matches_key(busid, x->busid)) {
            n++;
        }
    }
    return n;
}

static void inflight_append_locked(xfer_t *x)
{
    x->next = NULL;
    if (s_state.inflight_tail != NULL) {
        s_state.inflight_tail->next = x;
    } else {
        s_state.inflight_head = x;
    }
    s_state.inflight_tail = x;
    s_state.num_inflight++;
    if (s_state.num_inflight > s_state.inflight_peak) {
        s_state.inflight_peak = s_state.num_inflight;
    }
    if (!x->is_control && x->is_in) {
        s_state.num_in_inflight++;
    }
}

/* Unlink a transfer from the in-flight list.  If its device's USB/IP
   session ended while the transfer was still in flight (so the session
   could not release the interfaces, see release_session_device()), release
   them once the device has no transfer left. */
static void inflight_remove_locked(xfer_t *x)
{
    xfer_t *prev = NULL;
    for (xfer_t *cur = s_state.inflight_head; cur != NULL; prev = cur, cur = cur->next) {
        if (cur != x) {
            continue;
        }
        if (prev != NULL) {
            prev->next = x->next;
        } else {
            s_state.inflight_head = x->next;
        }
        if (s_state.inflight_tail == x) {
            s_state.inflight_tail = prev;
        }
        s_state.num_inflight--;
        if (!x->is_control && x->is_in) {
            s_state.num_in_inflight--;
        }
        break;
    }

    const int slot = find_slot_by_busid_locked(x->busid);
    if (slot >= 0 && s_state.devices[slot].release_pending && !busid_in_flight_locked(x->busid)) {
        s_state.devices[slot].release_pending = false;
        release_interfaces_locked(slot);
        s_state.devices[slot].halted_eps = 0;
        ESP_LOGI(TAG, "%s: last transfer retired after its session ended, interfaces released", x->busid);
    }
}

/* Retire a transfer (its host transfer already freed) and answer its
   request, if it still has one. */
static void finish_xfer(xfer_t *x, int status, size_t in_len)
{
    usb_backend_req_t *req = x->req;

    xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
    inflight_remove_locked(x);
    xSemaphoreGive(s_state.state_mutex);
    free(x);

    if (req != NULL) {
        req->done(req, status, in_len);
    }
}

typedef struct {
    usb_device_handle_t dev_hdl;
    uint8_t endpoint_addr;
} flushed_ep_t;

/* Ask the host library to retire an aborted transfer.
   - Control: usb_host_transfer_cancel_control().  A transfer that is only
     queued is retired alone; one on the bus needs EP0 halted and flushed,
     which retires every control transfer queued to the device.
   - Bulk/interrupt: usb_host_transfer_cancel() retires a transfer still
     queued behind others on its own.  One already in the endpoint's
     transfer buffers needs the endpoint halted and flushed, which retires
     every transfer queued on it.
   Transfers retired as collateral complete as CANCELED without having been
   aborted; process_inflight() submits them again, in order (Linux keeps
   the URBs queued behind an unlinked one). */
static void request_cancel(xfer_t *x, TickType_t now, flushed_ep_t *flushed, int *num_flushed)
{
    const char *why = x->req->cancel ? "unlinked" : "timed out";

    if (x->dev_hdl == NULL) {
        return;
    }
    if (x->is_control) {
        esp_err_t cerr = usb_host_transfer_cancel_control(s_state.client_hdl, x->xfer);
        if (cerr != ESP_OK && cerr != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "%s: control cancel failed: %s", x->busid, esp_err_to_name(cerr));
            x->cancel_tick = now - pdMS_TO_TICKS(USB_BACKEND_CANCEL_WAIT_MS);
        } else {
            ESP_LOGI(TAG, "%s: control transfer %s, cancelled (bmRequestType 0x%02x bRequest 0x%02x wValue 0x%04x wIndex 0x%04x)",
                     x->busid, why,
                     x->setup.bmRequestType, x->setup.bRequest,
                     x->setup.wValue, x->setup.wIndex);
        }
        return;
    }

    esp_err_t cerr = usb_host_transfer_cancel(x->xfer);
    if (cerr == ESP_OK || cerr == ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "%s ep 0x%02x: %s, retired on its own", x->busid, x->endpoint_addr, why);
        return;
    }
    if (cerr != ESP_ERR_NOT_FINISHED) {
        ESP_LOGW(TAG, "%s ep 0x%02x: cancel failed: %s", x->busid, x->endpoint_addr, esp_err_to_name(cerr));
        x->cancel_tick = now - pdMS_TO_TICKS(USB_BACKEND_CANCEL_WAIT_MS);
        return;
    }
    for (int i = 0; i < *num_flushed; i++) {
        if (flushed[i].dev_hdl == x->dev_hdl && flushed[i].endpoint_addr == x->endpoint_addr) {
            return;     /* flushed earlier in this pass */
        }
    }
    if (*num_flushed < USB_BACKEND_MAX_FLUSHED_PER_PASS) {
        flushed[*num_flushed].dev_hdl = x->dev_hdl;
        flushed[*num_flushed].endpoint_addr = x->endpoint_addr;
        (*num_flushed)++;
    }
    ESP_LOGD(TAG, "%s ep 0x%02x: %s while in flight, halting and flushing the endpoint",
             x->busid, x->endpoint_addr, why);
    usb_host_endpoint_halt(x->dev_hdl, x->endpoint_addr);
    usb_host_endpoint_flush(x->dev_hdl, x->endpoint_addr);
    usb_host_endpoint_clear(x->dev_hdl, x->endpoint_addr);
}

/* Completions, cancels and deadlines of the transfers in flight. */
static void process_inflight(TickType_t now)
{
    flushed_ep_t flushed[USB_BACKEND_MAX_FLUSHED_PER_PASS];
    int num_flushed = 0;
    xfer_t *next;

    for (xfer_t *x = s_state.inflight_head; x != NULL; x = next) {
        next = x->next;
        if (!x->submitted) {
            continue;       /* submit_pending() runs it */
        }

        if (x->req == NULL) {
            /* Orphan: its caller was answered; free the host transfer
               once the device answers, or it is gone. */
            if (x->completed) {
                ESP_LOGD(TAG, "%s ep 0x%02x: orphaned transfer completed (usb_status=%d)",
                         x->busid, x->endpoint_addr, x->xfer->status);
                usb_host_transfer_free(x->xfer);
                x->xfer = NULL;
                finish_xfer(x, 0, 0);
            } else if (!x->is_control && x->dev_hdl != NULL
                       && (now - x->cancel_tick) >= pdMS_TO_TICKS(USB_BACKEND_CANCEL_WAIT_MS)) {
                /* Round 2m: an orphan still executing after its cancel keeps its endpoint's
                   in-flight slot (and the endpoint's pipe) busy. Re-issue the cancel, with
                   the halt/flush/clear that retires a stuck pipe, once per wait period
                   until the host completes it. Without this a silent device wedges the
                   endpoint for good. */
                x->cancel_tick = now;
                ESP_LOGW(TAG, "%s ep 0x%02x: orphaned transfer still executing, re-cancelling",
                         x->busid, x->endpoint_addr);
                if (usb_host_transfer_cancel(x->xfer) == ESP_ERR_NOT_FINISHED) {
                    usb_host_endpoint_halt(x->dev_hdl, x->endpoint_addr);
                    usb_host_endpoint_flush(x->dev_hdl, x->endpoint_addr);
                    usb_host_endpoint_clear(x->dev_hdl, x->endpoint_addr);
                }
            }
            continue;
        }

        if (!x->aborted && x->req->cancel) {
            x->aborted = true;
            ESP_LOGD(TAG, "transfer cancelled (ep=0x%02x)", x->endpoint_addr);
        }
        if (!x->aborted && x->has_deadline && (int32_t)(now - x->deadline) >= 0) {
            x->aborted = true;
            ESP_LOGD(TAG, "transfer timed out (ep=0x%02x)", x->endpoint_addr);
        }

        if (x->aborted && !x->completed) {
            if (!x->cancel_sent) {
                x->cancel_sent = true;
                x->cancel_tick = now;
                request_cancel(x, now, flushed, &num_flushed);
            }
            if ((now - x->cancel_tick) < pdMS_TO_TICKS(USB_BACKEND_CANCEL_WAIT_MS)) {
                continue;
            }
            /* Still in flight after the cancel: answer the caller now and
               keep the host transfer until it really completes (freeing it
               earlier lets the stack write into freed heap). */
            ESP_LOGW(TAG, "%s ep 0x%02x: cancelled transfer did not complete, holding its host transfer",
                     x->busid, x->endpoint_addr);
            usb_backend_req_t *req = x->req;
            x->req = NULL;
            req->done(req, req->cancel ? -ECONNRESET : -ETIMEDOUT, 0);
            continue;
        }

        if (!x->completed) {
            continue;
        }

        const usb_transfer_status_t usb_status = x->xfer->status;
        if (usb_status == USB_TRANSFER_STATUS_CANCELED && !x->aborted) {
            if (x->req->cancel) {
                /* Its own cancel arrived meanwhile: no need to run it again. */
                x->aborted = true;
            } else if (x->resubmits < USB_BACKEND_MAX_RESUBMITS) {
                /* Retired by a halt/flush it did not ask for: submit it
                   again, as Linux keeps URBs queued behind an unlinked or
                   failed one. */
                x->resubmits++;
                ESP_LOGD(TAG, "%s ep 0x%02x: transfer retired by a flush, resubmitting (%u)",
                         x->busid, x->endpoint_addr, x->resubmits);
                usb_host_transfer_free(x->xfer);
                x->xfer = NULL;
                x->completed = false;
                x->submitted = false;   /* submit_pending() runs it, in order */
                continue;
            }
        }

        size_t in_len = 0;
        int status = complete_transfer(x, &in_len);

        if (x->aborted && usb_status == USB_TRANSFER_STATUS_CANCELED) {
            /* We forced the abort (unless the transfer finished on its own
               before it took effect). */
            status = x->req->cancel ? -ECONNRESET : -ETIMEDOUT;
            in_len = 0;
        } else if (x->is_control) {
            if (status == 0) {
                apply_standard_request_to_host(x);
            }
        } else if (status != 0 && x->dev_hdl != NULL) {
            note_transfer_failure(x, usb_status);
            /* URBs queued behind a STALL are flushed by the host library;
               they failed because the endpoint halted. */
            if (usb_status == USB_TRANSFER_STATUS_CANCELED && endpoint_is_halted(x)) {
                status = -EPIPE;
            }
        }

        finish_xfer(x, status, in_len);
    }
}

/* Move waiting requests into flight, oldest first.  A request cancelled
   while waiting is answered here.  Reads (bulk/interrupt IN) are held back
   while their endpoint, or reads as a whole, hold their share; requests
   for other endpoints go ahead of them, so pending reads never delay OUT
   data or control requests. */
static void admit_waiting(TickType_t now)
{
    usb_backend_req_t *answer = NULL;      /* cancelled or failed while waiting */

    xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
    usb_backend_req_t *prev = NULL;
    usb_backend_req_t *req = s_state.waiting_head;
    while (req != NULL) {
        usb_backend_req_t *next = req->next;
        const bool is_control = req->endpoint_addr == 0;
        const bool is_in = is_control
            ? (req->setup.bmRequestType & USB_BM_REQUEST_TYPE_DIR_IN) != 0
            : (req->endpoint_addr & 0x80) != 0;
        xfer_t *x = NULL;
        int fail = 0;

        if (req->cancel) {
            fail = -ECONNRESET;
        } else if (s_state.num_inflight >= USB_BACKEND_MAX_INFLIGHT) {
            break;      /* nothing behind can go either; keeps the order */
        } else if (!is_control && is_in
                   && (s_state.num_in_inflight >= USB_BACKEND_MAX_IN_INFLIGHT
                       || count_in_on_endpoint_locked(req->busid, req->endpoint_addr)
                          >= USB_BACKEND_MAX_IN_PER_EP)) {
            prev = req;
            req = next;
            continue;
        } else {
            x = calloc(1, sizeof(*x));
            if (x == NULL) {
                fail = -ENOMEM;
            }
        }

        /* Unlink the request from the waiting queue. */
        if (prev != NULL) {
            prev->next = next;
        } else {
            s_state.waiting_head = next;
        }
        if (s_state.waiting_tail == req) {
            s_state.waiting_tail = prev;
        }

        if (x == NULL) {
            req->status = fail;
            req->next = answer;
            answer = req;
        } else {
            x->req = req;
            x->seq = req->seq;
            memcpy(x->busid, req->busid, sizeof(x->busid));
            x->endpoint_addr = req->endpoint_addr;
            x->setup = req->setup;
            x->is_control = is_control;
            x->is_in = is_in;
            x->payload_len = is_in ? req->in_capacity : req->out_len;
            if (is_control && USB_BACKEND_CTRL_TIMEOUT_MS > 0) {
                x->has_deadline = true;
                x->deadline = now + pdMS_TO_TICKS(USB_BACKEND_CTRL_TIMEOUT_MS);
            }
            inflight_append_locked(x);
            /* The device is in use again: do not release its interfaces
               under this session when an old orphan retires. */
            const int slot = find_slot_by_busid_locked(req->busid);
            if (slot >= 0) {
                s_state.devices[slot].release_pending = false;
            }
        }
        req = next;
    }
    xSemaphoreGive(s_state.state_mutex);

    while (answer != NULL) {
        usb_backend_req_t *done = answer;
        answer = done->next;
        done->done(done, done->status, 0);
    }
}

/* Hand new and resubmitted transfers to the host library, in order. */
static void submit_pending(void)
{
    xfer_t *next;

    for (xfer_t *x = s_state.inflight_head; x != NULL; x = next) {
        next = x->next;
        if (x->submitted || x->req == NULL) {
            continue;
        }
        int ret = prepare_and_submit_transfer(x);
        if (ret == -EAGAIN) {
            /* EP0 is being recovered: try again on the next pass. */
            if (x->req->cancel) {
                ret = -ECONNRESET;
            } else if (x->has_deadline && (int32_t)(xTaskGetTickCount() - x->deadline) >= 0) {
                ret = -ETIMEDOUT;
            } else {
                continue;
            }
        }
        if (ret < 0) {
            finish_xfer(x, ret, 0);
        }
    }
}

static void usb_backend_daemon_task(void *arg)
{
    (void)arg;

    while (true) {
        uint32_t event_flags = 0;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "usb_host_lib_handle_events failed: %s", esp_err_to_name(err));
            continue;
        }

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void usb_backend_task(void *arg)
{
    (void)arg;

    usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = USB_BACKEND_EVENT_QUEUE_LEN,
        .async = {
            .client_event_callback = usb_client_event_cb,
            .callback_arg = &s_state,
        },
    };

    esp_err_t err = usb_host_client_register(&client_config, &s_state.client_hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_client_register failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGD(TAG, "USB backend task running (up to %d transfers in flight, %d reads per endpoint)",
             USB_BACKEND_MAX_INFLIGHT, USB_BACKEND_MAX_IN_PER_EP);

    while (true) {
        /* Blocks until a transfer completes or a device event arrives
           (usb_backend_submit() and usb_backend_cancel() unblock it too),
           for at most the poll period.  Completion callbacks run in here
           and set xfer->completed. */
        err = usb_host_client_handle_events(s_state.client_hdl, pdMS_TO_TICKS(USB_BACKEND_POLL_MS));
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGD(TAG, "usb_host_client_handle_events failed: %s", esp_err_to_name(err));
        }

        process_backend_events();

        const TickType_t now = xTaskGetTickCount();
        process_inflight(now);
        admit_waiting(now);
        submit_pending();
    }
}

esp_err_t usb_backend_start(void)
{
    memset(&s_state, 0, sizeof(s_state));

#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_USBIP_S3_USB_OTG_DEVKIT_POWER
    esp_err_t board_power_err = usb_backend_s3_usb_otg_devkit_power_init();
    if (board_power_err != ESP_OK) {
        return board_power_err;
    }
#endif

#if CONFIG_IDF_TARGET_ESP32P4 && CONFIG_USBIP_P4HIL_USB_POWER_ENABLE
    esp_err_t p4hil_power_err = usb_backend_p4hil_usb_power_init();
    if (p4hil_power_err != ESP_OK) {
        return p4hil_power_err;
    }
#endif

    s_state.state_mutex = xSemaphoreCreateMutex();
    if (s_state.state_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_state.event_queue = xQueueCreate(USB_BACKEND_EVENT_QUEUE_LEN, sizeof(usb_backend_event_t));
    if (s_state.event_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = 0,
    };

    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(usb_backend_daemon_task,
                    "usb_host_daemon",
                    USB_BACKEND_DAEMON_TASK_STACK,
                    NULL,
                    USB_BACKEND_DAEMON_TASK_PRIORITY,
                    NULL) != pdPASS) {
        return ESP_FAIL;
    }

    if (xTaskCreate(usb_backend_task,
                    "usb_backend",
                    USB_BACKEND_TASK_STACK,
                    NULL,
                    USB_BACKEND_TASK_PRIORITY,
                    &s_state.task_hdl) != pdPASS) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

size_t usb_backend_get_devices(usbip_backend_device_t *out_devices, size_t max_devices)
{
    if (out_devices == NULL || max_devices == 0) {
        return 0;
    }

    size_t copied = 0;

    if (backend_started()) {
        xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
        for (int i = 0; i < CONFIG_USBIP_MAX_DEVICES && copied < max_devices; i++) {
            if (!s_state.devices[i].in_use) {
                continue;
            }

            out_devices[copied++] = s_state.devices[i].device;
        }
        xSemaphoreGive(s_state.state_mutex);
    }

    /* Append virtual devices. */
    if (copied < max_devices) {
        copied += virtual_device_get_all(out_devices + copied, max_devices - copied);
    }

    return copied;
}

static int count_in_flight_locked(const char busid[32])
{
    int n = 0;
    for (const xfer_t *x = s_state.inflight_head; x != NULL; x = x->next) {
        if (busid_matches_key(busid, x->busid)) {
            n++;
        }
    }
    return n;
}

size_t usb_backend_get_claims(usb_backend_claims_device_t *out, size_t max,
                              usb_backend_claims_totals_t *totals)
{
    size_t copied = 0;
    usb_backend_claims_totals_t sums;
    memset(&sums, 0, sizeof(sums));

    /* Before usb_backend_start() the table is empty; the host library
       stats below report "unknown" (-1) on their own. */
    if (backend_started()) {
        xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
        for (int i = 0; i < CONFIG_USBIP_MAX_DEVICES; i++) {
            if (!s_state.devices[i].in_use) {
                continue;
            }
            uint8_t eps[USBIP_MAX_ENDPOINTS];
            const int num_eps = collect_claimed_endpoints_locked(i, -1, eps, USBIP_MAX_ENDPOINTS);
            usb_backend_claims_device_t scratch;
            usb_backend_claims_device_t *d = (out != NULL && copied < max) ? &out[copied] : &scratch;
            memset(d, 0, sizeof(*d));
            strlcpy(d->busid, s_state.devices[i].device.busid, sizeof(d->busid));
            int num_intf = 0;
            for (int n = 0; n < USBIP_MAX_INTERFACES; n++) {
                if (s_state.devices[i].claimed_interfaces[n]) {
                    d->claimed_interfaces[num_intf++] = (uint8_t)n;
                }
            }
            d->num_claimed_interfaces = num_intf;
            d->num_claimed_endpoints = num_eps;
            d->in_flight = count_in_flight_locked(s_state.devices[i].device.busid);
            d->release_pending = s_state.devices[i].release_pending;
            d->claim_failures = s_state.devices[i].claim_failures;
            if (s_state.devices[i].last_claim_err != ESP_OK) {
                strlcpy(d->last_claim_error, esp_err_to_name(s_state.devices[i].last_claim_err),
                        sizeof(d->last_claim_error));
                d->last_claim_was_exhaustion =
                    (s_state.devices[i].last_claim_err == ESP_ERR_NOT_SUPPORTED);
            }
            if (out != NULL && copied < max) {
                copied++;
            }
            sums.connected_devices++;
            sums.claimed_interfaces += num_intf;
            sums.claimed_endpoints += num_eps;
        }
        sums.exhaustion_events = s_state.exhaustion_events;
        sums.last_exhaustion_us = s_state.last_exhaustion_us;
        strlcpy(sums.last_exhaustion_busid, s_state.last_exhaustion_busid,
                sizeof(sums.last_exhaustion_busid));
        xSemaphoreGive(s_state.state_mutex);
    }

    /* Hubs are outside the device table; each holds one interrupt channel. */
    usb_backend_hub_t hubs[USB_BACKEND_MAX_HUBS];
    sums.external_hubs = (int)usb_backend_get_hubs(hubs, USB_BACKEND_MAX_HUBS);

    /* The HCD's own ledger (issue #20's authoritative accounting). */
    sums.hcd_channels_total = -1;
    sums.hcd_channels_allocated = -1;
    sums.hcd_channel_waiters = -1;
    int hcd_total = 0;
    int hcd_allocated = 0;
    if (usb_host_channel_stats(&hcd_total, &hcd_allocated) == ESP_OK) {
        sums.hcd_channels_total = hcd_total;
        sums.hcd_channels_allocated = hcd_allocated;
    }
    int hcd_waiters = 0;
    if (usb_host_channel_waiters(&hcd_waiters) == ESP_OK) {
        sums.hcd_channel_waiters = hcd_waiters;
    }
    /* Parked-listener and rotation counters (phase 2f). */
    sums.parked_bound = -1;
    sums.parked_intr_bound = -1;
    sums.rotor_active = -1;
    sums.promotions = -1;
    sums.yields_req = -1;
    sums.yields_done = -1;
    sums.cap_refusals = -1;
    sums.reserve_refusals = -1;
    sums.rot_passes = -1;
    sums.rej_not_bound = -1;
    sums.rej_not_parked = -1;
    sums.rej_pause = -1;
    sums.rej_halt_wait = -1;
    sums.rej_state = -1;
    sums.rej_not_exec = -1;
    sums.rej_urb_null = -1;
    sums.rej_too_big = -1;
    sums.halt_noop = -1;
    usb_host_parked_stats_t ps;
    if (usb_host_parked_stats(&ps) == ESP_OK) {
        sums.parked_bound = ps.parked_bound;
        sums.parked_intr_bound = ps.parked_intr_bound;
        sums.rotor_active = ps.rotor_active ? 1 : 0;
        sums.promotions = (int)ps.promotions;
        sums.yields_req = (int)ps.yields_req;
        sums.yields_done = (int)ps.yields_done;
        sums.cap_refusals = (int)ps.cap_refusals;
        sums.reserve_refusals = (int)ps.reserve_refusals;
        sums.rot_passes = (int)ps.rot_passes;
        sums.rej_not_bound = (int)ps.rej_not_bound;
        sums.rej_not_parked = (int)ps.rej_not_parked;
        sums.rej_pause = (int)ps.rej_pause;
        sums.rej_halt_wait = (int)ps.rej_halt_wait;
        sums.rej_state = (int)ps.rej_state;
        sums.rej_not_exec = (int)ps.rej_not_exec;
        sums.rej_urb_null = (int)ps.rej_urb_null;
        sums.rej_too_big = (int)ps.rej_too_big;
        sums.halt_noop = (int)ps.halt_noop;
    }

    /* Leak detector (round 2m): transfers in flight now, and the peak since boot. */
    sums.inflight_now = s_state.num_inflight;
    sums.inflight_peak = s_state.inflight_peak;
    sums.heap_free = (int)heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    sums.heap_min = (int)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    sums.dma_free = (int)heap_caps_get_free_size(MALLOC_CAP_DMA);
    sums.dma_largest = (int)heap_caps_get_largest_free_block(MALLOC_CAP_DMA);

    if (totals != NULL) {
        *totals = sums;
    }
    return copied;
}

/* Hubs are owned by the USB Host Library and never reported to clients, so
   ask the library for them and rebuild each hub's port path from the parent
   links (a hub's parent is always another hub or the root port). */
size_t usb_backend_get_hubs(usb_backend_hub_t *out_hubs, size_t max_hubs)
{
    if (out_hubs == NULL || max_hubs == 0 || s_state.client_hdl == NULL) {
        return 0;
    }
    uint8_t addrs[USB_BACKEND_MAX_HUBS];
    size_t count = 0;
    if (usb_host_hub_list(addrs, USB_BACKEND_MAX_HUBS, &count) != ESP_OK) {
        return 0;
    }
    if (count > USB_BACKEND_MAX_HUBS) {
        count = USB_BACKEND_MAX_HUBS;
    }

    usb_host_hub_info_t infos[USB_BACKEND_MAX_HUBS];
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        if (usb_host_hub_get_info(addrs[i], &infos[n]) == ESP_OK) {
            n++;
        }
    }

    size_t copied = 0;
    for (size_t i = 0; i < n && copied < max_hubs; i++) {
        usb_backend_hub_t *hub = &out_hubs[copied++];
        memset(hub, 0, sizeof(*hub));
        hub->addr = infos[i].dev_addr;
        hub->parent_hub_addr = infos[i].parent_addr;
        hub->parent_port = infos[i].parent_port;
        hub->id_vendor = infos[i].vid;
        hub->id_product = infos[i].pid;
        strlcpy(hub->manufacturer, infos[i].manufacturer, sizeof(hub->manufacturer));
        strlcpy(hub->product, infos[i].product, sizeof(hub->product));

        /* Walk up the parent chain collecting port numbers */
        uint8_t ports[USB_BACKEND_MAX_HUB_DEPTH];
        size_t depth = 0;
        const usb_host_hub_info_t *cur = &infos[i];
        while (cur != NULL && cur->parent_addr != 0 && depth < USB_BACKEND_MAX_HUB_DEPTH) {
            ports[depth++] = cur->parent_port;
            const usb_host_hub_info_t *parent = NULL;
            for (size_t k = 0; k < n; k++) {
                if (infos[k].dev_addr == cur->parent_addr) {
                    parent = &infos[k];
                    break;
                }
            }
            cur = parent;
        }
        size_t off = strlcpy(hub->path, "1-1", sizeof(hub->path));
        while (depth > 0 && off < sizeof(hub->path)) {
            off += snprintf(hub->path + off, sizeof(hub->path) - off, ".%u", ports[--depth]);
        }
    }
    return copied;
}

bool usb_backend_get_device_by_busid(const char busid[32], usbip_backend_device_t *out_device)
{
    if (busid == NULL || out_device == NULL) {
        return false;
    }

    bool found = false;

    if (backend_started()) {
        xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
        const int slot = find_slot_by_busid_locked(busid);
        if (slot >= 0) {
            *out_device = s_state.devices[slot].device;
            found = true;
        }
        xSemaphoreGive(s_state.state_mutex);
    }

    if (!found) {
        virtual_device_t *vdev = virtual_device_find_by_busid(busid);
        if (vdev != NULL) {
            *out_device = vdev->desc;
            found = true;
        }
    }

    return found;
}

void usb_backend_submit(usb_backend_req_t *req)
{
    if (req == NULL || req->done == NULL) {
        return;
    }
    if (!backend_started()) {
        /* No backend task to answer it: done() always follows a submit. */
        req->done(req, -ENODEV, 0);
        return;
    }

    req->next = NULL;
    req->status = 0;
    req->error_detail = NULL;
    xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
    req->seq = s_state.next_seq++;
    if (s_state.waiting_tail != NULL) {
        s_state.waiting_tail->next = req;
    } else {
        s_state.waiting_head = req;
    }
    s_state.waiting_tail = req;
    xSemaphoreGive(s_state.state_mutex);

    wake_backend();
}

void usb_backend_cancel(usb_backend_req_t *req)
{
    if (req == NULL) {
        return;
    }
    req->cancel = true;
    wake_backend();
}

bool usb_backend_is_interrupt_endpoint(const char busid[32], uint8_t ep_num, uint8_t direction)
{
    if (busid == NULL || !backend_started()) {
        return false;
    }

    const uint8_t ep_addr = ep_num | (direction ? 0x80 : 0x00);

    xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
    const int slot = find_slot_by_busid_locked(busid);
    if (slot < 0) {
        xSemaphoreGive(s_state.state_mutex);
        return false;
    }

    const usbip_backend_device_t *device = &s_state.devices[slot].device;
    for (uint8_t i = 0; i < device->num_endpoints; i++) {
        if (device->endpoints[i].address == ep_addr) {
            bool is_intr = (device->endpoints[i].attributes & 0x03) == 0x03;
            xSemaphoreGive(s_state.state_mutex);
            return is_intr;
        }
    }

    xSemaphoreGive(s_state.state_mutex);
    return false;
}

/* One transfer through usb_backend_submit(), waited for: the request and
   buffers stay on this stack until done() has run.  A transfer still
   pending at timeout_ms (a bulk IN with no data, a control request the
   device never finishes) is cancelled, and done() is waited for, which
   always follows a cancel. */
typedef struct {
    SemaphoreHandle_t done;
    int status;
    size_t in_len;
    const char *error_detail;
} sync_xfer_t;

static void sync_xfer_done(usb_backend_req_t *req, int status, size_t in_len)
{
    sync_xfer_t *d = (sync_xfer_t *)req->ctx;
    d->status = status;
    d->in_len = in_len;
    d->error_detail = req->error_detail;
    xSemaphoreGive(d->done);
}

int usb_backend_transfer_sync(const char busid[32], uint8_t endpoint_addr,
                              const usb_setup_packet_t *setup,
                              const uint8_t *out_data, size_t out_len,
                              uint8_t *in_buf, size_t in_capacity, uint32_t timeout_ms,
                              size_t *in_len, const char **error_detail)
{
    if (!backend_started()) {
        if (in_len != NULL) {
            *in_len = 0;
        }
        if (error_detail != NULL) {
            *error_detail = NULL;
        }
        return -ENODEV;
    }
    sync_xfer_t d = {0};
    d.done = xSemaphoreCreateBinary();
    if (d.done == NULL) {
        return -ENOMEM;
    }
    usb_backend_req_t req;
    memset(&req, 0, sizeof(req));
    strlcpy(req.busid, busid, sizeof(req.busid));
    req.endpoint_addr = endpoint_addr;
    if (setup != NULL) {
        req.setup = *setup;
    }
    req.out_data = out_data;
    req.out_len = out_len;
    req.in_data = in_buf;
    req.in_capacity = in_capacity;
    req.done = sync_xfer_done;
    req.ctx = &d;

    usb_backend_submit(&req);
    if (xSemaphoreTake(d.done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        usb_backend_cancel(&req);
        xSemaphoreTake(d.done, portMAX_DELAY);     /* done() always follows a cancel */
    }
    vSemaphoreDelete(d.done);
    if (in_len != NULL) {
        *in_len = d.in_len;
    }
    if (error_detail != NULL) {
        *error_detail = d.error_detail;
    }
    return d.status;
}

/* Diagnostics (issue #37): submit one control transfer through the same backend request
   path as a forwarded EP0 request (usb_backend_submit -> admit -> prepare/round/submit ->
   complete) and wait for its completion. */
int usb_backend_debug_control(const char busid[32], const usb_setup_packet_t *setup,
                              uint8_t *in_buf, size_t in_capacity,
                              size_t *in_len, const char **error_detail)
{
    return usb_backend_transfer_sync(busid, 0, setup, NULL, 0, in_buf, in_capacity,
                                     CONFIG_USBIP_CTRL_XFER_TIMEOUT_MS + USB_BACKEND_CANCEL_WAIT_MS + 2000,
                                     in_len, error_detail);
}

size_t usb_backend_get_config_descriptor(const char busid[32], uint8_t *buf, size_t cap)
{
    if (busid == NULL || buf == NULL || !backend_started()) {
        return 0;
    }
    char key[32] = {0};
    strlcpy(key, busid, sizeof(key));
    size_t n = 0;
    xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
    const int slot = find_slot_by_busid_locked(key);
    if (slot >= 0 && s_state.devices[slot].config_desc != NULL) {
        n = s_state.devices[slot].config_desc_len;
        if (n > cap) {
            n = cap;
        }
        memcpy(buf, s_state.devices[slot].config_desc, n);
    }
    xSemaphoreGive(s_state.state_mutex);
    return n;
}

void usb_backend_arm_ep0_dump(int dev_addr, int count)
{
    (void)usb_host_debug_arm_ep0_dump((uint8_t)dev_addr, count);
}

/* Hub-class control (issue: hardware-true hub port status). A hub is not exported, so it has
   no entry in the export table; its handle is opened from the host library for the duration
   of the request and closed afterwards. */
static void hub_ctl_done_cb(usb_transfer_t *transfer)
{
    xSemaphoreGive((SemaphoreHandle_t)transfer->context);
}

esp_err_t usb_backend_hub_control(uint8_t hub_addr, const usb_setup_packet_t *setup,
                                  uint8_t *in_buf, size_t in_capacity,
                                  size_t *in_len, const char **err_class)
{
    *in_len = 0;
    *err_class = "none";
    if (s_state.client_hdl == NULL) {
        *err_class = "not_started";
        return ESP_ERR_INVALID_STATE;
    }
    usb_device_handle_t dev = NULL;
    esp_err_t err = usb_host_device_open(s_state.client_hdl, hub_addr, &dev);
    if (err != ESP_OK) {
        *err_class = "open";
        return err;
    }
    const uint16_t mps0 = ep0_mps_for(dev, NULL);
    const bool is_in = (setup->bmRequestType & USB_BM_REQUEST_TYPE_DIR_IN) != 0;
    const size_t data_len = is_in ? setup->wLength : 0;
    const size_t rounded = (data_len > 0) ? ((data_len + mps0 - 1) / mps0) * mps0 : 0;
    usb_transfer_t *t = NULL;
    err = usb_host_transfer_alloc(USB_SETUP_PACKET_SIZE + rounded, 0, &t);
    if (err != ESP_OK || t == NULL) {
        *err_class = "alloc";
        usb_host_device_close(s_state.client_hdl, dev);
        return ESP_ERR_NO_MEM;
    }
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (done == NULL) {
        *err_class = "alloc";
        usb_host_transfer_free(t);
        usb_host_device_close(s_state.client_hdl, dev);
        return ESP_ERR_NO_MEM;
    }
    memcpy(t->data_buffer, setup, USB_SETUP_PACKET_SIZE);
    t->device_handle = dev;
    t->bEndpointAddress = 0;
    t->num_bytes = USB_SETUP_PACKET_SIZE + rounded;
    t->callback = hub_ctl_done_cb;
    t->context = done;
    t->timeout_ms = 0;
    err = usb_host_transfer_submit_control(s_state.client_hdl, t);
    if (err != ESP_OK) {
        *err_class = "submit";
        vSemaphoreDelete(done);
        usb_host_transfer_free(t);
        usb_host_device_close(s_state.client_hdl, dev);
        return err;
    }
    if (xSemaphoreTake(done, pdMS_TO_TICKS(1500)) != pdTRUE) {
        /* Retire it; the transfer must not be freed while the host still owns it. */
        *err_class = "timeout";
        usb_host_transfer_cancel_control(s_state.client_hdl, t);
        xSemaphoreTake(done, portMAX_DELAY);
    }
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        size_t n = (t->actual_num_bytes > USB_SETUP_PACKET_SIZE) ? (t->actual_num_bytes - USB_SETUP_PACKET_SIZE) : 0;
        if (n > in_capacity) {
            n = in_capacity;
        }
        if (n > 0 && in_buf != NULL) {
            memcpy(in_buf, t->data_buffer + USB_SETUP_PACKET_SIZE, n);
        }
        *in_len = n;
        err = ESP_OK;
    } else {
        *err_class = usb_status_name(t->status);
        err = ESP_FAIL;
    }
    vSemaphoreDelete(done);
    usb_host_transfer_free(t);
    usb_host_device_close(s_state.client_hdl, dev);
    return err;
}

bool usb_backend_hub_addr_by_path(const char path[32], uint8_t *addr)
{
    usb_backend_hub_t hubs[USB_BACKEND_MAX_HUBS];
    const size_t n = usb_backend_get_hubs(hubs, USB_BACKEND_MAX_HUBS);
    for (size_t i = 0; i < n; i++) {
        if (strncmp(hubs[i].path, path, sizeof(hubs[i].path)) == 0) {
            if (addr != NULL) {
                *addr = hubs[i].addr;
            }
            return true;
        }
    }
    return false;
}

