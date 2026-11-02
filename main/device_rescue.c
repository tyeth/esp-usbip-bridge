#include "device_rescue.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "busid_owner.h"
#include "device_naming.h"
#include "hub_control.h"
#include "rescue_job.h"
#include "rescue_logic.h"
#include "serial_link.h"
#include "serial_monitor.h"
#include "usb_backend.h"
#include "virtual_device.h"

static const char *TAG = "rescue";

#define RESCUE_CFG_MAX          1024    /* configuration descriptor copy */
#define RESCUE_CTRL_TIMEOUT_MS  1000
#define RESCUE_BULK_TIMEOUT_MS  1000
#define RESCUE_CAPTURE_MAX_MS   10000
#define RESCUE_SESSION_MAX_MS   12000   /* pokes + settle + capture: the HTTP task is blocked meanwhile */
#define RESCUE_MAX_POKES        8
#define RESCUE_POKE_MAX_LEN     64
#define RESCUE_BYTES_DEFAULT    4096
#define RESCUE_BYTES_MAX        16384
#define RESCUE_WAIT_MAX_MS      10000
#define RESCUE_DROP_WAIT_MS     3000    /* default re-enumeration wait after a reset that drops the device */
#define RESCUE_DEFAULT_BAUD     115200

/* A device being worked on: identity and classification.  Serial actions
   use `link`; the RP2 requests go straight to EP0. */
typedef struct {
    char busid[32];
    usbip_backend_device_t dev;
    rescue_config_t cfg;
    rescue_caps_t caps;
    cJSON *log;                 /* requests sent, for the response */
    bool shared_with_monitor;   /* the monitor holds the device; do not release it */
} rctx_t;

/* ===========================================================================
 *  Helpers
 * ======================================================================== */

static int fail(cJSON **out, int status, const char *msg)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", false);
    cJSON_AddStringToObject(j, "error", msg);
    *out = j;
    return status;
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void add_hex(cJSON *o, const char *key, unsigned v, int digits)
{
    char buf[12];
    snprintf(buf, sizeof(buf), "%0*x", digits, v);
    cJSON_AddStringToObject(o, key, buf);
}

static const cJSON *arg(const cJSON *args, const char *key)
{
    return cJSON_IsObject(args) ? cJSON_GetObjectItemCaseSensitive(args, key) : NULL;
}

static bool arg_int(const cJSON *args, const char *key, int *out)
{
    const cJSON *v = arg(args, key);
    if (cJSON_IsNumber(v)) {
        *out = (int)v->valuedouble;
        return true;
    }
    return false;
}

static bool arg_double(const cJSON *args, const char *key, double *out)
{
    const cJSON *v = arg(args, key);
    if (cJSON_IsNumber(v)) {
        *out = v->valuedouble;
        return true;
    }
    return false;
}

/* -1 absent, 0 false, 1 true */
static int arg_tristate(const cJSON *args, const char *key)
{
    const cJSON *v = arg(args, key);
    if (cJSON_IsBool(v)) {
        return cJSON_IsTrue(v) ? 1 : 0;
    }
    if (cJSON_IsNumber(v)) {
        return v->valuedouble != 0 ? 1 : 0;
    }
    return -1;
}

/* Look the device up and classify it.  Returns 0, or an HTTP status with *out set. */
static int load(const char *busid, int port, rctx_t *c, cJSON **out)
{
    memset(c, 0, sizeof(*c));
    if (busid == NULL || busid[0] == '\0' || strlen(busid) > 31) {
        return fail(out, 400, "busid (port path, e.g. 1-1.3) required");
    }
    strlcpy(c->busid, busid, sizeof(c->busid));
    if (!usb_backend_get_device_by_busid(c->busid, &c->dev)) {
        return fail(out, 404, "no device with that busid (hubs are not devices)");
    }
    if (c->dev.busnum == VIRTUAL_DEVICE_BUSNUM) {
        return fail(out, 409, "virtual device: nothing to rescue");
    }
    uint8_t *cfg = malloc(RESCUE_CFG_MAX);
    if (cfg == NULL) {
        return fail(out, 503, "out of memory");
    }
    const size_t n = usb_backend_get_config_descriptor(c->busid, cfg, RESCUE_CFG_MAX);
    rescue_parse_config(cfg, n, &c->cfg);
    free(cfg);
    rescue_classify(c->dev.id_vendor, c->dev.id_product, c->dev.bcd_device, &c->cfg, port, &c->caps);
    return 0;
}

/* Take the device for a rescue action: 409 while a USB/IP client or a job
   has it.  A running serial monitor is shared for EP0-only actions (lines,
   resets: watch the boot log) and stopped for the others. */
static int take(rctx_t *c, const char *what, bool share_with_monitor, cJSON **out)
{
    if (rescue_job_busy(c->busid)) {
        return fail(out, 409, "a rescue job is running on this device");
    }
    if (share_with_monitor && serial_monitor_active(c->busid)) {
        c->shared_with_monitor = true;
        return 0;
    }
    serial_monitor_stop(c->busid);
    busid_owner_info_t owner;
    if (busid_owner_try_begin(c->busid, BUSID_OWNER_RESCUE, what, &owner)) {
        return 0;
    }
    char msg[128];
    if (owner.kind == BUSID_OWNER_USBIP) {
        snprintf(msg, sizeof(msg), "device is imported over USB/IP by %s; detach it first",
                 owner.peer[0] ? owner.peer : "a client");
    } else {
        snprintf(msg, sizeof(msg), "device is busy (%s)", owner.peer[0] ? owner.peer : "rescue action");
    }
    return fail(out, 409, msg);
}

static void give(const rctx_t *c)
{
    if (!c->shared_with_monitor) {
        busid_owner_end(c->busid, BUSID_OWNER_RESCUE);
    }
}

/* One control request to EP0 (RP2 reset interface, PICOBOOT). */
static int do_ctrl(rctx_t *c, const rescue_ctrl_t *r)
{
    usb_setup_packet_t setup = {
        .bmRequestType = r->bm,
        .bRequest = r->req,
        .wValue = r->value,
        .wIndex = r->index,
        .wLength = r->len,
    };
    const char *detail = NULL;
    const int st = usb_backend_transfer_sync(c->busid, 0, &setup, r->data, r->len, NULL, 0,
                                             RESCUE_CTRL_TIMEOUT_MS, NULL, &detail);
    if (c->log != NULL) {
        char line[96];
        snprintf(line, sizeof(line), "ctrl %02x %02x %04x %04x len=%u -> %d%s%s",
                 r->bm, r->req, r->value, r->index, (unsigned)r->len, st,
                 detail ? " " : "", detail ? detail : "");
        cJSON_AddItemToArray(c->log, cJSON_CreateString(line));
    }
    return st;
}

/* Open the device's serial function on top of a loaded context. */
static int open_link(rctx_t *c, int port, serial_link_t **out_link, cJSON **out)
{
    serial_link_t *l = calloc(1, sizeof(*l));
    if (l == NULL) {
        return fail(out, 503, "out of memory");
    }
    char msg[128];
    const int st = serial_link_open(l, c->busid, port, msg, sizeof(msg));
    if (st != 0) {
        free(l);
        return fail(out, st, msg);
    }
    l->log = c->log;
    *out_link = l;
    return 0;
}

static void close_link(serial_link_t *l)
{
    if (l != NULL) {
        serial_link_close(l);
        free(l);
    }
}

/* ===========================================================================
 *  Shared with the jobs (esp_flasher.c etc.)
 * ======================================================================== */

int device_rescue_run_line_seq(serial_link_t *l, rescue_mode_t mode, bool *dropped)
{
    const rescue_line_seq_t *seq = rescue_mode_line_seq(mode);
    if (seq == NULL) {
        return -EINVAL;
    }
    *dropped = false;
    for (int i = 0; i < seq->num_steps; i++) {
        const rescue_line_step_t *s = &seq->steps[i];
        const int st = serial_link_set_lines(l, s->dtr, s->rts);
        if (st != 0) {
            /* From the commit step on, failing is the device leaving the bus. */
            if (seq->commit_step >= 0 && i >= seq->commit_step) {
                *dropped = true;
                return 0;
            }
            return st;
        }
        if (s->delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(s->delay_ms));
        }
    }
    return 0;
}

bool device_rescue_wait_reenumeration(const char *busid, uint32_t devnum, int wait_ms,
                                      usbip_backend_device_t *now, bool *dropped)
{
    const int64_t t0 = now_ms();
    char key[32] = {0};
    strlcpy(key, busid, sizeof(key));
    *dropped = false;
    while (now_ms() - t0 < wait_ms) {
        const bool present = usb_backend_get_device_by_busid(key, now);
        if (!present || now->devnum != devnum) {
            *dropped = true;
        }
        if (present && now->devnum != devnum) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

/* ===========================================================================
 *  GET .../rescue
 * ======================================================================== */

static const char *class_name(uint8_t cls)
{
    switch (cls) {
    case 0x01: return "audio";
    case 0x02: return "cdc";
    case 0x03: return "hid";
    case 0x07: return "printer";
    case 0x08: return "mass-storage";
    case 0x0A: return "cdc-data";
    case 0x0E: return "video";
    case 0xDC: return "diagnostic";
    case 0xE0: return "wireless";
    case 0xEF: return "misc";
    case 0xFE: return "application";
    case 0xFF: return "vendor";
    default: return "other";
    }
}

static void add_port_power(cJSON *j, const char *busid)
{
    hub_ctl_hub_status_t *snap = malloc(USB_BACKEND_MAX_HUBS * sizeof(*snap));
    const size_t n = snap ? hub_ctl_snapshot(snap, USB_BACKEND_MAX_HUBS) : 0;
    const hub_ctl_port_t *port = NULL;
    for (size_t h = 0; h < n && port == NULL; h++) {
        for (uint8_t p = 0; p < snap[h].num_ports; p++) {
            if (strcmp(snap[h].ports[p].path, busid) == 0) {
                port = &snap[h].ports[p];
                break;
            }
        }
    }
    cJSON *o = cJSON_AddObjectToObject(j, "port");
    if (port != NULL) {
        cJSON_AddStringToObject(o, "path", port->path);
        cJSON_AddStringToObject(o, "power", port->powered ? "on" : "off");
        cJSON_AddBoolToObject(o, "connected", port->connected);
        cJSON_AddBoolToObject(o, "over_current", port->over_current);
        cJSON_AddStringToObject(o, "saved", port->desired_on ? "on" : "off");
    } else {
        cJSON_AddStringToObject(o, "path", busid);
        cJSON_AddNullToObject(o, "power");
        cJSON_AddStringToObject(o, "note", "root port or hub not switchable: no per-port power state");
    }
    cJSON_AddBoolToObject(o, "read_only", true);
    free(snap);
}

static bool has_msc(const rescue_config_t *cfg)
{
    for (int i = 0; i < cfg->num_ifaces; i++) {
        if (cfg->ifaces[i].cls == 0x08 && cfg->ifaces[i].subclass == 0x06 && cfg->ifaces[i].protocol == 0x50) {
            return true;
        }
    }
    return false;
}

int device_rescue_info(const char *busid, cJSON **out)
{
    rctx_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return fail(out, 503, "out of memory");
    }
    int st = load(busid, 0, c, out);
    if (st != 0) {
        free(c);
        return st;
    }
    const usbip_backend_device_t *d = &c->dev;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddStringToObject(j, "busid", d->busid);
    add_hex(j, "vid", d->id_vendor, 4);
    add_hex(j, "pid", d->id_product, 4);
    add_hex(j, "bcd_device", d->bcd_device, 4);
    add_hex(j, "device_class", d->device_class, 2);
    cJSON_AddStringToObject(j, "manufacturer", d->manufacturer);
    cJSON_AddStringToObject(j, "product", d->product);
    /* The iSerial string.  "serial" is the serial-function object below: the
       string used to be a second "serial" key, which JSON parsers drop. */
    if (d->serial[0]) {
        cJSON_AddStringToObject(j, "serial_number", d->serial);
    } else {
        cJSON_AddNullToObject(j, "serial_number");
    }
    char name[DEVICE_NAME_MAX_LEN] = "";
    device_naming_get_device_name(d->busid, name, sizeof(name));
    cJSON_AddStringToObject(j, "name", name);
    cJSON_AddStringToObject(j, "speed", d->speed == 1 ? "low" : d->speed == 2 ? "full" : d->speed == 3 ? "high" : "unknown");
    cJSON_AddNumberToObject(j, "address", d->dev_addr);
    cJSON_AddNumberToObject(j, "configuration", d->configuration_value);

    cJSON *ifs = cJSON_AddArrayToObject(j, "interfaces");
    for (int i = 0; i < c->cfg.num_ifaces; i++) {
        const rescue_iface_t *it = &c->cfg.ifaces[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "number", it->number);
        add_hex(o, "class", it->cls, 2);
        add_hex(o, "subclass", it->subclass, 2);
        add_hex(o, "protocol", it->protocol, 2);
        cJSON_AddStringToObject(o, "class_name", class_name(it->cls));
        cJSON_AddNumberToObject(o, "num_endpoints", it->num_endpoints);
        if (it->ep_in) {
            add_hex(o, "bulk_in", it->ep_in, 2);
        }
        if (it->ep_out) {
            add_hex(o, "bulk_out", it->ep_out, 2);
        }
        if (it->ep_intr_in) {
            add_hex(o, "intr_in", it->ep_intr_in, 2);
        }
        cJSON_AddItemToArray(ifs, o);
    }

    busid_owner_info_t owner;
    if (busid_owner_get(d->busid, &owner)) {
        cJSON *o = cJSON_AddObjectToObject(j, "owner");
        cJSON_AddStringToObject(o, "kind", owner.kind == BUSID_OWNER_USBIP ? "usbip" : "rescue");
        cJSON_AddStringToObject(o, "peer", owner.peer);
        cJSON_AddNumberToObject(o, "sessions", owner.sessions);
        cJSON_AddNumberToObject(o, "for_s", (double)((esp_timer_get_time() - owner.since_us) / 1000000));
        cJSON_AddBoolToObject(j, "imported", owner.kind == BUSID_OWNER_USBIP);
    } else {
        cJSON_AddNullToObject(j, "owner");
        cJSON_AddBoolToObject(j, "imported", false);
    }
    cJSON_AddBoolToObject(j, "monitor", serial_monitor_active(d->busid));
    cJSON_AddBoolToObject(j, "job_running", rescue_job_busy(d->busid));
    add_port_power(j, d->busid);

    const rescue_caps_t *k = &c->caps;
    cJSON *s = cJSON_AddObjectToObject(j, "serial");
    cJSON_AddStringToObject(s, "kind", rescue_serial_kind_name(k->serial));
    cJSON_AddNumberToObject(s, "ports", k->serial_ports);
    if (k->serial != RESCUE_SERIAL_NONE) {
        cJSON_AddNumberToObject(s, "control_interface", k->serial_ctrl_if);
        cJSON_AddNumberToObject(s, "data_interface", k->serial_data_if);
        add_hex(s, "bulk_in", k->ep_in, 2);
        add_hex(s, "bulk_out", k->ep_out, 2);
        cJSON_AddBoolToObject(s, "esp_usb_jtag", k->esp_usb_jtag);
        if (k->serial == RESCUE_SERIAL_FTDI) {
            cJSON_AddNumberToObject(s, "ftdi_channel", k->ftdi_channel);
            cJSON_AddBoolToObject(s, "ftdi_hispeed", k->ftdi_hispeed);
            cJSON_AddBoolToObject(s, "baud_supported", k->ftdi_baud_supported);
        }
        cJSON_AddItemToObject(s, "last_set", serial_link_last_set_json(d->busid, d->devnum));
    }
    cJSON *rp = cJSON_AddObjectToObject(j, "rp");
    cJSON_AddNumberToObject(rp, "reset_interface", k->rp_reset_if);
    cJSON_AddNumberToObject(rp, "picoboot_interface", k->picoboot_if);
    cJSON_AddStringToObject(rp, "chip", k->rp_chip == RESCUE_CHIP_RP2040 ? "rp2040"
                                       : k->rp_chip == RESCUE_CHIP_RP2350 ? "rp2350" : "unknown");

    cJSON *modes = cJSON_AddArrayToObject(j, "modes");
    for (int m = 0; m < RESCUE_MODE_COUNT; m++) {
        if (!rescue_mode_applicable(k, (rescue_mode_t)m)) {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", rescue_mode_name((rescue_mode_t)m));
        cJSON_AddStringToObject(o, "description", rescue_mode_description((rescue_mode_t)m));
        cJSON_AddItemToArray(modes, o);
    }
    const rescue_mode_t autom = rescue_auto_bootloader(k, d->id_vendor);
    if (autom != RESCUE_MODE_INVALID) {
        cJSON_AddStringToObject(j, "bootloader_mode", rescue_mode_name(autom));
    } else {
        cJSON_AddNullToObject(j, "bootloader_mode");
    }
    cJSON_AddBoolToObject(j, "console", k->serial != RESCUE_SERIAL_NONE);
    /* What can program this device from here. */
    cJSON *fl = cJSON_AddArrayToObject(j, "flashers");
    if (k->serial != RESCUE_SERIAL_NONE) {
        cJSON_AddItemToArray(fl, cJSON_CreateString("esp"));
        cJSON_AddItemToArray(fl, cJSON_CreateString("samba"));
    }
    if (k->picoboot_if >= 0) {
        cJSON_AddItemToArray(fl, cJSON_CreateString("picoboot"));
    }
    const bool msc = has_msc(&c->cfg);
    if (msc) {
        cJSON_AddItemToArray(fl, cJSON_CreateString("uf2"));
    }
    cJSON_AddBoolToObject(j, "mass_storage", msc);
    cJSON *lim = cJSON_AddObjectToObject(j, "limits");
    cJSON_AddNumberToObject(lim, "capture_s_max", RESCUE_CAPTURE_MAX_MS / 1000);
    cJSON_AddNumberToObject(lim, "session_s_max", RESCUE_SESSION_MAX_MS / 1000);
    cJSON_AddNumberToObject(lim, "max_bytes", RESCUE_BYTES_MAX);
    free(c);
    *out = j;
    return 200;
}

/* ===========================================================================
 *  POST .../serial
 * ======================================================================== */

int device_rescue_serial(const char *busid, const cJSON *args, cJSON **out)
{
    int port = 0;
    arg_int(args, "port", &port);
    int baud = 0;
    const bool has_baud = arg_int(args, "baud", &baud);
    const int dtr = arg_tristate(args, "dtr");
    const int rts = arg_tristate(args, "rts");
    if (!has_baud && dtr < 0 && rts < 0) {
        return fail(out, 400, "give baud and/or dtr / rts");
    }
    if (has_baud && (baud < 50 || baud > 12000000)) {
        return fail(out, 400, "baud 50-12000000");
    }
    if (has_baud && baud == 1200 && !cJSON_IsTrue(arg(args, "allow_1200"))) {
        return fail(out, 400, "1200 baud is the bootloader touch on many boards: use reset mode touch_1200, "
                              "or pass allow_1200: true");
    }
    rctx_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return fail(out, 503, "out of memory");
    }
    int st = load(busid, port, c, out);
    if (st == 0) {
        st = take(c, "rescue:serial", true, out);
    }
    if (st != 0) {
        free(c);
        return st;
    }
    c->log = cJSON_CreateArray();
    serial_link_t *l = NULL;
    st = open_link(c, port, &l, out);
    if (st != 0) {
        give(c);
        cJSON_Delete(c->log);
        free(c);
        return st;
    }
    const char *step = "prepare";
    st = serial_link_prepare(l);
    if (st == 0 && has_baud) {
        step = "baud";
        st = serial_link_set_baud(l, (uint32_t)baud);
    }
    if (st == 0 && (dtr >= 0 || rts >= 0)) {
        step = "lines";
        /* Lines not given keep what the bridge last set (deasserted if never). */
        bool d, r;
        serial_link_get_lines(l, &d, &r);
        st = serial_link_set_lines(l, dtr >= 0 ? dtr == 1 : d, rts >= 0 ? rts == 1 : r);
    }
    /* Only control transfers here: nothing claimed to release while a monitor reads. */
    if (c->shared_with_monitor) {
        vSemaphoreDelete(l->in_sem);
        l->in_sem = NULL;
        free(l->tx);
        free(l);
    } else {
        close_link(l);
    }
    give(c);

    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", st == 0);
    if (st != 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s request failed: %s (%d)", step, serial_link_errno_name(st), st);
        cJSON_AddStringToObject(j, "error", msg);
    }
    cJSON_AddStringToObject(j, "busid", c->busid);
    cJSON_AddStringToObject(j, "serial_kind", rescue_serial_kind_name(c->caps.serial));
    cJSON_AddNumberToObject(j, "port", port);
    cJSON_AddItemToObject(j, "lines", serial_link_last_set_json(c->busid, c->dev.devnum));
    cJSON_AddItemToObject(j, "requests", c->log);
    free(c);
    *out = j;
    return st == 0 ? 200 : (st == -EINVAL ? 400 : 502);
}

/* ===========================================================================
 *  POST .../reset
 * ======================================================================== */

static int run_touch_1200(serial_link_t *l, bool *dropped)
{
    int st = serial_link_set_lines(l, true, true);
    if (st != 0) {
        return st;
    }
    if (serial_link_set_baud(l, 1200) != 0) {
        *dropped = true;            /* pico-sdk resets on the line coding itself */
        return 0;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    if (serial_link_set_lines(l, false, false) != 0) {
        *dropped = true;
    }
    return 0;
}

static int run_picoboot(rctx_t *c, bool to_bootsel, bool *dropped, char *msg, size_t msg_len)
{
    rescue_ctrl_t r;
    rescue_picoboot_if_reset_request(c->caps.picoboot_if, &r);
    int st = do_ctrl(c, &r);
    if (st != 0) {
        snprintf(msg, msg_len, "PICOBOOT interface reset failed: %s (%d)", serial_link_errno_name(st), st);
        return st;
    }
    uint8_t cmd[32];
    rescue_picoboot_reboot_cmd(c->caps.rp_chip, to_bootsel, (uint32_t)now_ms(), 500, cmd);
    st = usb_backend_transfer_sync(c->busid, c->caps.picoboot_ep_out, NULL, cmd, sizeof(cmd),
                                   NULL, 0, RESCUE_BULK_TIMEOUT_MS, NULL, NULL);
    char line[64];
    snprintf(line, sizeof(line), "bulk out picoboot reboot cmd -> %d", st);
    cJSON_AddItemToArray(c->log, cJSON_CreateString(line));
    if (st != 0) {
        snprintf(msg, msg_len, "PICOBOOT command failed: %s (%d)", serial_link_errno_name(st), st);
        usb_backend_session_ended(c->busid);
        return st;
    }
    /* The ACK: a zero-length IN for a command without a data phase. */
    uint8_t ack[64];
    size_t n = 0;
    st = usb_backend_transfer_sync(c->busid, c->caps.picoboot_ep_in, NULL, NULL, 0,
                                   ack, sizeof(ack), RESCUE_BULK_TIMEOUT_MS, &n, NULL);
    snprintf(line, sizeof(line), "bulk in picoboot ack -> %d", st);
    cJSON_AddItemToArray(c->log, cJSON_CreateString(line));
    *dropped = true;                /* reboots after its 500 ms delay */
    usb_backend_session_ended(c->busid);
    return 0;
}

int device_rescue_reset(const char *busid, const cJSON *args, cJSON **out)
{
    const cJSON *mv = arg(args, "mode");
    if (!cJSON_IsString(mv)) {
        return fail(out, 400, "mode (string) required: see GET /api/usb/devices/{busid}/rescue");
    }
    rescue_mode_t mode = rescue_mode_from_name(mv->valuestring);
    if (mode == RESCUE_MODE_INVALID) {
        return fail(out, 400, "unknown mode");
    }
    int port = 0;
    arg_int(args, "port", &port);
    rctx_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return fail(out, 503, "out of memory");
    }
    int st = load(busid, port, c, out);
    if (st != 0) {
        free(c);
        return st;
    }
    const rescue_mode_t requested = mode;
    if (mode == RESCUE_MODE_AUTO_BOOTLOADER) {
        mode = rescue_auto_bootloader(&c->caps, c->dev.id_vendor);
        if (mode == RESCUE_MODE_INVALID) {
            const bool in_bootsel = c->caps.picoboot_if >= 0;
            free(c);
            return fail(out, 409, in_bootsel ? "already in BOOTSEL (PICOBOOT)"
                                             : "no bootloader entry known for this device");
        }
    }
    /* Any DTR/RTS sequence may be sent to any serial function (the user may
       know the wiring better); the others need their interface. */
    const rescue_line_seq_t *seq = rescue_mode_line_seq(mode);
    const bool needs_serial = seq != NULL || mode == RESCUE_MODE_TOUCH_1200;
    const bool ok_here = seq ? c->caps.serial != RESCUE_SERIAL_NONE : rescue_mode_applicable(&c->caps, mode);
    if (!ok_here) {
        char msg[96];
        snprintf(msg, sizeof(msg), "mode %s does not apply to this device", rescue_mode_name(mode));
        free(c);
        return fail(out, 409, msg);
    }
    /* Line sequences share the device with a monitor (the boot log is the point);
       PICOBOOT claims an interface, so it does not. */
    st = take(c, "rescue:reset", needs_serial, out);
    if (st != 0) {
        free(c);
        return st;
    }
    c->log = cJSON_CreateArray();
    const uint32_t devnum = c->dev.devnum;
    bool dropped = false;
    char msg[128] = "";
    ESP_LOGI(TAG, "%s: reset mode %s", c->busid, rescue_mode_name(mode));

    if (needs_serial) {
        serial_link_t *l = NULL;
        st = open_link(c, port, &l, out);
        if (st != 0) {
            give(c);
            cJSON_Delete(c->log);
            free(c);
            return st;
        }
        st = serial_link_prepare(l);
        if (st == 0) {
            st = (seq != NULL) ? device_rescue_run_line_seq(l, mode, &dropped) : run_touch_1200(l, &dropped);
        }
        if (c->shared_with_monitor) {
            vSemaphoreDelete(l->in_sem);
            free(l->tx);
            free(l);
        } else {
            close_link(l);
        }
    } else if (mode == RESCUE_MODE_RP_BOOTSEL || mode == RESCUE_MODE_RP_REBOOT) {
        rescue_ctrl_t r;
        rescue_rp_reset_request(c->caps.rp_reset_if, mode == RESCUE_MODE_RP_BOOTSEL, &r);
        /* The device reboots from the SETUP stage: no status stage is normal. */
        (void)do_ctrl(c, &r);
        dropped = true;
        st = 0;
    } else {
        st = run_picoboot(c, mode == RESCUE_MODE_PICOBOOT_BOOTSEL, &dropped, msg, sizeof(msg));
    }
    give(c);

    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", st == 0);
    if (st != 0) {
        if (msg[0] == '\0') {
            snprintf(msg, sizeof(msg), "request failed: %s (%d)", serial_link_errno_name(st), st);
        }
        cJSON_AddStringToObject(j, "error", msg);
    }
    cJSON_AddStringToObject(j, "busid", c->busid);
    cJSON_AddStringToObject(j, "mode", rescue_mode_name(requested));
    cJSON_AddStringToObject(j, "ran", rescue_mode_name(mode));
    cJSON_AddItemToObject(j, "requests", c->log);
    if (st == 0) {
        int wait_ms = dropped ? RESCUE_DROP_WAIT_MS : 0;
        arg_int(args, "wait_ms", &wait_ms);
        if (wait_ms < 0) {
            wait_ms = 0;
        } else if (wait_ms > RESCUE_WAIT_MAX_MS) {
            wait_ms = RESCUE_WAIT_MAX_MS;
        }
        const int64_t t0 = now_ms();
        usbip_backend_device_t d;
        bool gone = dropped;
        if (wait_ms > 0 && device_rescue_wait_reenumeration(c->busid, devnum, wait_ms, &d, &gone)) {
            cJSON *o = cJSON_AddObjectToObject(j, "reenumerated");
            add_hex(o, "vid", d.id_vendor, 4);
            add_hex(o, "pid", d.id_product, 4);
            cJSON_AddStringToObject(o, "product", d.product);
            cJSON_AddNumberToObject(o, "address", d.dev_addr);
        } else {
            cJSON_AddNullToObject(j, "reenumerated");
        }
        cJSON_AddBoolToObject(j, "dropped", gone || dropped);
        cJSON_AddNumberToObject(j, "waited_ms", (double)(now_ms() - t0));
    }
    free(c);
    *out = j;
    return st == 0 ? 200 : 502;
}

/* ===========================================================================
 *  POST .../console
 * ======================================================================== */

typedef struct {
    char label[24];
    uint8_t data[RESCUE_POKE_MAX_LEN];
    int len;
} poke_t;

static int parse_pokes(const cJSON *args, poke_t *pokes, int *n, cJSON **out)
{
    *n = 0;
    const cJSON *arr = arg(args, "pokes");
    if (arr == NULL || cJSON_IsNull(arr)) {
        return 0;
    }
    if (!cJSON_IsArray(arr)) {
        return fail(out, 400, "pokes must be a list of \"enter\" / \"ctrl_c\" / \"ctrl_d\" / {\"hex\": \"...\"}");
    }
    const cJSON *p;
    cJSON_ArrayForEach(p, arr) {
        if (*n >= RESCUE_MAX_POKES) {
            return fail(out, 400, "at most 8 pokes");
        }
        poke_t *k = &pokes[*n];
        if (cJSON_IsString(p)) {
            k->len = rescue_named_poke(p->valuestring, k->data, sizeof(k->data));
            strlcpy(k->label, p->valuestring, sizeof(k->label));
        } else if (cJSON_IsObject(p) && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(p, "hex"))) {
            k->len = rescue_hex_decode(cJSON_GetObjectItemCaseSensitive(p, "hex")->valuestring,
                                       k->data, sizeof(k->data));
            snprintf(k->label, sizeof(k->label), "hex:%d bytes", k->len);
        } else {
            k->len = -1;
        }
        if (k->len <= 0) {
            return fail(out, 400, "bad poke: use \"enter\", \"ctrl_c\", \"ctrl_d\" or {\"hex\": \"0d0a\"} (max 64 bytes)");
        }
        (*n)++;
    }
    return 0;
}

int device_rescue_console(const char *busid, const cJSON *args, cJSON **out)
{
    int port = 0;
    arg_int(args, "port", &port);
    int baud = RESCUE_DEFAULT_BAUD;
    if (!arg_int(args, "serial_baud", &baud)) {
        arg_int(args, "baud", &baud);
    }
    if (baud == 1200) {
        return fail(out, 400, "1200 baud is the bootloader touch on many boards: not used for the console");
    }
    if (baud < 50 || baud > 12000000) {
        return fail(out, 400, "serial_baud 50-12000000");
    }
    double settle_s = 1.0, capture_s = 3.0;
    arg_double(args, "settle_s", &settle_s);
    arg_double(args, "capture_s", &capture_s);
    const int settle_ms = (int)(settle_s * 1000);
    const int capture_ms = (int)(capture_s * 1000);
    if (settle_ms < 0 || capture_ms < 0 || capture_ms > RESCUE_CAPTURE_MAX_MS) {
        return fail(out, 400, "capture_s 0-10, settle_s >= 0");
    }
    int max_bytes = RESCUE_BYTES_DEFAULT;
    arg_int(args, "max_bytes", &max_bytes);
    if (max_bytes < 1 || max_bytes > RESCUE_BYTES_MAX) {
        return fail(out, 400, "max_bytes 1-16384");
    }
    poke_t *pokes = calloc(RESCUE_MAX_POKES, sizeof(*pokes));
    if (pokes == NULL) {
        return fail(out, 503, "out of memory");
    }
    int npokes = 0;
    int st = parse_pokes(args, pokes, &npokes, out);
    if (st != 0) {
        free(pokes);
        return st;
    }
    const int poke_lead_ms = 100;   /* the first read is posted before the first poke */
    const int span_ms = poke_lead_ms + (npokes > 0 ? (npokes - 1) * settle_ms : 0) + capture_ms;
    if (span_ms > RESCUE_SESSION_MAX_MS) {
        free(pokes);
        return fail(out, 400, "pokes x settle_s + capture_s must stay under 12 s (the bridge API is blocked meanwhile)");
    }
    const int dtr = arg_tristate(args, "dtr");
    const int rts = arg_tristate(args, "rts");

    rctx_t *c = calloc(1, sizeof(*c));
    uint8_t *data = malloc((size_t)max_bytes);
    serial_link_t *l = NULL;
    if (c == NULL || data == NULL) {
        st = fail(out, 503, "out of memory");
        goto free_all;
    }
    st = load(busid, port, c, out);
    if (st == 0) {
        st = take(c, "rescue:console", false, out);     /* stops a monitor */
    }
    if (st != 0) {
        goto free_all;
    }
    c->log = cJSON_CreateArray();
    st = open_link(c, port, &l, out);
    if (st != 0) {
        give(c);
        cJSON_Delete(c->log);
        goto free_all;
    }

    char err[128] = "";
    /* A toggle that failed to sync is not fatal: the data may still get through. */
    serial_link_sync_toggles(l);
    st = serial_link_prepare(l);
    if (st == 0) {
        st = serial_link_set_baud(l, (uint32_t)baud);
    }
    /* Lines are only touched when asked: on 303a:1001 some transitions reset the chip. */
    if (st == 0 && (dtr >= 0 || rts >= 0)) {
        bool d, r;
        serial_link_get_lines(l, &d, &r);
        st = serial_link_set_lines(l, dtr >= 0 ? dtr == 1 : d, rts >= 0 ? rts == 1 : r);
    }
    if (st != 0) {
        snprintf(err, sizeof(err), "serial setup failed: %s (%d)", serial_link_errno_name(st), st);
    }

    size_t total = 0;
    bool truncated = false;
    int read_status = 0, write_errors = 0;
    const int64_t t0 = now_ms();
    if (st == 0) {
        int64_t next_poke = t0 + poke_lead_ms;
        int poke_i = 0;
        const int64_t end = t0 + span_ms;
        bool reading = true;
        while (true) {
            const int64_t now = now_ms();
            if (poke_i < npokes && now >= next_poke) {
                int ws = serial_link_write(l, pokes[poke_i].data, (size_t)pokes[poke_i].len);
                if (ws == 0) {
                    ws = serial_link_flush(l, RESCUE_BULK_TIMEOUT_MS);
                }
                char line[64];
                snprintf(line, sizeof(line), "poke %s -> %d", pokes[poke_i].label, ws);
                cJSON_AddItemToArray(c->log, cJSON_CreateString(line));
                if (ws != 0) {
                    write_errors++;
                }
                poke_i++;
                next_poke += settle_ms;
                continue;
            }
            if (now >= end) {
                break;
            }
            const int64_t deadline = (poke_i < npokes && next_poke < end) ? next_poke : end;
            if (!reading) {
                vTaskDelay(pdMS_TO_TICKS(deadline - now < 50 ? deadline - now + 1 : 50));
                continue;
            }
            const int n = serial_link_read(l, &data[total], (size_t)max_bytes - total, (uint32_t)(deadline - now));
            if (n < 0) {
                read_status = n;
                reading = false;
                serial_link_explain(l, n, "serial read", err, sizeof(err));
            } else {
                total += (size_t)n;
                if (total >= (size_t)max_bytes) {
                    truncated = true;
                    reading = false;
                }
            }
        }
    }
    close_link(l);
    l = NULL;
    give(c);

    cJSON *j = cJSON_CreateObject();
    const bool ok = (st == 0);
    cJSON_AddBoolToObject(j, "ok", ok);
    if (err[0]) {
        cJSON_AddStringToObject(j, ok ? "warning" : "error", err);
    }
    cJSON_AddStringToObject(j, "busid", c->busid);
    cJSON_AddStringToObject(j, "serial_kind", rescue_serial_kind_name(c->caps.serial));
    cJSON_AddNumberToObject(j, "serial_baud", baud);
    cJSON_AddNumberToObject(j, "elapsed_ms", (double)(now_ms() - t0));
    cJSON_AddNumberToObject(j, "bytes", (double)total);
    cJSON_AddBoolToObject(j, "truncated", truncated);
    cJSON_AddNumberToObject(j, "read_status", read_status);
    cJSON_AddNumberToObject(j, "write_errors", write_errors);
    char *text = malloc(total * 4 + 1);
    char *hex = malloc(total * 2 + 1);
    if (text != NULL && hex != NULL) {
        rescue_sanitize_text(data, total, text, total * 4 + 1);
        for (size_t i = 0; i < total; i++) {
            snprintf(&hex[i * 2], 3, "%02x", data[i]);
        }
        hex[total * 2] = '\0';
        cJSON_AddStringToObject(j, "text", text);
        cJSON_AddStringToObject(j, "data_hex", hex);
        cJSON_AddStringToObject(j, "classification", rescue_classify_transcript(text));
    }
    free(text);
    free(hex);
    cJSON_AddItemToObject(j, "lines", serial_link_last_set_json(c->busid, c->dev.devnum));
    cJSON_AddItemToObject(j, "requests", c->log);
    *out = j;
    st = ok ? 200 : 502;

free_all:
    close_link(l);
    free(data);
    free(c);
    free(pokes);
    return st;
}
