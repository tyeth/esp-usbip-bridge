#include "serial_link.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "virtual_device.h"

#define LINK_CTRL_TIMEOUT_MS 1000
#define LINK_CFG_MAX         1024

/* What the bridge last set on a device's serial function, per enumeration. */
typedef struct {
    bool used;
    char busid[32];
    uint32_t devnum;
    bool lines_known;
    bool dtr, rts;
    uint32_t baud;              /* 0 = not set by the bridge */
    bool ch34x_ready;           /* version read and serial init done */
    uint8_t ch34x_version;
    bool ch34x_limited;
} shadow_t;

static shadow_t s_shadow[CONFIG_USBIP_MAX_DEVICES];
static SemaphoreHandle_t s_shadow_mutex;
static portMUX_TYPE s_init_lock = portMUX_INITIALIZER_UNLOCKED;

static void shadow_lock(void)
{
    if (s_shadow_mutex == NULL) {
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        taskENTER_CRITICAL(&s_init_lock);
        if (s_shadow_mutex == NULL) {
            s_shadow_mutex = m;
            m = NULL;
        }
        taskEXIT_CRITICAL(&s_init_lock);
        if (m != NULL) {
            vSemaphoreDelete(m);
        }
    }
    xSemaphoreTake(s_shadow_mutex, portMAX_DELAY);
}

static void shadow_unlock(void)
{
    xSemaphoreGive(s_shadow_mutex);
}

/* The shadow for busid at devnum (reset when the device re-enumerated);
   call with the lock held. */
static shadow_t *shadow_locked(const char *busid, uint32_t devnum, bool create)
{
    shadow_t *free_slot = NULL;
    for (int i = 0; i < CONFIG_USBIP_MAX_DEVICES; i++) {
        shadow_t *s = &s_shadow[i];
        if (s->used && strcmp(s->busid, busid) == 0) {
            if (s->devnum != devnum) {
                if (!create) {
                    return NULL;
                }
                memset(s, 0, sizeof(*s));
                s->used = true;
                strlcpy(s->busid, busid, sizeof(s->busid));
                s->devnum = devnum;
            }
            return s;
        }
        if (!s->used && free_slot == NULL) {
            free_slot = s;
        }
    }
    if (!create) {
        return NULL;
    }
    if (free_slot == NULL) {
        /* Recycle a shadow whose device is gone. */
        for (int i = 0; i < CONFIG_USBIP_MAX_DEVICES && free_slot == NULL; i++) {
            usbip_backend_device_t probe;
            char key[32] = {0};
            strlcpy(key, s_shadow[i].busid, sizeof(key));
            if (!usb_backend_get_device_by_busid(key, &probe) || probe.devnum != s_shadow[i].devnum) {
                free_slot = &s_shadow[i];
            }
        }
        if (free_slot == NULL) {
            free_slot = &s_shadow[0];
        }
    }
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->used = true;
    strlcpy(free_slot->busid, busid, sizeof(free_slot->busid));
    free_slot->devnum = devnum;
    return free_slot;
}

const char *serial_link_errno_name(int st)
{
    switch (st) {
    case 0: return "ok";
    case -ENODEV: return "ENODEV (device gone)";
    case -ETIMEDOUT: return "ETIMEDOUT";
    case -ECONNRESET: return "ECONNRESET (no answer in time)";
    case -EPIPE: return "EPIPE (stall)";
    case -EPROTO: return "EPROTO";
    case -EIO: return "EIO";
    case -ENOMEM: return "ENOMEM";
    case -EINVAL: return "EINVAL";
    case -EOPNOTSUPP: return "EOPNOTSUPP";
    case -EMSGSIZE: return "EMSGSIZE";
    default: return "error";
    }
}

static uint32_t claim_failures(const char *busid, bool *last_was_exhaustion)
{
    usb_backend_claims_device_t *devs = calloc(CONFIG_USBIP_MAX_DEVICES, sizeof(*devs));
    usb_backend_claims_totals_t totals;
    uint32_t f = 0;
    if (last_was_exhaustion) {
        *last_was_exhaustion = false;
    }
    if (devs != NULL) {
        const size_t n = usb_backend_get_claims(devs, CONFIG_USBIP_MAX_DEVICES, &totals);
        for (size_t i = 0; i < n; i++) {
            if (strncmp(devs[i].busid, busid, 31) == 0) {
                f = devs[i].claim_failures;
                if (last_was_exhaustion) {
                    *last_was_exhaustion = devs[i].last_claim_was_exhaustion;
                }
            }
        }
        free(devs);
    }
    return f;
}

void serial_link_explain(const serial_link_t *l, int st, const char *what, char *msg, size_t msg_len)
{
    bool exhausted = false;
    const uint32_t f = claim_failures(l->busid, &exhausted);
    if (st == -EIO && f > l->claim_failures_at_open && exhausted) {
        snprintf(msg, msg_len, "%s: interface claim failed: no free USB host channel (see /api/usb/claims)", what);
    } else {
        snprintf(msg, msg_len, "%s failed: %s (%d)", what, serial_link_errno_name(st), st);
    }
}

static int load(serial_link_t *l, char *msg, size_t msg_len)
{
    usbip_backend_device_t dev;
    if (!usb_backend_get_device_by_busid(l->busid, &dev)) {
        snprintf(msg, msg_len, "no device on %s", l->busid);
        return 404;
    }
    if (dev.busnum == VIRTUAL_DEVICE_BUSNUM) {
        snprintf(msg, msg_len, "virtual device: no serial function to drive");
        return 409;
    }
    l->devnum = dev.devnum;
    l->vid = dev.id_vendor;
    l->pid = dev.id_product;
    uint8_t *cfg = malloc(LINK_CFG_MAX);
    if (cfg == NULL) {
        snprintf(msg, msg_len, "out of memory");
        return 503;
    }
    const size_t n = usb_backend_get_config_descriptor(l->busid, cfg, LINK_CFG_MAX);
    rescue_parse_config(cfg, n, &l->cfg);
    free(cfg);
    rescue_classify(dev.id_vendor, dev.id_product, dev.bcd_device, &l->cfg, l->port, &l->caps);
    l->claim_failures_at_open = claim_failures(l->busid, NULL);
    return 0;
}

/* Runs on the backend task: only records the result; the reader applies it
   after taking in_sem. */
static void in_done(usb_backend_req_t *req, int status, size_t in_len)
{
    serial_link_t *l = (serial_link_t *)req->ctx;
    l->in_status = status;
    l->in_done_len = (status == 0) ? in_len : 0;
    xSemaphoreGive(l->in_sem);
}

int serial_link_open(serial_link_t *l, const char *busid, int port, char *msg, size_t msg_len)
{
    memset(l, 0, sizeof(*l));
    if (busid == NULL || busid[0] == '\0' || strlen(busid) > 31) {
        snprintf(msg, msg_len, "busid (port path, e.g. 1-1.3) required");
        return 400;
    }
    strlcpy(l->busid, busid, sizeof(l->busid));
    l->port = port < 0 ? 0 : port;
    const int st = load(l, msg, msg_len);
    if (st != 0) {
        return st;
    }
    if (l->caps.serial == RESCUE_SERIAL_NONE) {
        snprintf(msg, msg_len, "no serial function (CDC-ACM, CP210x, CH34x or FTDI) on this device/port");
        return 409;
    }
    l->in_sem = xSemaphoreCreateBinary();
    l->tx = heap_caps_malloc(SERIAL_LINK_TX_MAX, MALLOC_CAP_DEFAULT);
    if (l->in_sem == NULL || l->tx == NULL) {
        serial_link_close(l);
        snprintf(msg, msg_len, "out of memory");
        return 503;
    }
    return 0;
}

static void cancel_in(serial_link_t *l)
{
    if (l->in_pending) {
        usb_backend_cancel(&l->in_req);
        xSemaphoreTake(l->in_sem, portMAX_DELAY);   /* done() always follows a cancel */
        l->in_pending = false;
    }
}

int serial_link_reopen(serial_link_t *l, char *msg, size_t msg_len)
{
    cancel_in(l);
    l->rx_off = l->rx_len = 0;
    l->tx_len = 0;
    l->in_status = 0;
    const int st = load(l, msg, msg_len);
    if (st == 0 && l->caps.serial == RESCUE_SERIAL_NONE) {
        snprintf(msg, msg_len, "the device came back without a serial function");
        return 409;
    }
    return st;
}

void serial_link_close(serial_link_t *l)
{
    if (l->in_sem != NULL) {
        cancel_in(l);
        vSemaphoreDelete(l->in_sem);
        l->in_sem = NULL;
    }
    free(l->tx);
    l->tx = NULL;
    /* Free the data interface's host channels for USB/IP sessions. */
    if (l->busid[0]) {
        usb_backend_session_ended(l->busid);
    }
}

int serial_link_ctrl(serial_link_t *l, const rescue_ctrl_t *r, uint8_t *in, size_t in_cap, size_t *in_len)
{
    const bool is_in = (r->bm & 0x80) != 0;
    usb_setup_packet_t setup = {
        .bmRequestType = r->bm,
        .bRequest = r->req,
        .wValue = r->value,
        .wIndex = r->index,
        .wLength = is_in ? (uint16_t)in_cap : r->len,
    };
    size_t got = 0;
    const char *detail = NULL;
    const int st = usb_backend_transfer_sync(l->busid, 0, &setup,
                                             is_in ? NULL : r->data, is_in ? 0 : r->len,
                                             in, is_in ? in_cap : 0,
                                             LINK_CTRL_TIMEOUT_MS, &got, &detail);
    if (in_len != NULL) {
        *in_len = got;
    }
    if (l->log != NULL) {
        char line[96];
        snprintf(line, sizeof(line), "ctrl %02x %02x %04x %04x len=%u -> %d%s%s",
                 r->bm, r->req, r->value, r->index, (unsigned)setup.wLength, st,
                 detail ? " " : "", detail ? detail : "");
        cJSON_AddItemToArray(l->log, cJSON_CreateString(line));
    }
    return st;
}

int serial_link_prepare(serial_link_t *l)
{
    rescue_ctrl_t r;
    if (rescue_cp210x_enable_request(&l->caps, &r)) {
        return serial_link_ctrl(l, &r, NULL, 0, NULL);
    }
    if (l->caps.serial != RESCUE_SERIAL_CH34X) {
        return 0;
    }
    shadow_lock();
    shadow_t *sh = shadow_locked(l->busid, l->devnum, true);
    const bool ready = sh->ch34x_ready;
    shadow_unlock();
    if (ready) {
        return 0;
    }
    /* What the Linux driver does at probe: version, quirk probe, serial init. */
    uint8_t buf[2] = {0};
    size_t n = 0;
    r = (rescue_ctrl_t){ .bm = 0xC0, .req = RESCUE_CH341_REQ_READ_VERSION };
    int st = serial_link_ctrl(l, &r, buf, sizeof(buf), &n);
    if (st != 0) {
        return st;
    }
    const uint8_t version = buf[0];
    r = (rescue_ctrl_t){ .bm = 0xC0, .req = RESCUE_CH341_REQ_READ_REG, .value = RESCUE_CH341_REG_BREAK };
    const bool limited = serial_link_ctrl(l, &r, buf, sizeof(buf), &n) == -EPIPE;
    r = (rescue_ctrl_t){ .bm = 0x40, .req = RESCUE_CH341_REQ_SERIAL_INIT };
    st = serial_link_ctrl(l, &r, NULL, 0, NULL);
    if (st != 0) {
        return st;
    }
    shadow_lock();
    sh = shadow_locked(l->busid, l->devnum, true);
    sh->ch34x_ready = true;
    sh->ch34x_version = version;
    sh->ch34x_limited = limited;
    shadow_unlock();
    return 0;
}

int serial_link_set_lines(serial_link_t *l, bool dtr, bool rts)
{
    rescue_ctrl_t reqs[4];
    const int n = rescue_lines_requests(&l->caps, dtr, rts, reqs, 4);
    if (n <= 0) {
        return -EOPNOTSUPP;
    }
    for (int i = 0; i < n; i++) {
        const int st = serial_link_ctrl(l, &reqs[i], NULL, 0, NULL);
        if (st != 0) {
            return st;
        }
    }
    shadow_lock();
    shadow_t *sh = shadow_locked(l->busid, l->devnum, true);
    sh->lines_known = true;
    sh->dtr = dtr;
    sh->rts = rts;
    shadow_unlock();
    return 0;
}

void serial_link_get_lines(const serial_link_t *l, bool *dtr, bool *rts)
{
    shadow_lock();
    const shadow_t *sh = shadow_locked(l->busid, l->devnum, false);
    *dtr = sh != NULL && sh->lines_known && sh->dtr;
    *rts = sh != NULL && sh->lines_known && sh->rts;
    shadow_unlock();
}

int serial_link_set_baud(serial_link_t *l, uint32_t baud)
{
    shadow_lock();
    shadow_t *sh = shadow_locked(l->busid, l->devnum, true);
    const uint8_t version = sh->ch34x_version;
    const bool limited = sh->ch34x_limited;
    shadow_unlock();
    rescue_ctrl_t reqs[4];
    const int n = rescue_baud_requests(&l->caps, baud, version, limited, reqs, 4);
    if (n < 0) {
        return -EINVAL;
    }
    for (int i = 0; i < n; i++) {
        const int st = serial_link_ctrl(l, &reqs[i], NULL, 0, NULL);
        if (st != 0) {
            return st;
        }
    }
    shadow_lock();
    sh = shadow_locked(l->busid, l->devnum, true);
    sh->baud = baud;
    shadow_unlock();
    return 0;
}

int serial_link_flush(serial_link_t *l, uint32_t timeout_ms)
{
    if (l->tx_len == 0) {
        return 0;
    }
    const int st = usb_backend_transfer_sync(l->busid, l->caps.ep_out, NULL, l->tx, l->tx_len,
                                             NULL, 0, timeout_ms, NULL, NULL);
    l->tx_len = 0;
    return st;
}

int serial_link_write(serial_link_t *l, const uint8_t *data, size_t len)
{
    while (len > 0) {
        size_t n = SERIAL_LINK_TX_MAX - l->tx_len;
        if (n > len) {
            n = len;
        }
        memcpy(&l->tx[l->tx_len], data, n);
        l->tx_len += n;
        data += n;
        len -= n;
        if (l->tx_len == SERIAL_LINK_TX_MAX) {
            const int st = serial_link_flush(l, 3000);
            if (st != 0) {
                return st;
            }
        }
    }
    return 0;
}

static void post_in(serial_link_t *l)
{
    memset(&l->in_req, 0, sizeof(l->in_req));
    strlcpy(l->in_req.busid, l->busid, sizeof(l->in_req.busid));
    l->in_req.endpoint_addr = l->caps.ep_in;
    l->in_req.in_data = l->in_buf;
    l->in_req.in_capacity = SERIAL_LINK_IN_CHUNK;
    l->in_req.done = in_done;
    l->in_req.ctx = l;
    l->in_pending = true;
    usb_backend_submit(&l->in_req);
}

int serial_link_read(serial_link_t *l, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    int st = serial_link_flush(l, 3000);
    if (st != 0) {
        return st;
    }
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (true) {
        if (l->rx_len > l->rx_off) {
            size_t n = l->rx_len - l->rx_off;
            if (n > cap) {
                n = cap;
            }
            memcpy(buf, &l->in_buf[l->rx_off], n);
            l->rx_off += n;
            return (int)n;
        }
        if (!l->in_pending) {
            post_in(l);
        }
        const int64_t left_us = deadline - esp_timer_get_time();
        if (left_us <= 0) {
            return 0;
        }
        if (xSemaphoreTake(l->in_sem, pdMS_TO_TICKS((left_us + 999) / 1000) + 1) != pdTRUE) {
            return 0;               /* the IN stays posted for the next read */
        }
        l->in_pending = false;
        if (l->in_status != 0) {
            return l->in_status;    /* the next read posts again */
        }
        l->rx_off = 0;
        l->rx_len = l->in_done_len;
        if (l->caps.serial == RESCUE_SERIAL_FTDI) {
            l->rx_len = rescue_ftdi_strip_status(l->in_buf, l->rx_len, l->caps.mps_in ? l->caps.mps_in : 64);
        }
    }
}

void serial_link_discard_input(serial_link_t *l)
{
    l->rx_off = l->rx_len = 0;
}

cJSON *serial_link_last_set_json(const char *busid, uint32_t devnum)
{
    cJSON *o = cJSON_CreateObject();
    shadow_lock();
    const shadow_t *sh = shadow_locked(busid, devnum, false);
    if (sh != NULL && sh->baud) {
        cJSON_AddNumberToObject(o, "baud", sh->baud);
    } else {
        cJSON_AddNullToObject(o, "baud");
    }
    if (sh != NULL && sh->lines_known) {
        cJSON_AddBoolToObject(o, "dtr", sh->dtr);
        cJSON_AddBoolToObject(o, "rts", sh->rts);
    } else {
        cJSON_AddNullToObject(o, "dtr");
        cJSON_AddNullToObject(o, "rts");
    }
    shadow_unlock();
    cJSON_AddStringToObject(o, "note", "last set by this bridge since the device enumerated (null = never)");
    return o;
}
