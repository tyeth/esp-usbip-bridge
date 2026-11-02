#include "controller_api.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "analog_mux.h"
#include "bridge_identity.h"
#include "busid_owner.h"
#include "device_naming.h"
#include "blob_store.h"
#include "capture.h"
#include "device_rescue.h"
#include "dut_flash.h"
#include "dut_fs.h"
#include "esp_flasher.h"
#include "msc_host.h"
#include "ota_update.h"
#include "picoboot_flasher.h"
#include "rescue_job.h"
#include "samba_flasher.h"
#include "serial_monitor.h"
#include "discovery_service.h"
#include "hub_control.h"
#include "network_init.h"
#include "port_events.h"
#include "rescue_logic.h"
#include "usb/usb_types_stack.h"
#include "usb_backend.h"
#include "usbip_protocol.h"
#include "virtual_device.h"
#include "version_autogen.h"
#if CONFIG_USBIP_WIFI_STA
#include "wifi_sta.h"
#endif

static const char *TAG = "controller_api";

#define API_NVS_NAMESPACE   "api"
#define API_NVS_KEY_TOKEN   "token"
#define API_TOKEN_MAX_LEN   64
#define API_DEFAULT_OFF_MS  1000
#define API_REBOOT_DELAY_US (800 * 1000)
#define API_HTTP_PORT       80      /* http_server.c */
#define API_WIFI_WAIT_MS    10000   /* PUT /api/network/wifi waits this long for an address, at most */

#define MCP_PROTOCOL_VERSION "2025-06-18"

static char s_token[API_TOKEN_MAX_LEN + 1];

/* ===========================================================================
 *  Results
 * ======================================================================== */

typedef struct {
    int status;
    cJSON *json;
} api_result_t;

static api_result_t result(int status, cJSON *json)
{
    return (api_result_t){ .status = status, .json = json };
}

/* {"ok":false,"error":msg} */
static api_result_t fail(int status, const char *msg)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", false);
    cJSON_AddStringToObject(j, "error", msg);
    return result(status, j);
}

/* {"error":msg}, the analog mux API's error shape */
static api_result_t mux_fail(int status, const char *msg)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "error", msg);
    return result(status, j);
}

static int status_for_err(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return 200;
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_INVALID_SIZE:
        return 400;
    case ESP_ERR_NOT_FOUND:
        return 404;
    case ESP_ERR_NOT_SUPPORTED:
        return 409;
    case ESP_ERR_INVALID_STATE:
        return 409;
    case ESP_ERR_TIMEOUT:
    case ESP_ERR_NO_MEM:
        return 503;
    default:
        return 500;
    }
}

const char *controller_api_status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default:  return "Internal Server Error";
    }
}

/* ===========================================================================
 *  Auth
 * ======================================================================== */

esp_err_t controller_api_init(void)
{
    nvs_handle_t h;
    s_token[0] = '\0';
    if (nvs_open(API_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_token);
        if (nvs_get_str(h, API_NVS_KEY_TOKEN, s_token, &len) != ESP_OK) {
            s_token[0] = '\0';
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "API token %s", s_token[0] ? "set: mutating requests need Authorization: Bearer" : "not set");
    return ESP_OK;
}

bool controller_api_auth_required(void)
{
    return s_token[0] != '\0';
}

bool controller_api_authorized(const char *auth)
{
    if (s_token[0] == '\0') {
        return true;
    }
    if (auth == NULL || strncasecmp(auth, "Bearer ", 7) != 0) {
        return false;
    }
    const char *given = auth + 7;
    while (*given == ' ') {
        given++;
    }
    size_t n = strlen(s_token);
    if (strlen(given) != n) {
        return false;
    }
    unsigned diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (unsigned)(given[i] ^ s_token[i]);
    }
    return diff == 0;
}

static esp_err_t set_token(const char *token)
{
    if (strlen(token) > API_TOKEN_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(API_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = token[0] ? nvs_set_str(h, API_NVS_KEY_TOKEN, token) : nvs_erase_key(h, API_NVS_KEY_TOKEN);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        strlcpy(s_token, token, sizeof(s_token));
        discovery_service_notify_auth_changed();   /* _esp-usbip TXT "auth" */
    }
    return err;
}

/* ===========================================================================
 *  Helpers
 * ======================================================================== */

static const char *speed_name(uint32_t usbip_speed)
{
    switch (usbip_speed) {
    case 1: return "low";
    case 2: return "full";
    case 3: return "high";
    default: return "unknown";
    }
}

static int speed_mbps(uint32_t usbip_speed)
{
    switch (usbip_speed) {
    case 1: return 1;      /* 1.5 */
    case 2: return 12;
    case 3: return 480;
    default: return 0;
    }
}

static void add_hex(cJSON *obj, const char *key, unsigned value, int digits)
{
    char buf[12];
    snprintf(buf, sizeof(buf), "%0*x", digits, value);
    cJSON_AddStringToObject(obj, key, buf);
}

static void add_str_or_null(cJSON *obj, const char *key, const char *value)
{
    if (value != NULL && value[0] != '\0') {
        cJSON_AddStringToObject(obj, key, value);
    } else {
        cJSON_AddNullToObject(obj, key);
    }
}

/* Percent-decode `in` into `out`. */
static void url_decode(const char *in, size_t in_len, char *out, size_t out_size)
{
    size_t o = 0;
    for (size_t i = 0; i < in_len && o + 1 < out_size; i++) {
        if (in[i] == '%' && i + 2 < in_len && isxdigit((unsigned char)in[i + 1]) &&
                isxdigit((unsigned char)in[i + 2])) {
            char hex[3] = { in[i + 1], in[i + 2], 0 };
            out[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (in[i] == '+') {
            out[o++] = ' ';
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

/* Copy the path segment starting at `p` (up to '/' or end) decoded into out.
   Returns a pointer to the rest of the path. */
static const char *take_segment(const char *p, char *out, size_t out_size)
{
    const char *slash = strchr(p, '/');
    size_t len = slash ? (size_t)(slash - p) : strlen(p);
    url_decode(p, len, out, out_size);
    return slash ? slash : p + len;
}

static bool query_flag(const char *query, const char *name)
{
    if (query == NULL) {
        return false;
    }
    size_t n = strlen(name);
    for (const char *q = query; q && *q; ) {
        if (strncmp(q, name, n) == 0 && (q[n] == '\0' || q[n] == '&' || q[n] == '=')) {
            if (q[n] != '=') {
                return true;
            }
            const char *v = q + n + 1;
            return !(v[0] == '0' || strncasecmp(v, "false", 5) == 0 || strncasecmp(v, "no", 2) == 0);
        }
        q = strchr(q, '&');
        if (q) {
            q++;
        }
    }
    return false;
}

static int query_int(const char *query, const char *name, int def)
{
    if (query == NULL) {
        return def;
    }
    size_t n = strlen(name);
    for (const char *q = query; q && *q; ) {
        if (strncmp(q, name, n) == 0 && q[n] == '=') {
            return atoi(q + n + 1);
        }
        q = strchr(q, '&');
        if (q) {
            q++;
        }
    }
    return def;
}

/* Percent-decoded value of name in the query string; false when absent. */
static bool query_str(const char *query, const char *name, char *out, size_t out_size)
{
    if (query == NULL) {
        return false;
    }
    size_t n = strlen(name);
    for (const char *q = query; q && *q; ) {
        if (strncmp(q, name, n) == 0 && q[n] == '=') {
            const char *v = q + n + 1;
            const char *e = strchr(v, '&');
            url_decode(v, e ? (size_t)(e - v) : strlen(v), out, out_size);
            return true;
        }
        q = strchr(q, '&');
        if (q) {
            q++;
        }
    }
    return false;
}

static bool json_bool(const cJSON *obj, const char *key, bool def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(v)) {
        return cJSON_IsTrue(v);
    }
    if (cJSON_IsNumber(v)) {
        return v->valuedouble != 0;
    }
    if (cJSON_IsString(v)) {
        return strcasecmp(v->valuestring, "true") == 0 || strcmp(v->valuestring, "1") == 0 ||
               strcasecmp(v->valuestring, "on") == 0;
    }
    return def;
}

static const char *json_string(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static bool json_int(const cJSON *obj, const char *key, int *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(v)) {
        *out = (int)v->valuedouble;
        return true;
    }
    if (cJSON_IsString(v) && v->valuestring[0] != '\0') {
        char *end = NULL;
        long n = strtol(v->valuestring, &end, 10);
        if (*end == '\0') {
            *out = (int)n;
            return true;
        }
    }
    return false;
}

/* ===========================================================================
 *  Core operations (shared by REST and MCP)
 * ======================================================================== */

static api_result_t core_info(void)
{
    cJSON *j = cJSON_CreateObject();
    char buf[DEVICE_NAME_MAX_LEN];
    device_naming_get_hostname(buf, sizeof(buf));
    cJSON_AddStringToObject(j, "hostname", buf);
    if (device_naming_get_board_id(buf, sizeof(buf)) == ESP_OK) {
        cJSON_AddStringToObject(j, "board_id", buf);
    } else {
        cJSON_AddNullToObject(j, "board_id");
    }
    cJSON_AddStringToObject(j, "target", CONFIG_IDF_TARGET);
    cJSON_AddStringToObject(j, "version", HARNESS_FW_VERSION);
    cJSON_AddStringToObject(j, "api_version", BRIDGE_API_VERSION);
    ota_update_add_summary(j);
    cJSON_AddNumberToObject(j, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(j, "free_heap", esp_get_free_heap_size());
    cJSON_AddBoolToObject(j, "auth_required", s_token[0] != '\0');
    cJSON_AddBoolToObject(j, "mux_bus_ok", analog_mux_bus_ok());
    cJSON_AddNumberToObject(j, "usbip_port", 3240);
    cJSON_AddNumberToObject(j, "max_devices", CONFIG_USBIP_MAX_DEVICES);
    cJSON_AddNumberToObject(j, "max_interfaces_per_device", CONFIG_USBIP_MAX_INTERFACES);
    cJSON_AddNumberToObject(j, "max_endpoints_per_device", CONFIG_USBIP_MAX_ENDPOINTS);
    cJSON_AddNumberToObject(j, "max_inflight_transfers", CONFIG_USBIP_MAX_INFLIGHT_XFERS);
    return result(200, j);
}

static cJSON *settings_json(void);

static hub_ctl_hub_status_t *snapshot_hubs(size_t *n)
{
    hub_ctl_hub_status_t *snap = malloc(USB_BACKEND_MAX_HUBS * sizeof(*snap));
    *n = snap ? hub_ctl_snapshot(snap, USB_BACKEND_MAX_HUBS) : 0;
    return snap;
}

static const hub_ctl_port_t *find_port(const hub_ctl_hub_status_t *snap, size_t n, const char *path)
{
    for (size_t h = 0; h < n; h++) {
        for (uint8_t p = 0; p < snap[h].num_ports; p++) {
            if (strcmp(snap[h].ports[p].path, path) == 0) {
                return &snap[h].ports[p];
            }
        }
    }
    return NULL;
}

static cJSON *device_json(const usbip_backend_device_t *d, const hub_ctl_hub_status_t *snap, size_t n_hubs)
{
    cJSON *o = cJSON_CreateObject();
    const bool is_virtual = (d->busnum == VIRTUAL_DEVICE_BUSNUM);
    cJSON_AddStringToObject(o, "busid", d->busid);
    add_hex(o, "vid", d->id_vendor, 4);
    add_hex(o, "pid", d->id_product, 4);

    char desc[160];
    snprintf(desc, sizeof(desc), "%04x:%04x%s%s%s%s", d->id_vendor, d->id_product,
             d->manufacturer[0] ? " " : "", d->manufacturer,
             d->product[0] ? " " : "", d->product);
    cJSON_AddStringToObject(o, "description", desc);
    add_str_or_null(o, "manufacturer", d->manufacturer);
    add_str_or_null(o, "product", d->product);
    add_str_or_null(o, "serial", d->serial);

    char name[DEVICE_NAME_MAX_LEN] = "";
    device_naming_get_device_name(d->busid, name, sizeof(name));
    add_str_or_null(o, "name", name);

    cJSON_AddStringToObject(o, "speed", speed_name(d->speed));
    cJSON_AddNumberToObject(o, "speed_mbps", speed_mbps(d->speed));
    if (d->max_power_ma) {
        cJSON_AddNumberToObject(o, "max_power_ma", d->max_power_ma);
    } else {
        cJSON_AddNullToObject(o, "max_power_ma");
    }
    char cls[8];
    snprintf(cls, sizeof(cls), "0x%02x", d->device_class);
    cJSON_AddStringToObject(o, "device_class", cls);
    snprintf(cls, sizeof(cls), "0x%04x", d->bcd_device);
    cJSON_AddStringToObject(o, "bcd_device", cls);
    cJSON_AddNumberToObject(o, "num_interfaces", d->num_interfaces);
    cJSON *ifs = cJSON_AddArrayToObject(o, "interfaces");
    for (uint8_t i = 0; i < d->num_interfaces; i++) {
        cJSON *it = cJSON_CreateObject();
        snprintf(cls, sizeof(cls), "0x%02x", d->interfaces[i].interface_class);
        cJSON_AddStringToObject(it, "class", cls);
        snprintf(cls, sizeof(cls), "0x%02x", d->interfaces[i].interface_subclass);
        cJSON_AddStringToObject(it, "subclass", cls);
        snprintf(cls, sizeof(cls), "0x%02x", d->interfaces[i].interface_protocol);
        cJSON_AddStringToObject(it, "protocol", cls);
        cJSON_AddItemToArray(ifs, it);
    }
    cJSON_AddBoolToObject(o, "virtual", is_virtual);
    if (is_virtual) {
        cJSON_AddNullToObject(o, "address");
        cJSON_AddNullToObject(o, "hub");
        cJSON_AddNullToObject(o, "port");
    } else {
        cJSON_AddNumberToObject(o, "address", d->dev_addr);
        if (d->parent_hub_addr) {
            /* The hub path is the busid without its last ".N" */
            char hub_path[32];
            strlcpy(hub_path, d->busid, sizeof(hub_path));
            char *dot = strrchr(hub_path, '.');
            if (dot) {
                *dot = '\0';
            }
            cJSON_AddStringToObject(o, "hub", hub_path);
            cJSON_AddNumberToObject(o, "port", d->parent_port);
        } else {
            cJSON_AddNullToObject(o, "hub");
            cJSON_AddNullToObject(o, "port");
        }
    }
    busid_owner_info_t owner;
    const bool owned = busid_owner_get(d->busid, &owner);
    cJSON_AddBoolToObject(o, "imported", owned && owner.kind == BUSID_OWNER_USBIP);
    if (owned) {
        cJSON *ow = cJSON_AddObjectToObject(o, "owner");
        cJSON_AddStringToObject(ow, "kind", owner.kind == BUSID_OWNER_USBIP ? "usbip" : "rescue");
        cJSON_AddStringToObject(ow, "peer", owner.peer);
    } else {
        cJSON_AddNullToObject(o, "owner");
    }
    const hub_ctl_port_t *port = is_virtual ? NULL : find_port(snap, n_hubs, d->busid);
    if (port) {
        cJSON_AddStringToObject(o, "port_power_status", port->powered ? "on" : "off");
        cJSON_AddStringToObject(o, "port_connect_status", port->connected ? "connected" : "disconnected");
    } else {
        cJSON_AddNullToObject(o, "port_power_status");
        cJSON_AddNullToObject(o, "port_connect_status");
    }
    return o;
}

static api_result_t core_devices(void)
{
    size_t devs_max = CONFIG_USBIP_MAX_DEVICES + VIRTUAL_DEVICE_MAX;
    usbip_backend_device_t *devs = malloc(devs_max * sizeof(*devs));
    if (devs == NULL) {
        return fail(503, "out of memory");
    }
    size_t n = usb_backend_get_devices(devs, devs_max);
    size_t n_hubs = 0;
    hub_ctl_hub_status_t *snap = snapshot_hubs(&n_hubs);

    cJSON *j = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(j, "devices");
    for (size_t i = 0; i < n; i++) {
        cJSON_AddItemToArray(arr, device_json(&devs[i], snap, n_hubs));
    }
    free(snap);
    free(devs);
    return result(200, j);
}

static api_result_t core_device(const char *busid)
{
    char key[32] = {0};
    strlcpy(key, busid, sizeof(key));
    usbip_backend_device_t *dev = malloc(sizeof(*dev));
    if (dev == NULL) {
        return fail(503, "out of memory");
    }
    if (!usb_backend_get_device_by_busid(key, dev)) {
        free(dev);
        return fail(404, "no device with that busid");
    }
    size_t n_hubs = 0;
    hub_ctl_hub_status_t *snap = snapshot_hubs(&n_hubs);
    cJSON *j = device_json(dev, snap, n_hubs);
    free(snap);
    free(dev);
    return result(200, j);
}

static cJSON *port_json(const hub_ctl_port_t *p)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "port", p->port);
    cJSON_AddStringToObject(o, "path", p->path);
    cJSON_AddStringToObject(o, "power", p->powered ? "on" : "off");
    cJSON_AddBoolToObject(o, "connected", p->connected);
    cJSON_AddBoolToObject(o, "enabled", p->enabled);
    cJSON_AddBoolToObject(o, "suspended", p->suspended);
    cJSON_AddBoolToObject(o, "over_current", p->over_current);
    cJSON_AddBoolToObject(o, "user_off", p->user_off);
    cJSON_AddStringToObject(o, "desired", p->desired_on ? "on" : "off");
    add_str_or_null(o, "pending", p->pending);
    cJSON_AddBoolToObject(o, "mismatch", p->mismatch);
    port_last_change_t lc;
    if (port_events_last_change(p->path, &lc)) {
        cJSON *lcj = cJSON_AddObjectToObject(o, "last_change");
        cJSON_AddNumberToObject(lcj, "when_ms", lc.t_ms);
        cJSON_AddStringToObject(lcj, "what", lc.what);
        cJSON_AddStringToObject(lcj, "attribution", lc.attribution);
    }
    cJSON_AddNumberToObject(o, "enum_timeout_ms", p->enum_timeout_ms);
    cJSON_AddBoolToObject(o, "enum_timeout_override", p->enum_timeout_override);
    add_str_or_null(o, "speed", p->speed);
    if (p->has_device || p->has_hub) {
        cJSON *d = cJSON_AddObjectToObject(o, "device");
        cJSON_AddStringToObject(d, "type", p->has_hub ? "hub" : "device");
        cJSON_AddStringToObject(d, "busid", p->path);
        add_hex(d, "vid", p->id_vendor, 4);
        add_hex(d, "pid", p->id_product, 4);
    } else {
        cJSON_AddNullToObject(o, "device");
    }
    return o;
}

static api_result_t core_hubs(void)
{
    size_t n = 0;
    hub_ctl_hub_status_t *snap = snapshot_hubs(&n);
    if (snap == NULL) {
        return fail(503, "out of memory");
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddItemToObject(j, "settings", settings_json());
    cJSON *arr = cJSON_AddArrayToObject(j, "hubs");
    for (size_t h = 0; h < n; h++) {
        const hub_ctl_hub_status_t *s = &snap[h];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "path", s->hub.path);
        cJSON_AddNumberToObject(o, "address", s->hub.addr);
        add_hex(o, "vid", s->hub.id_vendor, 4);
        add_hex(o, "pid", s->hub.id_product, 4);
        add_str_or_null(o, "manufacturer", s->hub.manufacturer);
        add_str_or_null(o, "product", s->hub.product);
        cJSON_AddBoolToObject(o, "ready", s->info_ok);
        cJSON_AddNumberToObject(o, "enum_timeout_ms", s->enum_timeout_ms);
        cJSON_AddBoolToObject(o, "enum_timeout_override", s->enum_timeout_override);
        if (s->info_ok) {
            cJSON_AddNumberToObject(o, "num_ports", s->info.num_ports);
            cJSON_AddStringToObject(o, "power_switching", hub_ctl_power_switching_name(s->info.power_switching));
            cJSON_AddNumberToObject(o, "pwr_on_to_pwr_good_ms", s->info.pwr_on_to_pwr_good_ms);
            cJSON_AddBoolToObject(o, "compound", s->info.compound);
            cJSON *c = cJSON_AddObjectToObject(o, "characteristics");
            char raw[8];
            snprintf(raw, sizeof(raw), "0x%04x", s->info.characteristics);
            cJSON_AddStringToObject(c, "raw", raw);
            cJSON_AddStringToObject(c, "power_switching", hub_ctl_power_switching_name(s->info.power_switching));
            cJSON_AddStringToObject(c, "over_current_protection",
                                    hub_ctl_over_current_name(s->info.over_current_protection));
            cJSON_AddBoolToObject(c, "compound", s->info.compound);
            cJSON_AddNumberToObject(c, "tt_think_time_fs_bits", 8 * (s->info.tt_think_time + 1));
            cJSON_AddBoolToObject(c, "port_indicators", s->info.port_indicators);
            cJSON_AddNumberToObject(c, "pwr_on_to_pwr_good_ms", s->info.pwr_on_to_pwr_good_ms);
            cJSON_AddNumberToObject(c, "hub_contr_current_ma", s->info.hub_contr_current_ma);
        }
        cJSON *ports = cJSON_AddArrayToObject(o, "ports");
        for (uint8_t p = 0; p < s->num_ports; p++) {
            cJSON_AddItemToArray(ports, port_json(&s->ports[p]));
        }
        cJSON_AddItemToArray(arr, o);
    }
    free(snap);
    return result(200, j);
}

/* GET /api/ports/events: the port state transition log, newest first, with attribution
   (api / device / external; via restore for the saved-state policy) and counters. */
static api_result_t core_port_events(void)
{
    port_event_t *ev = malloc(PORT_EVENT_LOG_LEN * sizeof(*ev));
    if (ev == NULL) {
        return fail(503, "out of memory");
    }
    const size_t n = port_events_get(ev, PORT_EVENT_LOG_LEN);
    uint32_t ext = 0;
    uint32_t replugs = 0;
    port_events_counters(&ext, &replugs);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "external_power_changes", ext);
    cJSON_AddNumberToObject(j, "device_replugs", replugs);
    cJSON *arr = cJSON_AddArrayToObject(j, "events");
    for (size_t i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "when_ms", ev[i].t_ms);
        cJSON_AddStringToObject(e, "port", ev[i].port);
        cJSON_AddStringToObject(e, "what", ev[i].what);
        cJSON_AddStringToObject(e, "from", ev[i].from);
        cJSON_AddStringToObject(e, "to", ev[i].to);
        cJSON_AddStringToObject(e, "attribution", ev[i].attribution);
        if (ev[i].via[0] != '\0') {
            cJSON_AddStringToObject(e, "via", ev[i].via);
        }
        cJSON_AddBoolToObject(e, "mismatch", ev[i].mismatch);
        cJSON_AddItemToArray(arr, e);
    }
    free(ev);
    return result(200, j);
}

static api_result_t core_ports(void)
{
    size_t n = 0;
    hub_ctl_hub_status_t *snap = snapshot_hubs(&n);
    if (snap == NULL) {
        return fail(503, "out of memory");
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON *arr = cJSON_AddArrayToObject(j, "ports");
    for (size_t h = 0; h < n; h++) {
        for (uint8_t p = 0; p < snap[h].num_ports; p++) {
            cJSON *pj = port_json(&snap[h].ports[p]);
            cJSON_AddStringToObject(pj, "power_switching",
                                    hub_ctl_power_switching_name(snap[h].info.power_switching));
            cJSON_AddItemToArray(arr, pj);
        }
    }
    free(snap);
    return result(200, j);
}

static api_result_t core_port_get(const char *path)
{
    size_t n = 0;
    hub_ctl_hub_status_t *snap = snapshot_hubs(&n);
    if (snap == NULL) {
        return fail(503, "out of memory");
    }
    const hub_ctl_port_t *p = find_port(snap, n, path);
    api_result_t r;
    if (p == NULL) {
        r = fail(404, "no such hub port");
    } else {
        cJSON *j = port_json(p);
        cJSON_AddBoolToObject(j, "ok", true);
        r = result(200, j);
    }
    free(snap);
    return r;
}

static api_result_t port_action_ok(const char *path, const char *action)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddStringToObject(j, "port", path);
    cJSON_AddStringToObject(j, "action", action);
    return result(200, j);
}

static api_result_t core_port_power(const char *path, bool on, bool force)
{
    char msg[200] = "";
    bool pending = false;
    esp_err_t err = hub_ctl_port_power(path, on, force, &pending, msg, sizeof(msg));
    if (err != ESP_OK) {
        return fail(status_for_err(err), msg[0] ? msg : esp_err_to_name(err));
    }
    api_result_t r = port_action_ok(path, on ? "on" : "off");
    /* 202: the port is busy (e.g. its device is enumerating); applied shortly */
    cJSON_AddBoolToObject(r.json, "pending", pending);
    r.status = pending ? 202 : 200;
    return r;
}

static api_result_t core_port_cycle(const char *path, int off_ms, bool force)
{
    if (off_ms < 0) {
        return fail(400, "off_ms must be >= 0");
    }
    char msg[200] = "";
    esp_err_t err = hub_ctl_port_cycle(path, (uint32_t)off_ms, force, msg, sizeof(msg));
    if (err != ESP_OK) {
        return fail(status_for_err(err), msg[0] ? msg : esp_err_to_name(err));
    }
    api_result_t r = port_action_ok(path, "cycle");
    cJSON_AddNumberToObject(r.json, "off_ms", off_ms);
    cJSON_AddBoolToObject(r.json, "async", true);
    return r;
}

static api_result_t core_ports_all(bool on)
{
    int n = hub_ctl_all_ports(on);
    if (n < 0) {
        return fail(503, "out of memory");
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddStringToObject(j, "action", on ? "all_on" : "all_off");
    cJSON_AddNumberToObject(j, "ports", n);
    return result(200, j);
}

static cJSON *settings_json(void)
{
    hub_ctl_settings_t st;
    hub_ctl_get_settings(&st);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "enforce_per_port_switching", st.enforce_per_port);
    cJSON_AddBoolToObject(j, "restore_port_power", st.restore_on_reset);
    cJSON_AddNumberToObject(j, "enum_timeout_ms", st.enum_timeout_ms);
    cJSON *ov = cJSON_AddArrayToObject(j, "enum_timeout_overrides");
    hub_ctl_timeout_override_t t[HUB_CTL_MAX_TIMEOUT_OVERRIDES];
    size_t n = hub_ctl_get_enum_timeouts(t, HUB_CTL_MAX_TIMEOUT_OVERRIDES);
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "path", t[i].path);
        cJSON_AddNumberToObject(o, "timeout_ms", t[i].timeout_ms);
        cJSON_AddItemToArray(ov, o);
    }
    return j;
}

/* {"path": "1-1.3", "timeout_ms": int|null}; null or a negative value removes
   the override.  Without "path" the global default is set. */
static api_result_t core_set_enum_timeout(const cJSON *args)
{
    const char *path = json_string(args, "path");
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, "timeout_ms");
    int ms = -1;
    if (!cJSON_IsNull(v) && !json_int(args, "timeout_ms", &ms)) {
        return fail(400, "timeout_ms must be an integer (ms, 0 = no timeout) or null");
    }
    if (ms > 60000) {
        return fail(400, "timeout_ms is limited to 60000");
    }
    if (path == NULL) {
        if (ms < 0) {
            return fail(400, "the global timeout cannot be removed; use 0 for no timeout");
        }
        hub_ctl_settings_t st;
        hub_ctl_get_settings(&st);
        st.enum_timeout_ms = (uint32_t)ms;
        esp_err_t err = hub_ctl_set_settings(&st);
        if (err != ESP_OK) {
            return fail(500, esp_err_to_name(err));
        }
    } else {
        esp_err_t err = hub_ctl_set_enum_timeout(path, ms);
        if (err != ESP_OK) {
            return fail(status_for_err(err), err == ESP_ERR_NO_MEM ? "too many overrides"
                        : "bad path (a port or hub path such as 1-1.3)");
        }
    }
    cJSON *j = settings_json();
    cJSON_AddBoolToObject(j, "ok", true);
    return result(200, j);
}

static api_result_t core_get_settings(void)
{
    return result(200, settings_json());
}

/* Update the settings present in `args` (others unchanged) */
static api_result_t core_set_settings(const cJSON *args)
{
    if (!cJSON_IsObject(args)) {
        return fail(400, "expected {\"enforce_per_port_switching\": bool, \"restore_port_power\": bool}");
    }
    hub_ctl_settings_t st;
    hub_ctl_get_settings(&st);
    st.enforce_per_port = json_bool(args, "enforce_per_port_switching", st.enforce_per_port);
    st.restore_on_reset = json_bool(args, "restore_port_power", st.restore_on_reset);
    int to;
    if (json_int(args, "enum_timeout_ms", &to)) {
        if (to < 0 || to > 60000) {
            return fail(400, "enum_timeout_ms must be 0..60000 (0 = no timeout)");
        }
        st.enum_timeout_ms = (uint32_t)to;
    }
    esp_err_t err = hub_ctl_set_settings(&st);
    if (err != ESP_OK) {
        return fail(500, esp_err_to_name(err));
    }
    cJSON *j = settings_json();
    cJSON_AddBoolToObject(j, "ok", true);
    return result(200, j);
}

static const char *enum_kind_name(uint8_t kind)
{
    switch (kind) {
    case USB_HOST_ENUM_EVENT_STARTED: return "started";
    case USB_HOST_ENUM_EVENT_COMPLETED: return "completed";
    default: return "failed";
    }
}

static const char *transfer_status_name(int status)
{
    switch (status) {
    case USB_TRANSFER_STATUS_COMPLETED: return "completed";
    case USB_TRANSFER_STATUS_ERROR: return "error";
    case USB_TRANSFER_STATUS_TIMED_OUT: return "timed_out";
    case USB_TRANSFER_STATUS_CANCELED: return "canceled";
    case USB_TRANSFER_STATUS_STALL: return "stall";
    case USB_TRANSFER_STATUS_OVERFLOW: return "overflow";
    case USB_TRANSFER_STATUS_SKIPPED: return "skipped";
    case USB_TRANSFER_STATUS_NO_DEVICE: return "no_device";
    default: return NULL;
    }
}

/* What a failure says about the device */
static const char *enum_fail_diagnosis(const hub_ctl_enum_event_t *e)
{
    switch (e->fail_reason) {
    case USB_HOST_ENUM_FAIL_TIMEOUT:
        return "no answer within the timeout: device connected but slow or stalled (NAKing); transfer aborted, port retried";
    case USB_HOST_ENUM_FAIL_DISCONNECTED:
        return "device disconnected during enumeration: rebooting or unplugged";
    default:
        if (e->transfer_status == USB_TRANSFER_STATUS_STALL) {
            return "device rejected a request (STALL)";
        }
        if (e->transfer_status == USB_TRANSFER_STATUS_NO_DEVICE ||
                e->transfer_status == USB_TRANSFER_STATUS_ERROR) {
            return "transfer error: device reset or dropped off the bus (typically rebooting)";
        }
        return "transfer failed or returned bad data";
    }
}

static cJSON *enum_event_json(const hub_ctl_enum_event_t *log, size_t n, size_t i, uint32_t now_ms)
{
    const hub_ctl_enum_event_t *e = &log[i];
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "t_ms", e->t_ms);
    cJSON_AddNumberToObject(o, "age_ms", now_ms - e->t_ms);
    cJSON_AddStringToObject(o, "path", e->path);
    cJSON_AddStringToObject(o, "event", enum_kind_name(e->kind));
    if (e->kind == USB_HOST_ENUM_EVENT_STARTED) {
        cJSON_AddNumberToObject(o, "timeout_ms", e->timeout_ms);
        return o;
    }
    cJSON_AddNumberToObject(o, "elapsed_ms", e->elapsed_ms);
    if (e->kind == USB_HOST_ENUM_EVENT_COMPLETED) {
        add_hex(o, "vid", e->vid, 4);
        add_hex(o, "pid", e->pid, 4);
        cJSON_AddNumberToObject(o, "address", e->dev_addr);
        return o;
    }
    static const char *const reasons[] = { "none", "timeout", "transfer_error", "disconnected" };
    cJSON_AddStringToObject(o, "reason", e->fail_reason < 4 ? reasons[e->fail_reason] : "unknown");
    add_str_or_null(o, "stage", e->stage);
    add_str_or_null(o, "transfer_status", transfer_status_name(e->transfer_status));
    cJSON_AddNumberToObject(o, "timeout_ms", e->timeout_ms);
    cJSON_AddStringToObject(o, "diagnosis", enum_fail_diagnosis(e));
    /* What happened next on this port */
    const char *outcome = "not re-enumerated yet";
    int32_t after_ms = -1;
    for (size_t k = i + 1; k < n; k++) {
        if (strcmp(log[k].path, e->path) != 0) {
            continue;
        }
        if (log[k].kind == USB_HOST_ENUM_EVENT_COMPLETED) {
            outcome = "re-enumerated";
            after_ms = (int32_t)(log[k].t_ms - e->t_ms);
            break;
        }
        if (log[k].kind == USB_HOST_ENUM_EVENT_FAILED) {
            outcome = "failed again";
            after_ms = (int32_t)(log[k].t_ms - e->t_ms);
            break;
        }
    }
    cJSON_AddStringToObject(o, "outcome", outcome);
    if (after_ms >= 0) {
        cJSON_AddNumberToObject(o, "outcome_after_ms", after_ms);
    }
    return o;
}

static api_result_t core_enum_events(void)
{
    hub_ctl_enum_event_t *log = malloc(HUB_CTL_ENUM_LOG_LEN * sizeof(*log));
    if (log == NULL) {
        return fail(503, "out of memory");
    }
    size_t n = hub_ctl_get_enum_log(log, HUB_CTL_ENUM_LOG_LEN);
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    cJSON *j = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(j, "events");
    size_t failures = 0;
    for (size_t i = n; i-- > 0;) {      /* newest first */
        cJSON_AddItemToArray(arr, enum_event_json(log, n, i, now_ms));
        failures += (log[i].kind == USB_HOST_ENUM_EVENT_FAILED);
    }
    cJSON_AddNumberToObject(j, "failures", failures);

    /* Per port: how often it enumerated, how it failed, and what that says */
    cJSON *ports = cJSON_AddObjectToObject(j, "ports");
    for (size_t i = 0; i < n; i++) {
        if (cJSON_GetObjectItemCaseSensitive(ports, log[i].path) != NULL) {
            continue;
        }
        uint32_t completed = 0, timeouts = 0, resets = 0, first_ok = 0, last_ok = 0;
        for (size_t k = i; k < n; k++) {
            if (strcmp(log[k].path, log[i].path) != 0) {
                continue;
            }
            if (log[k].kind == USB_HOST_ENUM_EVENT_COMPLETED) {
                if (completed++ == 0) {
                    first_ok = log[k].t_ms;
                }
                last_ok = log[k].t_ms;
            } else if (log[k].kind == USB_HOST_ENUM_EVENT_FAILED) {
                if (log[k].fail_reason == USB_HOST_ENUM_FAIL_TIMEOUT) {
                    timeouts++;
                } else {
                    resets++;
                }
            }
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "enumerations", completed);
        cJSON_AddNumberToObject(o, "timeouts", timeouts);
        cJSON_AddNumberToObject(o, "resets_during_enumeration", resets);
        if (completed > 0) {
            cJSON_AddNumberToObject(o, "last_enumerated_ms_ago", now_ms - last_ok);
        }
        const char *verdict;
        if (timeouts > 0) {
            /* No answer while connected: slow or stalled; the port keeps being retried */
            verdict = completed > 0 ? "stalled at times (timed out, recovered on retry)"
                                    : "stalled: connected but not answering (retrying with back-off)";
        } else if (completed > 1 || resets > 0) {
            /* Enumerated repeatedly (each time after a disconnect) or dropped mid-enumeration */
            verdict = "rebooting: disconnects and re-enumerates";
            if (completed > 1) {
                cJSON_AddNumberToObject(o, "mean_interval_ms", (last_ok - first_ok) / (completed - 1));
            }
        } else {
            verdict = "ok";
        }
        cJSON_AddStringToObject(o, "verdict", verdict);
        cJSON_AddItemToObject(ports, log[i].path, o);
    }
    cJSON_AddNumberToObject(j, "window_ms", n ? now_ms - log[0].t_ms : 0);
    free(log);
    return result(200, j);
}

/* GET /api/ports/hwstatus: hub-class GetPortStatus for every downstream port of every hub the
   bridge manages, issued to the hub itself. hw_mismatch is set when the bridge's claimed power
   state for the port differs from the hub's reported power bit. */
static api_result_t core_port_hwstatus(void)
{
    hub_ctl_hub_status_t *snap = malloc(USB_BACKEND_MAX_HUBS * sizeof(*snap));
    if (snap == NULL) {
        return fail(503, "out of memory");
    }
    const size_t nh = hub_ctl_snapshot(snap, USB_BACKEND_MAX_HUBS);
    cJSON *j = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(j, "ports");
    size_t mismatches = 0;
    for (size_t h = 0; h < nh; h++) {
        for (uint8_t p = 0; p < snap[h].num_ports && p < HUB_CTL_MAX_PORTS; p++) {
            const hub_ctl_port_t *ps = &snap[h].ports[p];
            usb_setup_packet_t setup;
            memset(&setup, 0, sizeof(setup));
            setup.bmRequestType = 0xA3;             /* device-to-host | class | other */
            setup.bRequest = 0;                     /* GET_STATUS */
            setup.wIndex = ps->port;
            setup.wLength = 4;
            uint8_t buf[4] = {0};
            size_t len = 0;
            const char *cls = "none";
            const esp_err_t err = usb_backend_hub_control(snap[h].hub.addr, &setup, buf, sizeof(buf), &len, &cls);
            cJSON *e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "hub", snap[h].hub.path);
            cJSON_AddNumberToObject(e, "port", ps->port);
            cJSON_AddStringToObject(e, "path", ps->path);
            cJSON_AddBoolToObject(e, "claimed_power", ps->powered);
            if (err == ESP_OK && len >= 4) {
                const uint16_t st = (uint16_t)(buf[0] | (buf[1] << 8));
                const uint16_t ch = (uint16_t)(buf[2] | (buf[3] << 8));
                const bool hw_power = (st & 0x0100) != 0;
                cJSON_AddStringToObject(e, "status_hex", "");
                char hx[8];
                snprintf(hx, sizeof(hx), "0x%04x", st);
                cJSON_ReplaceItemInObject(e, "status_hex", cJSON_CreateString(hx));
                snprintf(hx, sizeof(hx), "0x%04x", ch);
                cJSON_AddStringToObject(e, "change_hex", hx);
                cJSON_AddBoolToObject(e, "connected", (st & 0x0001) != 0);
                cJSON_AddBoolToObject(e, "enabled", (st & 0x0002) != 0);
                cJSON_AddBoolToObject(e, "suspended", (st & 0x0004) != 0);
                cJSON_AddBoolToObject(e, "over_current", (st & 0x0008) != 0);
                cJSON_AddBoolToObject(e, "power_hw", hw_power);
                cJSON_AddBoolToObject(e, "high_speed", (st & 0x0400) != 0);
                cJSON_AddBoolToObject(e, "low_speed", (st & 0x0200) != 0);
                const bool mismatch = (ps->powered != hw_power);
                cJSON_AddBoolToObject(e, "hw_mismatch", mismatch);
                mismatches += mismatch ? 1 : 0;
            } else {
                ESP_LOGW(TAG, "hwstatus: GetPortStatus %s port %u failed: %s (%s)",
                         snap[h].hub.path, (unsigned)ps->port, cls, esp_err_to_name(err));
                cJSON_AddStringToObject(e, "error", cls);
                cJSON_AddStringToObject(e, "error_name", esp_err_to_name(err));
            }
            cJSON_AddItemToArray(arr, e);
        }
    }
    cJSON_AddNumberToObject(j, "hw_mismatches", (double)mismatches);
    free(snap);
    return result(200, j);
}

/* POST /api/usb/debug_ctrl (issue #37): one control transfer through the forwarded-EP0
   backend path, no usbip in the loop. Body: busid, bm, breq, wvalue, windex, wlength. */
static api_result_t core_debug_ctrl(const cJSON *json)
{
    if (!cJSON_IsObject(json)) {
        return fail(400, "body must be a JSON object");
    }
    const char *busid = json_string(json, "busid");
    if (busid == NULL || strlen(busid) == 0 || strlen(busid) > 31) {
        return fail(400, "busid (string) required");
    }
    int bm = 0, breq = 0, wvalue = 0, windex = 0, wlength = 0;
    json_int(json, "bm", &bm);
    json_int(json, "breq", &breq);
    json_int(json, "wvalue", &wvalue);
    json_int(json, "windex", &windex);
    json_int(json, "wlength", &wlength);
    if (bm < 0 || bm > 255 || breq < 0 || breq > 255 || wvalue < 0 || wvalue > 0xffff
        || windex < 0 || windex > 0xffff || wlength < 0 || wlength > 4096) {
        return fail(400, "bm/breq 0-255, wvalue/windex 0-65535, wlength 0-4096");
    }
    if (!(bm & 0x80) && wlength > 0) {
        return fail(400, "OUT requests with data are not supported here; use wlength 0");
    }

    usb_setup_packet_t setup;
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = (uint8_t)bm;
    setup.bRequest = (uint8_t)breq;
    setup.wValue = (uint16_t)wvalue;
    setup.wIndex = (uint16_t)windex;
    setup.wLength = (uint16_t)wlength;

    uint8_t *in_buf = malloc(wlength > 0 ? (size_t)wlength : 1);
    if (in_buf == NULL) {
        return fail(503, "out of memory");
    }
    char bus[32] = {0};
    strlcpy(bus, busid, sizeof(bus));
    /* "dump": arm the EPDUMP knob for this device's address (issue #37). */
    bool dump_armed = false;
    if (json_bool(json, "dump", false)) {
        int dump_count = 4;
        json_int(json, "count", &dump_count);
        if (dump_count < 1 || dump_count > 64) {
            free(in_buf);
            return fail(400, "count 1-64");
        }
        usbip_backend_device_t dev;
        if (usb_backend_get_device_by_busid(bus, &dev)) {
            usb_backend_arm_ep0_dump(dev.dev_addr, dump_count);
            dump_armed = true;
        }
    }
    size_t in_len = 0;
    const char *detail = "none";
    int status = 0;
    uint8_t hub_addr = 0;
    usbip_backend_device_t probe;
    if (!usb_backend_get_device_by_busid(bus, &probe) && usb_backend_hub_addr_by_path(bus, &hub_addr)) {
        /* A managed hub: control transfers only (this request is always ep 0) */
        const esp_err_t herr = usb_backend_hub_control(hub_addr, &setup, in_buf, (size_t)wlength, &in_len, &detail);
        status = (herr == ESP_OK) ? 0 : -EIO;
    } else {
        status = usb_backend_debug_control(bus, &setup, in_buf, (size_t)wlength, &in_len, &detail);
    }

    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "status", status);
    cJSON_AddNumberToObject(j, "actual_length", (double)in_len);
    cJSON_AddStringToObject(j, "error_detail", detail != NULL ? detail : "none");
    char hex[65] = {0};
    const size_t show = in_len < 32 ? in_len : 32;
    for (size_t i = 0; i < show; i++) {
        snprintf(&hex[i * 2], 3, "%02x", in_buf[i]);
    }
    cJSON_AddStringToObject(j, "data_hex", hex);
    cJSON_AddBoolToObject(j, "dump_armed", dump_armed);
    free(in_buf);
    return result(200, j);
}

/* POST /api/usb/debug_dump_enum (issue #37): arm EPDUMP for address 0 (the enumeration's
   first descriptor read, CHECK_SHORT_DEV_DESC) and power-cycle the port so the stack
   enumerates again. The first 4 control executions on address 0 are dumped to the log. */
static api_result_t core_debug_dump_enum(const cJSON *json)
{
    const char *busid = json_string(json, "busid");
    if (busid == NULL || strlen(busid) == 0 || strlen(busid) > 31) {
        return fail(400, "busid (port path, e.g. 1-1.3.3) required");
    }
    /* "all": true arms the wildcard (every control execution on any address, 16 of them), so
       the window covers address 0 AND the new address of this enumeration, and a debug_ctrl
       sent right after lands inside it. Otherwise only address 0, 4 executions. */
    const bool all = json_bool(json, "all", false);
    int count = 16;
    json_int(json, "count", &count);
    if (count < 1 || count > 64) {
        return fail(400, "count 1-64");
    }
    usb_backend_arm_ep0_dump(all ? 0xFF : 0, count);
    api_result_t off = core_port_power(busid, false, true);
    if (off.json != NULL) {
        cJSON_Delete(off.json);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    api_result_t on = core_port_power(busid, true, true);
    if (on.json != NULL) {
        cJSON_Delete(on.json);
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "busid", busid);
    cJSON_AddStringToObject(j, "armed_address", all ? "any" : "0");
    cJSON_AddNumberToObject(j, "armed_count", count);
    cJSON_AddNumberToObject(j, "off_status", off.status);
    cJSON_AddNumberToObject(j, "on_status", on.status);
    cJSON_AddStringToObject(j, "note", "EPDUMP lines appear on the console (prefix EPDUMP:)");
    return result(200, j);
}

/* GET /api/usb/claims: interface-claim and channel-pressure accounting.
   The DWC OTG host has 16 channels; EP0 of every connected device and each
   external hub's interrupt pipe hold one for good, and every claimed
   endpoint holds one until the interface is released.  The host library
   does not expose its per-channel table, so estimated_channels_in_use is a
   documented lower bound, not an authoritative ledger. */
static api_result_t core_usb_claims(void)
{
    usb_backend_claims_device_t *devs =
        calloc(CONFIG_USBIP_MAX_DEVICES, sizeof(*devs));
    if (devs == NULL) {
        return fail(503, "out of memory");
    }
    usb_backend_claims_totals_t totals;
    const size_t n = usb_backend_get_claims(devs, CONFIG_USBIP_MAX_DEVICES, &totals);

    cJSON *j = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(j, "devices");
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "busid", devs[i].busid);
        cJSON *ints = cJSON_AddArrayToObject(o, "claimed_interfaces");
        for (int k = 0; k < devs[i].num_claimed_interfaces; k++) {
            cJSON_AddItemToArray(ints, cJSON_CreateNumber(devs[i].claimed_interfaces[k]));
        }
        cJSON_AddNumberToObject(o, "claimed_endpoints", devs[i].num_claimed_endpoints);
        cJSON_AddNumberToObject(o, "in_flight", devs[i].in_flight);
        cJSON_AddBoolToObject(o, "release_pending", devs[i].release_pending);
        cJSON_AddNumberToObject(o, "claim_failures", devs[i].claim_failures);
        if (devs[i].last_claim_error[0] != '\0') {
            cJSON_AddStringToObject(o, "last_claim_error", devs[i].last_claim_error);
            cJSON_AddBoolToObject(o, "last_claim_error_was_exhaustion",
                                  devs[i].last_claim_was_exhaustion);
        }
        cJSON_AddItemToArray(arr, o);
    }
    free(devs);

    cJSON *tt = cJSON_AddObjectToObject(j, "totals");
    cJSON_AddNumberToObject(tt, "channels_total",
                            totals.hcd_channels_total > 0 ? totals.hcd_channels_total : 16);
    if (totals.hcd_channels_allocated >= 0) {
        /* The HCD's own ledger: every allocated channel, including the
           EP0/hub/host-stack pipes the estimate cannot see. */
        cJSON_AddNumberToObject(tt, "channels_in_use", totals.hcd_channels_allocated);
    }
    if (totals.hcd_channel_waiters >= 0) {
        /* Dynamic channel binding: CTRL/BULK pipes queued for a free channel
           (their transfers run as soon as one frees, instead of failing). */
        cJSON_AddNumberToObject(tt, "channels_waiting", totals.hcd_channel_waiters);
    }
    /* Parked-listener and rotation counters (short flat keys, phase 2f/2g). */
    if (totals.parked_bound >= 0) {
        cJSON_AddNumberToObject(tt, "parked_bound", totals.parked_bound);
        cJSON_AddNumberToObject(tt, "parked_intr_bound", totals.parked_intr_bound);
        cJSON_AddBoolToObject(tt, "rotor_active", totals.rotor_active == 1);
        cJSON_AddNumberToObject(tt, "promotions", totals.promotions);
        cJSON_AddNumberToObject(tt, "yields_req", totals.yields_req);
        cJSON_AddNumberToObject(tt, "yields_done", totals.yields_done);
        cJSON_AddNumberToObject(tt, "cap_refusals", totals.cap_refusals);
        cJSON_AddNumberToObject(tt, "reserve_refusals", totals.reserve_refusals);
        cJSON_AddNumberToObject(tt, "rot_passes", totals.rot_passes);
        cJSON_AddNumberToObject(tt, "rej_not_bound", totals.rej_not_bound);
        cJSON_AddNumberToObject(tt, "rej_not_parked", totals.rej_not_parked);
        cJSON_AddNumberToObject(tt, "rej_pause", totals.rej_pause);
        cJSON_AddNumberToObject(tt, "rej_halt_wait", totals.rej_halt_wait);
        cJSON_AddNumberToObject(tt, "rej_state", totals.rej_state);
        cJSON_AddNumberToObject(tt, "rej_not_exec", totals.rej_not_exec);
        cJSON_AddNumberToObject(tt, "rej_urb_null", totals.rej_urb_null);
        cJSON_AddNumberToObject(tt, "rej_too_big", totals.rej_too_big);
        cJSON_AddNumberToObject(tt, "halt_noop", totals.halt_noop);
    }
    /* Heap telemetry (bytes): total free, minimum ever free, DMA free, largest DMA block. */
    cJSON_AddNumberToObject(tt, "heap_free", totals.heap_free);
    cJSON_AddNumberToObject(tt, "heap_min", totals.heap_min);
    cJSON_AddNumberToObject(tt, "dma_free", totals.dma_free);
    cJSON_AddNumberToObject(tt, "dma_largest", totals.dma_largest);
    cJSON_AddNumberToObject(tt, "inflight_now", totals.inflight_now);
    cJSON_AddNumberToObject(tt, "inflight_peak", totals.inflight_peak);
    cJSON_AddNumberToObject(tt, "connected_devices", totals.connected_devices);
    cJSON_AddNumberToObject(tt, "external_hubs", totals.external_hubs);
    cJSON_AddNumberToObject(tt, "claimed_interfaces", totals.claimed_interfaces);
    cJSON_AddNumberToObject(tt, "claimed_endpoints", totals.claimed_endpoints);
    /* Each external hub holds TWO channels: its interrupt pipe and its own
       EP0. Confirmed against the authoritative ledger on the bench
       (2026-10-26): devices + 2*hubs + claimed endpoints matched
       channels_in_use at every load step. */
    cJSON_AddNumberToObject(tt, "estimated_channels_in_use",
                            totals.connected_devices + 2 * totals.external_hubs
                            + totals.claimed_endpoints);
    cJSON_AddStringToObject(tt, "estimate_note",
        totals.hcd_channels_allocated >= 0
            ? "channels_in_use is the HCD's own ledger (authoritative); "
              "estimated_channels_in_use is claim-derived (devices + 2*hubs + "
              "claimed endpoints, bench-validated 2026-10-26)"
            : "EP0 per connected device + EP0 and interrupt pipe per external hub + "
              "every claimed endpoint; a lower bound (this host library build "
              "does not expose its per-channel ledger)");
    cJSON_AddNumberToObject(tt, "exhaustion_events", totals.exhaustion_events);
    if (totals.exhaustion_events > 0 && totals.last_exhaustion_us > 0) {
        cJSON_AddNumberToObject(tt, "last_exhaustion_ms_ago",
            (double)((esp_timer_get_time() - totals.last_exhaustion_us) / 1000));
        cJSON_AddStringToObject(tt, "last_exhaustion_busid", totals.last_exhaustion_busid);
    }
    return result(200, j);
}

static api_result_t core_restore(void)
{
    int n = hub_ctl_restore_now();
    if (n < 0) {
        return fail(503, "out of memory");
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddStringToObject(j, "action", "restore");
    cJSON_AddNumberToObject(j, "ports", n);
    return result(200, j);
}

static api_result_t core_set_device_name(const char *busid, const char *name)
{
    if (busid == NULL || strlen(busid) >= 32) {
        return fail(400, "bad busid");
    }
    esp_err_t err = device_naming_set_device_name(busid, name ? name : "");
    if (err != ESP_OK) {
        return fail(500, esp_err_to_name(err));
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddStringToObject(j, "busid", busid);
    cJSON_AddStringToObject(j, "name", name ? name : "");
    return result(200, j);
}

/* --- analog mux (response shapes follow mux_api/server.py) --- */

/* Device rescue (device_rescue.c): identity, serial line control, resets
   and a console peek, refused while the device is imported over USB/IP. */
static api_result_t core_rescue_info(const char *busid)
{
    cJSON *j = NULL;
    const int st = device_rescue_info(busid, &j);
    return result(st, j);
}

static api_result_t core_rescue_serial(const char *busid, const cJSON *args)
{
    cJSON *j = NULL;
    const int st = device_rescue_serial(busid, args, &j);
    return result(st, j);
}

static api_result_t core_rescue_reset(const char *busid, const cJSON *args)
{
    cJSON *j = NULL;
    const int st = device_rescue_reset(busid, args, &j);
    return result(st, j);
}

static api_result_t core_rescue_console(const char *busid, const cJSON *args)
{
    cJSON *j = NULL;
    const int st = device_rescue_console(busid, args, &j);
    return result(st, j);
}

static bool is_method(const char *method, const char *m);

/* Long rescue work runs as a job: 202 with its id. */
static api_result_t core_rescue_job(const char *kind, const char *busid, const cJSON *args, rescue_job_fn_t fn)
{
    int status = 0;
    char msg[128] = "";
    const uint32_t id = rescue_job_start(kind, busid, args, fn, &status, msg, sizeof(msg));
    if (id == 0) {
        return fail(status, msg);
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddNumberToObject(j, "job", id);
    char poll[48];
    snprintf(poll, sizeof(poll), "/api/rescue/jobs/%lu", (unsigned long)id);
    cJSON_AddStringToObject(j, "poll", poll);
    return result(202, j);
}

/* Rescue job kinds (MCP rescue_start_job; the device routes below start the
   same jobs) and the /api/capabilities feature each one stands for. */
static const struct {
    const char *kind;
    rescue_job_fn_t fn;
    const char *feature;
} k_job_kinds[] = {
    { "esp_flash", esp_flasher_job, "flash_esp" },
    { "esp_info", esp_flasher_info_job, NULL },
    { "picoboot_load", picoboot_flasher_job, "flash_picoboot" },
    { "picoboot_info", picoboot_info_job, NULL },
    { "samba_flash", samba_flasher_job, "flash_samba" },
    { "samba_info", samba_info_job, NULL },
    { "uf2_copy", msc_uf2_job, "flash_uf2" },
    { "msc_write", msc_write_job, "msc" },
    { "msc_read", msc_read_job, NULL },
    { "dut_flash", dut_flash_job, "dut_fs" },
};

static api_result_t core_device_release(const char *busid, const cJSON *json);

/* /api/usb/devices/{busid}/<rest> beyond the basic rescue actions. */
static api_result_t core_device_tools(const char *method, const char *busid, const char *rest,
                                      const char *query, const cJSON *json)
{
    cJSON *j = NULL;
    int st;
    const bool post = is_method(method, "POST");
    const bool get = is_method(method, "GET");
    if (post && strcmp(rest, "/flash/esp") == 0) {
        return core_rescue_job("esp_flash", busid, json, esp_flasher_job);
    }
    if (post && strcmp(rest, "/esp/info") == 0) {
        return core_rescue_job("esp_info", busid, json, esp_flasher_info_job);
    }
    if (post && strcmp(rest, "/flash/picoboot") == 0) {
        return core_rescue_job("picoboot_load", busid, json, picoboot_flasher_job);
    }
    if (post && strcmp(rest, "/picoboot/info") == 0) {
        return core_rescue_job("picoboot_info", busid, json, picoboot_info_job);
    }
    if (post && strcmp(rest, "/flash/samba") == 0) {
        return core_rescue_job("samba_flash", busid, json, samba_flasher_job);
    }
    if (post && strcmp(rest, "/samba/info") == 0) {
        return core_rescue_job("samba_info", busid, json, samba_info_job);
    }
    if (post && strcmp(rest, "/flash/uf2") == 0) {
        return core_rescue_job("uf2_copy", busid, json, msc_uf2_job);
    }
    if (post && strcmp(rest, "/msc/write") == 0) {
        return core_rescue_job("msc_write", busid, json, msc_write_job);
    }
    if (post && strcmp(rest, "/msc/read") == 0) {
        return core_rescue_job("msc_read", busid, json, msc_read_job);
    }
    if (post && strcmp(rest, "/release") == 0) {
        return core_device_release(busid, json);
    }
    if (post && strcmp(rest, "/dutflash") == 0) {
        st = dut_flash_request(busid, json, &j);
    } else if (get && strcmp(rest, "/dutflash") == 0) {
        st = dut_flash_status(busid, &j);
    } else if (post && strcmp(rest, "/dutflash/close") == 0) {
        st = dut_flash_close(busid, &j);
    } else if (get && strcmp(rest, "/msc") == 0) {
        st = msc_info(busid, &j);
    } else if (get && strcmp(rest, "/msc/list") == 0) {
        char path[256] = "/";
        query_str(query, "path", path, sizeof(path));
        st = msc_list(busid, path, &j);
    } else if (post && strcmp(rest, "/msc/delete") == 0) {
        st = msc_delete(busid, json, &j);
    } else if (post && strcmp(rest, "/msc/rename") == 0) {
        st = msc_rename(busid, json, &j);
    } else if (post && strcmp(rest, "/msc/mkdir") == 0) {
        st = msc_mkdir(busid, json, &j);
    } else if (post && strcmp(rest, "/capture/start") == 0) {
        st = capture_start(busid, json, &j);
    } else if (post && strcmp(rest, "/capture/stop") == 0) {
        st = capture_stop(busid, json, &j);
    } else if (post && strcmp(rest, "/capture/send") == 0) {
        int port = 0;
        json_int(json, "port", &port);
        st = capture_send(busid, port, json, &j);
    } else if (get && strcmp(rest, "/capture") == 0) {
        char v[24] = "";
        uint64_t since = 0;
        const int port = query_int(query, "port", 0);
        if (query_str(query, "since", v, sizeof(v))) {
            since = strtoull(v, NULL, 10);
        } else if (query_str(query, "from", v, sizeof(v))) {
            uint64_t oldest = 0, epoch = 0, total = 0;
            capture_cursor_info(busid, port, &oldest, &epoch, &total);
            since = strcmp(v, "epoch") == 0 ? epoch : strcmp(v, "tail") == 0 ? total : oldest;
        }
        query_str(query, "format", v, sizeof(v));
        st = capture_poll(busid, port, since, query_int(query, "max", 16384), strcmp(v, "lines") == 0, &j);
    } else if (get && strcmp(rest, "/monitor") == 0) {
        char v[24] = "0";
        query_str(query, "since", v, sizeof(v));
        st = serial_monitor_poll(busid, strtoull(v, NULL, 10), query_int(query, "max", 8192), &j);
    } else if (post && strcmp(rest, "/monitor/start") == 0) {
        st = serial_monitor_start(busid, json, &j);
    } else if (post && strcmp(rest, "/monitor/stop") == 0) {
        st = serial_monitor_stop_api(busid, &j);
    } else if (post && strcmp(rest, "/monitor/send") == 0) {
        st = serial_monitor_send(busid, json, &j);
    } else {
        return fail(404, "unknown device action");
    }
    return result(st, j);
}

static api_result_t core_rescue_files(const char *method, const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return is_method(method, "GET") ? result(200, blob_list_json()) : fail(405, "use GET");
    }
    if (!is_method(method, "DELETE")) {
        return fail(405, "PUT and GET of a file are streamed by the HTTP server; DELETE removes it");
    }
    const esp_err_t err = blob_delete(name);
    if (err == ESP_ERR_NOT_FOUND) {
        return fail(404, "no such staged file");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return fail(409, "in use by a running job");
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    return result(200, j);
}

static api_result_t core_capture(const char *method, const char *path, const cJSON *json)
{
    if (strcmp(path, "/api/capture") == 0 && is_method(method, "GET")) {
        return result(200, capture_status_json());
    }
    if (strcmp(path, "/api/capture/config") == 0 && is_method(method, "POST")) {
        cJSON *j = NULL;
        const int st = capture_set_config(json, &j);
        return result(st, j);
    }
    return fail(404, "GET /api/capture, POST /api/capture/config");
}

static api_result_t core_rescue_jobs(const char *method, const char *id_str)
{
    if (id_str == NULL || id_str[0] == '\0') {
        return result(200, rescue_job_list_json());
    }
    const uint32_t id = strcmp(id_str, "latest") == 0 ? 0 : (uint32_t)strtoul(id_str, NULL, 10);
    if (is_method(method, "DELETE")) {
        if (!rescue_job_cancel(id)) {
            return fail(409, "that job is not running");
        }
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", true);
        cJSON_AddStringToObject(j, "note", "cancel requested; the job stops at its next block");
        return result(200, j);
    }
    cJSON *j = rescue_job_json(id);
    return j ? result(200, j) : fail(404, "no such job");
}

static void add_stop(cJSON *o, const char *label, const char *method, const char *path, const char *body)
{
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "label", label);
    cJSON_AddStringToObject(a, "method", method);
    cJSON_AddStringToObject(a, "path", path);
    if (body != NULL) {
        cJSON_AddItemToObject(a, "body", cJSON_Parse(body));
    }
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(o, "stop");
    if (arr == NULL) {
        arr = cJSON_AddArrayToObject(o, "stop");
    }
    cJSON_AddItemToArray(arr, a);
}

/* GET /api/rescue/holders: everything that holds a device now (the running
   job, running captures, USB/IP imports), each with the request(s) that
   release it, so a page or client other than the one that started it can
   let go of the device. */
static api_result_t core_rescue_holders(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON *hs = cJSON_AddArrayToObject(j, "holders");
    char path[96];

    const uint32_t job_id = rescue_job_running_on(NULL);
    cJSON *job = job_id ? rescue_job_json(job_id) : NULL;
    if (job != NULL) {
        const cJSON *b = cJSON_GetObjectItemCaseSensitive(job, "busid");
        const cJSON *k = cJSON_GetObjectItemCaseSensitive(job, "kind");
        const char *busid = cJSON_IsString(b) ? b->valuestring : "";
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "busid", busid);
        cJSON_AddStringToObject(o, "kind", "job");
        cJSON_AddNumberToObject(o, "job", job_id);
        cJSON_AddStringToObject(o, "what", cJSON_IsString(k) ? k->valuestring : "?");
        static const char *const copy[] = { "state", "stage", "elapsed_s", "limit_s" };
        for (size_t i = 0; i < sizeof(copy) / sizeof(copy[0]); i++) {
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(job, copy[i]);
            if (v != NULL) {
                cJSON_AddItemToObject(o, copy[i], cJSON_Duplicate(v, true));
            }
        }
        if (cJSON_IsString(k) && strcmp(k->valuestring, "dut_flash") == 0) {
            snprintf(path, sizeof(path), "/api/usb/devices/%s/dutflash/close", busid);
            add_stop(o, "Close session", "POST", path, "{}");
        }
        snprintf(path, sizeof(path), "/api/rescue/jobs/%" PRIu32, job_id);
        add_stop(o, "Cancel job", "DELETE", path, NULL);
        cJSON_AddItemToArray(hs, o);
        cJSON_Delete(job);
    }

    cJSON *caps = capture_status_json();
    const cJSON *armed = cJSON_GetObjectItemCaseSensitive(caps, "armed");
    const cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(caps, "captures")) {
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c, "running"))) {
            continue;
        }
        const cJSON *b = cJSON_GetObjectItemCaseSensitive(c, "busid");
        if (!cJSON_IsString(b)) {
            continue;
        }
        bool is_armed = false;
        const cJSON *a;
        cJSON_ArrayForEach(a, armed) {
            const cJSON *ab = cJSON_GetObjectItemCaseSensitive(a, "busid");
            is_armed |= cJSON_IsString(ab) && strcmp(ab->valuestring, b->valuestring) == 0;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "busid", b->valuestring);
        cJSON_AddStringToObject(o, "kind", "capture");
        cJSON_AddStringToObject(o, "what", "serial capture");
        const cJSON *s = cJSON_GetObjectItemCaseSensitive(c, "state");
        cJSON_AddStringToObject(o, "state", cJSON_IsString(s) ? s->valuestring : "running");
        cJSON_AddBoolToObject(o, "armed", is_armed);
        snprintf(path, sizeof(path), "/api/usb/devices/%s/capture/stop", b->valuestring);
        add_stop(o, "Stop", "POST", path, "{}");
        if (is_armed) {
            add_stop(o, "Stop & disarm", "POST", path, "{\"disarm\":true}");
        }
        cJSON_AddItemToArray(hs, o);
    }
    cJSON_Delete(caps);

    usbip_backend_device_t *devs = calloc(CONFIG_USBIP_MAX_DEVICES, sizeof(*devs));
    const size_t n = devs ? usb_backend_get_devices(devs, CONFIG_USBIP_MAX_DEVICES) : 0;
    for (size_t i = 0; i < n; i++) {
        busid_owner_info_t ow;
        if (!busid_owner_get(devs[i].busid, &ow) || ow.kind != BUSID_OWNER_USBIP) {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "busid", devs[i].busid);
        cJSON_AddStringToObject(o, "kind", "usbip");
        cJSON_AddStringToObject(o, "what", "imported over USB/IP");
        cJSON_AddStringToObject(o, "peer", ow.peer);
        cJSON_AddNumberToObject(o, "for_s", (double)((esp_timer_get_time() - ow.since_us) / 1000000));
        cJSON_AddStringToObject(o, "note", "released by usbip detach on the client");
        cJSON_AddItemToArray(hs, o);
    }
    free(devs);
    cJSON_AddNumberToObject(j, "job_limit_s", RESCUE_JOB_MAX_S);
    return result(200, j);
}

/* POST /api/usb/devices/{busid}/release: let go of the device whatever holds
   it here: close its DUT flash session, cancel its job, stop its capture
   ({"disarm": true} also disarms).  USB/IP imports are the client's. */
static api_result_t core_device_release(const char *busid, const cJSON *json)
{
    cJSON *j = cJSON_CreateObject();
    cJSON *done = cJSON_AddArrayToObject(j, "released");
    cJSON *tmp = NULL;
    /* A DUT flash session closes cleanly (pending writes flushed, device
       reset); any other job is cancelled. */
    const bool closed = dut_flash_close(busid, &tmp) == 200;
    if (closed) {
        cJSON_AddItemToArray(done, cJSON_CreateString("dut_flash session"));
    }
    cJSON_Delete(tmp);
    const uint32_t id = closed ? 0 : rescue_job_running_on(busid);
    if (id != 0 && rescue_job_cancel(id)) {
        char s[32];
        snprintf(s, sizeof(s), "job %" PRIu32, id);
        cJSON_AddItemToArray(done, cJSON_CreateString(s));
    }
    if (capture_active(busid) || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "disarm"))) {
        tmp = NULL;
        cJSON *args = cJSON_CreateObject();
        cJSON_AddBoolToObject(args, "disarm", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "disarm")));
        if (capture_stop(busid, args, &tmp) == 200) {
            cJSON_AddItemToArray(done, cJSON_CreateString("capture"));
        }
        cJSON_Delete(args);
        cJSON_Delete(tmp);
    }
    busid_owner_info_t ow;
    if (busid_owner_get(busid, &ow) && ow.kind == BUSID_OWNER_USBIP) {
        cJSON_AddStringToObject(j, "still_held", "imported over USB/IP: usbip detach on the client");
    }
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddStringToObject(j, "busid", busid);
    cJSON_AddStringToObject(j, "note", "a cancelled job stops at its next block; GET /api/rescue/holders to confirm");
    return result(200, j);
}

static api_result_t core_mux_status(void)
{
    return result(200, analog_mux_describe());
}

static api_result_t core_mux_duts(void)
{
    cJSON *full = analog_mux_describe();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddItemToObject(j, "active", cJSON_DetachItemFromObject(full, "active"));
    cJSON_AddItemToObject(j, "duts", cJSON_DetachItemFromObject(full, "duts"));
    cJSON_Delete(full);
    return result(200, j);
}

static api_result_t active_result(void)
{
    char active[64];
    analog_mux_get_active(active, sizeof(active));
    cJSON *j = cJSON_CreateObject();
    add_str_or_null(j, "active", active);
    return result(200, j);
}

static api_result_t core_mux_select_dut(const char *dut)
{
    char err[160] = "";
    esp_err_t e = analog_mux_select_dut(dut, err, sizeof(err));
    if (e == ESP_ERR_NOT_FOUND) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "error", "unknown dut");
        cJSON_AddStringToObject(j, "dut", dut);
        cJSON_AddItemToObject(j, "duts", analog_mux_dut_names());
        return result(404, j);
    }
    if (e != ESP_OK) {
        return mux_fail(503, err);
    }
    return active_result();
}

static api_result_t unknown_group(const char *group)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "error", "unknown group");
    cJSON_AddStringToObject(j, "group", group);
    cJSON_AddItemToObject(j, "groups", analog_mux_group_names());
    return result(404, j);
}

static api_result_t core_mux_select_channel(const char *group, int channel)
{
    char err[160] = "";
    char active[64] = "";
    esp_err_t e = analog_mux_select_channel(group, channel, active, sizeof(active), err, sizeof(err));
    if (e == ESP_ERR_NOT_FOUND) {
        return unknown_group(group);
    }
    if (e != ESP_OK) {
        return mux_fail(e == ESP_ERR_INVALID_ARG ? 400 : 503, err);
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "active", active);
    cJSON_AddStringToObject(j, "group", group);
    cJSON_AddNumberToObject(j, "channel", channel);
    return result(200, j);
}

static api_result_t core_mux_set_channel(const char *group, int channel, bool closed)
{
    char err[160] = "";
    esp_err_t e = analog_mux_set_channel(group, channel, closed, err, sizeof(err));
    if (e == ESP_ERR_NOT_FOUND) {
        return unknown_group(group);
    }
    if (e != ESP_OK) {
        return mux_fail(e == ESP_ERR_INVALID_ARG ? 400 : 503, err);
    }
    return result(200, analog_mux_describe());
}

static api_result_t core_mux_isolate(void)
{
    char err[160] = "";
    if (analog_mux_isolate(err, sizeof(err)) != ESP_OK) {
        return mux_fail(503, err);
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNullToObject(j, "active");
    return result(200, j);
}

static api_result_t core_mux_topology_get(void)
{
    return result(200, analog_mux_get_topology());
}

static api_result_t core_mux_topology_set(const cJSON *topology)
{
    if (!cJSON_IsObject(topology)) {
        return mux_fail(400, "expected a topology JSON object");
    }
    char err[200] = "";
    bool saved = false;
    esp_err_t e = analog_mux_apply_topology(topology, &saved, err, sizeof(err));
    if (e != ESP_OK) {
        char msg[240];
        snprintf(msg, sizeof(msg), "invalid topology: %s", err);
        return mux_fail(400, msg);
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "applied", true);
    cJSON_AddBoolToObject(j, "saved", saved);
    if (saved) {
        cJSON_AddNullToObject(j, "note");
    } else {
        cJSON_AddStringToObject(j, "note", "applied but not saved: NVS write failed");
    }
    char active[64];
    analog_mux_get_active(active, sizeof(active));
    add_str_or_null(j, "active", active);
    return result(200, j);
}

static api_result_t core_mux_probe(void)
{
    cJSON *j = analog_mux_probe();
    if (j == NULL) {
        return mux_fail(400, "no control bus available");
    }
    return result(200, j);
}

/* ===========================================================================
 *  Schema
 * ======================================================================== */

typedef struct {
    const char *method;
    const char *path;
    bool auth;
    const char *desc;
    const char *body;
} api_route_doc_t;

static const api_route_doc_t k_routes[] = {
    { "GET",  "/ping",                              false, "Liveness check", NULL },
    { "GET",  "/api/info",                          false, "Bridge identity, firmware version, api_version, auth and mux bus state", NULL },
    { "GET",  "/api/capabilities",                  false, "What this bridge can do (docs/bridge-discovery-and-capabilities.md): api_version, firmware, board, network, ports, auth, limits and versioned features (reset modes, DUT file systems ...); a feature is listed only when the build has it", NULL },
    { "GET",  "/api/network",                       false, "Transports and interfaces (eth, wifi): up, ip, mac; Wi-Fi ssid, rssi, configured, state. Never the password", NULL },
    { "PUT",  "/api/network/wifi",                  true,  "Join a Wi-Fi network now, without a reboot (Ethernet stays up). The credentials are stored in NVS once the station gets an address, or at once with store: true. Waits up to wait_ms (default and max 10000) for the address: 200 connected, 202 still connecting. An empty password joins an open network. POST also accepted", "{\"ssid\":str,\"password\":str?,\"store\":bool?,\"wait_ms\":int?}" },
    { "DELETE", "/api/network/wifi",                true,  "Forget the Wi-Fi credentials (NVS too) and disconnect", NULL },
    { "GET",  "/api/schema",                        false, "This endpoint list", NULL },
    { "GET",  "/api/usb/devices",                   false, "Exported USB devices with descriptors, topology and port power state. busid is the Linux style port path used by usbip", NULL },
    { "GET",  "/api/usb/devices/{busid}",           false, "One device; 404 when absent (presence check)", NULL },
    { "GET",  "/api/usb/devices/{busid}/rescue",    false, "Interrogate a device for rescue (no bus traffic): identity (serial_number = the iSerial string or null), interfaces with numbers and endpoints, owner (imported over USB/IP?), port power (read-only), serial function (cdc_acm, cp210x, ch34x, ftdi), RP2 reset/PICOBOOT interfaces, the reset modes that apply and the bridge's last-set baud/DTR/RTS", NULL },
    { "POST", "/api/usb/devices/{busid}/serial",    true,  "Set line coding and/or DTR and RTS on the device's serial function (lines not given keep their last-set state). 409 while imported over USB/IP", "{\"baud\":int?,\"dtr\":bool?,\"rts\":bool?,\"port\":int?,\"allow_1200\":bool?}" },
    { "POST", "/api/usb/devices/{busid}/reset",     true,  "Run a reset sequence: esp_bootloader, esp_hard_reset (classic DTR/RTS circuit), esp_usb_jtag_bootloader, esp_usb_jtag_reset (303a:1001), touch_1200, rp_bootsel, rp_reboot (RP2 reset interface), picoboot_reboot, picoboot_bootsel (RP2350), or bootloader (pick the fitting one). Waits up to wait_ms for the device to re-enumerate and reports what came back. 409 while imported over USB/IP", "{\"mode\":str,\"port\":int?,\"wait_ms\":int?}" },
    { "POST", "/api/usb/devices/{busid}/console",   true,  "Console peek: set serial_baud (default 115200), optionally DTR/RTS, send pokes (enter, ctrl_c, ctrl_d or {hex}) settle_s apart, capture capture_s (max 10; the whole call max 12 s, blocking the bridge API) and return text (sanitised UTF-8), data_hex and the serial_probe classification. 409 while imported over USB/IP", "{\"serial_baud\":int?,\"pokes\":[str|{\"hex\":str}]?,\"settle_s\":num?,\"capture_s\":num?,\"dtr\":bool?,\"rts\":bool?,\"max_bytes\":int?,\"port\":int?}" },
    { "POST", "/api/usb/devices/{busid}/flash/esp", true,  "Job: flash an ESP with esp-serial-flasher (stub, MD5 verify) over its USB serial: esptool write-flash without host tools. 202 {job}", "{\"images\":[{\"file\":str,\"offset\":int|str}],\"before\":\"default_reset|usb_reset|no_reset\",\"after\":\"hard_reset|no_reset\",\"baud\":int?,\"erase_all\":bool?,\"verify\":bool?,\"stub\":bool?}" },
    { "POST", "/api/usb/devices/{busid}/esp/info",  true,  "Job: connect to the ESP bootloader and report chip, MAC, flash size and security info", "{\"before\":str?,\"after\":str?}" },
    { "POST", "/api/usb/devices/{busid}/flash/picoboot", true, "Job: picotool load: write a UF2 or raw .bin to an RP2040/RP2350 over PICOBOOT (entering BOOTSEL first), verify by read-back, reboot", "{\"file\":str,\"address\":str?,\"verify\":bool?,\"reboot\":bool?,\"enter_bootsel\":bool?,\"force_family\":bool?}" },
    { "POST", "/api/usb/devices/{busid}/picoboot/info", true, "Job: enter BOOTSEL if needed and report the RP2 chip", "{\"enter_bootsel\":bool?,\"reboot\":bool?}" },
    { "POST", "/api/usb/devices/{busid}/flash/samba", true, "Job: bossac: write a .bin to a SAMD21/SAMD51 through its SAM-BA bootloader (Arduino X/Y/Z extensions), CRC16 verify, reset; 1200-baud touch first if needed", "{\"file\":str,\"offset\":str?,\"erase\":bool?,\"verify\":bool?,\"reset\":bool?}" },
    { "POST", "/api/usb/devices/{busid}/samba/info", true, "Job: connect to SAM-BA (1200-baud touch first if needed) and report the bootloader, chip and flash; reset: true returns the board to its app", "{\"reset\":bool?}" },
    { "POST", "/api/usb/devices/{busid}/flash/uf2", true,  "Job: copy a staged UF2 to the device's UF2 bootloader drive as a file, like a PC (method auto: raw sectors if that fails; file; raw) (entering the bootloader first if it has no drive or an app's drive without INFO_UF2.TXT); a reboot after the last block counts as success", "{\"file\":str,\"path\":str?,\"method\":\"auto|file|raw\"?,\"enter_bootloader\":bool?}" },
    { "POST", "/api/usb/devices/{busid}/monitor/start", true, "Start a serial monitor (64 KB ring, follows the device through resets); line and reset actions still work meanwhile", "{\"serial_baud\":int?,\"dtr\":bool?,\"rts\":bool?,\"port\":int?}" },
    { "GET",  "/api/usb/devices/{busid}/monitor",   false, "Monitor output since a cursor: ?since=<next from the last call>&max=<bytes>", NULL },
    { "POST", "/api/usb/devices/{busid}/monitor/send", true, "Send text or hex to the monitored device", "{\"text\":str}|{\"hex\":str}" },
    { "POST", "/api/usb/devices/{busid}/monitor/stop", true, "Stop the monitor", NULL },
    { "GET",  "/api/usb/devices/{busid}/msc",       false, "Mass storage: inquiry, capacity, FAT type, free space, root listing, whether it is a UF2 bootloader", NULL },
    { "GET",  "/api/usb/devices/{busid}/msc/list",  false, "List a directory: ?path=/lib", NULL },
    { "POST", "/api/usb/devices/{busid}/msc/delete", true, "Delete a file or empty directory", "{\"path\":str}" },
    { "POST", "/api/usb/devices/{busid}/msc/rename", true, "Rename / move", "{\"from\":str,\"to\":str}" },
    { "POST", "/api/usb/devices/{busid}/msc/mkdir", true,  "Make a directory", "{\"path\":str}" },
    { "POST", "/api/usb/devices/{busid}/msc/write", true,  "Job: copy a staged file to the volume (replaces an existing file)", "{\"file\":str,\"path\":str}" },
    { "POST", "/api/usb/devices/{busid}/msc/read",  true,  "Job: copy a file off the volume into the staged files (then GET /api/rescue/files/{name})", "{\"path\":str,\"file\":str?}" },
    { "POST", "/api/usb/devices/{busid}/dutflash",  true,  "Job: DUT flash session through the bootloader (ESP, RP2040/RP2350 PICOBOOT, SAMD SAM-BA read-only): map partitions/regions (result.regions), then ops: partitions, read_region {part|offset+size, file?}, fs_info/list {part, path}, read {part, path, file?}, write {part, path, file|text}, delete/mkdir {part, path}, rename {part, from, to}, format {part, fs: littlefs|fat|spiffs, confirm?}. FAT, LittleFS, SPIFFS; writes are read-modify-erase-write of 4 KB sectors. With keep_open_s the session waits for more: another POST adds ops to it. 202 {job}", "{\"ops\":[{\"op\":str,\"part\":str|int?,\"path\":str?,\"file\":str?}],\"platform\":\"auto|esp|rp2|samd\"?,\"keep_open_s\":int?,\"after\":\"hard_reset|no_reset\"?,\"flash_size\":str?}" },
    { "GET",  "/api/usb/devices/{busid}/dutflash",  false, "The open DUT flash session on the device: {open, job}", NULL },
    { "POST", "/api/usb/devices/{busid}/dutflash/close", true, "End the DUT flash session (writes are already on the flash; the device is reset unless after: no_reset)", NULL },
    { "POST", "/api/usb/devices/{busid}/capture/start", true, "Capture every serial function of the device into PSRAM (timestamped, follows resets / re-enumeration / port-count changes); arm: keep capturing it whenever it is present, across bridge reboots. baud: a rate, a list tried in order, or \"auto\" (UART bridges); dtr defaults on for native USB CDC (not UART bridges or the ESP USB-Serial/JTAG). source gpio_uart is planned (501)", "{\"size\":int?,\"baud\":int|[int|\"auto\"]|\"auto\",\"dtr\":bool?,\"rts\":bool?,\"arm\":bool?}" },
    { "POST", "/api/usb/devices/{busid}/capture/stop", true, "Stop capturing (the buffer stays readable); disarm stops re-arming, clear frees the buffer", "{\"disarm\":bool?,\"clear\":bool?}" },
    { "GET",  "/api/usb/devices/{busid}/capture",   false, "Captured output of one port: ?port=0&since=<cursor> | from=epoch|oldest|tail &max=<bytes>&format=lines (uptime timestamps). Answers carry next, epoch, epoch_cursor, uptime_ms", NULL },
    { "POST", "/api/usb/devices/{busid}/capture/send", true, "Send text or hex to one captured port", "{\"port\":int?,\"text\":str}|{\"hex\":str}" },
    { "GET",  "/api/rescue/capture/{busid}/{port}.log", false, "Download a port's whole buffer as a text file with [uptime] line timestamps", NULL },
    { "GET",  "/api/capture",                       false, "Capture limits (total_limit, default_size), PSRAM reserved, armed ports and every capture with its ports and cursors; live streams on TCP 3241", NULL },
    { "POST", "/api/capture/config",                true,  "Capture budget, saved in NVS: total_limit (all captures together, up to 12 MB) and default_size per port", "{\"total_limit\":int?,\"default_size\":int?}" },
    { "GET",  "/api/rescue/files",                  false, "Staged files (PSRAM, lost on reboot) with size and space left", NULL },
    { "PUT",  "/api/rescue/files/{name}",           true,  "Upload a staged file: the raw body (streamed into PSRAM; POST also accepted). Names [A-Za-z0-9._-]", "raw bytes" },
    { "GET",  "/api/rescue/files/{name}",           false, "Download a staged file", NULL },
    { "DELETE", "/api/rescue/files/{name}",         true,  "Delete a staged file", NULL },
    { "GET",  "/api/rescue/jobs",                   false, "Recent rescue jobs (one runs at a time)", NULL },
    { "GET",  "/api/rescue/jobs/{id}",              false, "A job's state, progress, stage, log, result and error ({id} = latest for the newest)", NULL },
    { "DELETE", "/api/rescue/jobs/{id}",            true,  "Cancel the running job", NULL },
    { "GET",  "/api/rescue/holders",                false, "What holds a device now (the running job, running captures, USB/IP imports), each with the request(s) that release it: {holders:[{busid, kind: job|capture|usbip, what, state, stop:[{label, method, path, body?}]}], job_limit_s}", NULL },
    { "POST", "/api/usb/devices/{busid}/release",   true,  "Let go of a device whatever started it: close its DUT flash session, cancel its job, stop its capture (disarm: also disarm). USB/IP imports are the client's to detach", "{\"disarm\":bool?}" },
    { "POST", "/api/devices/{busid}/name",          true,  "Set a friendly name for the device on a port", "{\"name\":str}" },
    { "GET",  "/api/usb/hubs",                      false, "Hubs with power switching mode and per-port status", NULL },
    { "GET",  "/api/usb/enum_events",               false, "Recent enumerations (newest first): starts, completions and failures with reason, stage, diagnosis (rebooting vs slow/stalled) and what happened next", NULL },
    { "GET",  "/api/usb/claims",                    false, "Interface-claim and host-channel-pressure accounting per device, with claim-failure history and exhaustion events (estimated channel totals; the 16-channel HCD table itself is not exposed by the host library)", NULL },
    { "POST", "/api/usb/debug",                     false, "Log the USB host's hub, port and enumeration state to the console (diagnostics)", NULL },
    { "POST", "/api/usb/debug_dump_enum",           true,  "Arm the EPDUMP register dump for address 0, power-cycle the port so it re-enumerates, and dump the enumeration's first control reads to the console (diagnostics, issue #37)", "{\"busid\":str,\"all\":bool,\"count\":int(1-64, default 16)}" },
    { "POST", "/api/usb/debug_ctrl",                true,  "Submit one control transfer to a connected device through the same backend path as a forwarded usbip EP0 request (diagnostics, issue #37). OUT requests must have wlength 0", "{\"busid\":str,\"bm\":int,\"breq\":int,\"wvalue\":int,\"windex\":int,\"wlength\":int,\"dump\":bool,\"count\":int(1-64, default 4)}" },
    { "GET",  "/api/ports/hwstatus",                false, "Hardware-true hub port status: a real hub-class GetPortStatus to every downstream port of every managed hub, decoded (power, connection, enable, over-current), with the bridge's claimed power and hw_mismatch", NULL },
    { "GET",  "/api/ports/events",                  false, "Port state transition log, newest first: power, connection, enable and over-current changes with attribution (api = commanded here, via restore for the saved-state policy; device = replug or drop; external = power changed without a command, e.g. a hand-toggled switch) and mismatch, plus external_power_changes and device_replugs counters since boot", NULL },
    { "GET",  "/api/ports",                         false, "Every downstream hub port (alias /ports)", NULL },
    { "GET",  "/api/ports/{port}",                  false, "One port, e.g. 1-1.3", NULL },
    { "POST", "/api/ports/{port}/on",               true,  "Power a hub port on (SetPortFeature PORT_POWER)", "{\"force\":bool}" },
    { "POST", "/api/ports/{port}/off",              true,  "Power a hub port off. Refused for ganged/non-switching hubs (while enforced) and ports leading to hubs unless force. The state is saved per port path", "{\"force\":bool}" },
    { "POST", "/api/ports/{port}/cycle",            true,  "Power off, wait off_ms (default 1000), power on; runs in the background", "{\"off_ms\":int,\"force\":bool}" },
    { "POST", "/api/ports/off",                     true,  "Power off every per-port switched port that does not lead to a hub", NULL },
    { "POST", "/api/ports/restore",                 true,  "Apply the saved power state to every port whose power does not match it", NULL },
    { "GET",  "/api/settings",                      false, "Port power and enumeration timeout settings", NULL },
    { "POST", "/api/settings/enum_timeout",         true,  "Enumeration control transfer timeout: global (no path) or an override for a hub/port path and everything behind it; timeout_ms 0 = none, null removes an override", "{\"path\":str?,\"timeout_ms\":int|null}" },
    { "POST", "/api/settings",                      true,  "enforce_per_port_switching: refuse ganged/non-switching hubs unless forced; restore_port_power: keep ports switched off when their hub re-enumerates; enum_timeout_ms: default enumeration timeout", "{\"enforce_per_port_switching\":bool,\"restore_port_power\":bool,\"enum_timeout_ms\":int}" },
    { "POST", "/api/ports/on",                      true,  "Power on every per-port switched port", NULL },
    { "GET",  "/api/status",                        false, "Analog mux state {active, manual, duts, groups}", NULL },
    { "GET",  "/api/duts",                          false, "Analog mux DUTs {active, duts}", NULL },
    { "POST", "/api/select",                        true,  "Connect one DUT to the shared I2C chain (break-before-make)", "{\"dut\":str}" },
    { "POST", "/api/select/{dut}",                  true,  "Same as /api/select (GET also accepted)", NULL },
    { "POST", "/api/isolate",                       true,  "Open every mux channel", NULL },
    { "POST", "/api/groups/{group}/select/{channel}", true, "Exclusively connect group+channel (break-before-make)", NULL },
    { "POST", "/api/groups/{group}/channel",        true,  "Manually open/close one route (marks the mux manual)", "{\"channel\":int,\"closed\":bool}" },
    { "GET",  "/api/topology",                      false, "Mux topology", NULL },
    { "PUT",  "/api/topology",                      true,  "Validate, apply and save a mux topology (POST also accepted)", "{\"groups\":[...],\"duts\":[...]}" },
    { "GET",  "/api/probe",                         false, "Scan the mux control bus", NULL },
    { "GET",  "/api/auth",                          false, "Whether a token is required for mutating requests", NULL },
    { "POST", "/api/auth/token",                    true,  "Set the API token (empty string clears it)", "{\"token\":str}" },
    { "POST", "/api/reboot",                        true,  "Restart the bridge", NULL },
    { "GET",  "/api/ota",                           false, "Firmware update status: running, boot and next OTA slot, each app slot's state (valid, pending_verify, invalid, aborted) and image (version, date, ELF SHA-256), the last rolled-back slot, the new image's confirm check and the last upload", NULL },
    { "PUT",  "/api/ota",                           true,  "Firmware update: the raw app .bin as the body (Content-Length required), streamed into the next OTA slot; the image header, chip, project and size are checked before writing and the whole image is verified after. It boots on the next restart (?reboot=1 restarts now), pending verification: it confirms itself once Ethernet and HTTP are up, or rolls back. ?force=1 accepts another project's app. POST also accepted", "raw bytes" },
    { "POST", "/api/ota/boot",                      true,  "Boot another app slot next (its image is verified first; an OTA slot boots pending verification, factory clears otadata)", "{\"partition\":\"factory|ota_0|ota_1\",\"reboot\":bool?}" },
    { "POST", "/mcp",                               true,  "Model Context Protocol endpoint (Streamable HTTP, JSON responses, stateless)", "JSON-RPC 2.0" },
};

static api_result_t core_schema(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "name", "esp-usbip-bridge");
    cJSON_AddStringToObject(j, "version", HARNESS_FW_VERSION);
    cJSON_AddStringToObject(j, "auth", "When a token is set, every non-GET request (and /api/select/*) needs Authorization: Bearer <token>");
    cJSON_AddStringToObject(j, "errors", "Failures return a 4xx/5xx status with {\"error\":msg} (and \"ok\":false on device and port endpoints)");
    cJSON *arr = cJSON_AddArrayToObject(j, "endpoints");
    for (size_t i = 0; i < sizeof(k_routes) / sizeof(k_routes[0]); i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "method", k_routes[i].method);
        cJSON_AddStringToObject(o, "path", k_routes[i].path);
        cJSON_AddBoolToObject(o, "auth", k_routes[i].auth);
        cJSON_AddStringToObject(o, "description", k_routes[i].desc);
        if (k_routes[i].body) {
            cJSON_AddStringToObject(o, "body", k_routes[i].body);
        }
        cJSON_AddItemToArray(arr, o);
    }
    return result(200, j);
}

/* ===========================================================================
 *  MCP (Streamable HTTP, stateless, JSON responses)
 * ======================================================================== */

typedef struct {
    const char *name;
    const char *description;
    const char *input_schema;
} mcp_tool_t;

#define NO_ARGS "{\"type\":\"object\",\"properties\":{}}"

static const mcp_tool_t k_tools[] = {
    { "get_bridge_info", "Bridge identity, firmware version, uptime and whether auth is required.", NO_ARGS },
    { "get_ota_status",
      "Firmware update (OTA) status: running / boot / next slot, each app slot's state and image version, the "
      "last rolled-back slot and the new image's confirm check. Upload new firmware with PUT /api/ota (raw .bin).",
      NO_ARGS },
    { "list_usb_devices",
      "List USB devices exported over USB/IP with VID/PID, strings, serial, speed, topology "
      "(hub and port) and the power state of the port they are on. busid is the Linux style port "
      "path to pass to `usbip attach -r <bridge> -b <busid>`.", NO_ARGS },
    { "get_usb_device", "Details of one exported USB device. Errors if no device is on that port.",
      "{\"type\":\"object\",\"properties\":{\"busid\":{\"type\":\"string\",\"description\":\"Port path, e.g. 1-1.3\"}},\"required\":[\"busid\"]}" },
    { "rescue_device_info",
      "Interrogate one USB device for rescue without touching the bus: identity, interfaces, whether it is "
      "imported over USB/IP (rescue actions are refused then), its port power (read-only), its serial function "
      "(cdc_acm / cp210x / ch34x / ftdi), RP2040/RP2350 reset or PICOBOOT interfaces and the reset modes that apply.",
      "{\"type\":\"object\",\"properties\":{\"busid\":{\"type\":\"string\"}},\"required\":[\"busid\"]}" },
    { "rescue_serial_lines",
      "Set the baud rate and/or DTR and RTS on a device's serial function (CDC-ACM, CP210x, CH34x, FTDI). "
      "DTR/RTS not given keep their last-set state.",
      "{\"type\":\"object\",\"properties\":{\"busid\":{\"type\":\"string\"},\"baud\":{\"type\":\"integer\"},"
      "\"dtr\":{\"type\":\"boolean\"},\"rts\":{\"type\":\"boolean\"},\"port\":{\"type\":\"integer\"}},\"required\":[\"busid\"]}" },
    { "rescue_reset_device",
      "Reset a device or put it into its bootloader without host tools: esp_bootloader / esp_hard_reset (DTR/RTS "
      "auto-reset circuit behind a UART bridge), esp_usb_jtag_bootloader / esp_usb_jtag_reset (ESP USB-Serial/JTAG), "
      "touch_1200 (UF2 / SAM-BA / Arduino), rp_bootsel / rp_reboot (RP2 reset interface), picoboot_reboot / "
      "picoboot_bootsel (device in BOOTSEL), or bootloader to pick the fitting one. Reports whether the device "
      "re-enumerated and as what. Never changes port power.",
      "{\"type\":\"object\",\"properties\":{\"busid\":{\"type\":\"string\"},\"mode\":{\"type\":\"string\"},"
      "\"port\":{\"type\":\"integer\"},\"wait_ms\":{\"type\":\"integer\"}},\"required\":[\"busid\",\"mode\"]}" },
    { "rescue_console_peek",
      "Open a device's serial console on the bridge, send optional pokes (enter, ctrl_c, ctrl_d or {hex}) and "
      "capture the output for capture_s seconds (max 10). Returns the text, hex and a classification (silent, "
      "circuitpython_repl, micropython_repl, circuitpython_safe_mode, boot_banner_detected, other).",
      "{\"type\":\"object\",\"properties\":{\"busid\":{\"type\":\"string\"},\"serial_baud\":{\"type\":\"integer\",\"default\":115200},"
      "\"pokes\":{\"type\":\"array\"},\"settle_s\":{\"type\":\"number\",\"default\":1},"
      "\"capture_s\":{\"type\":\"number\",\"default\":3},\"dtr\":{\"type\":\"boolean\"},\"rts\":{\"type\":\"boolean\"},"
      "\"max_bytes\":{\"type\":\"integer\"},\"port\":{\"type\":\"integer\"}},\"required\":[\"busid\"]}" },
    { "rescue_start_job",
      "Start a device rescue job and return its id (poll with rescue_get_job). kind: esp_flash "
      "{images:[{file,offset}], before, after, baud, erase_all}, esp_info, picoboot_load {file, address?}, "
      "picoboot_info, samba_flash {file, offset?}, samba_info, uf2_copy {file}, msc_write {file, path}, "
      "msc_read {path, file?}, dut_flash {ops, platform?, keep_open_s?} (see rescue_dut_flash). Files are staged "
      "first with PUT /api/rescue/files/<name>.",
      "{\"type\":\"object\",\"properties\":{\"busid\":{\"type\":\"string\"},\"kind\":{\"type\":\"string\"},"
      "\"args\":{\"type\":\"object\"}},\"required\":[\"busid\",\"kind\"]}" },
    { "rescue_dut_flash",
      "A device's own flash through its bootloader (ESP serial loader, RP2 PICOBOOT, SAMD SAM-BA read-only): "
      "starts a session job (or adds ops to the open one) that maps partitions/regions and runs ops: partitions, "
      "read_region {part|offset+size, file?} (to a staged file), fs_info/list {part, path}, read {part, path, file?}, "
      "write {part, path, file|text}, delete/mkdir {part, path}, rename {part, from, to}, format {part, fs, confirm?}. "
      "File systems: FAT, LittleFS, SPIFFS. Poll the returned job with rescue_get_job (result.regions, result.ops). "
      "action close ends the session.",
      "{\"type\":\"object\",\"properties\":{\"busid\":{\"type\":\"string\"},\"action\":{\"type\":\"string\","
      "\"description\":\"run (default) or close\"},\"ops\":{\"type\":\"array\"},\"platform\":{\"type\":\"string\"},"
      "\"keep_open_s\":{\"type\":\"integer\"},\"after\":{\"type\":\"string\"},\"flash_size\":{\"type\":\"string\"}},"
      "\"required\":[\"busid\"]}" },
    { "rescue_get_job", "A rescue job's state, progress, log and result (id 0 = the latest).",
      "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"}}}" },
    { "rescue_list_files", "Files staged on the bridge for flashing / copying (PSRAM).", NO_ARGS },
    { "rescue_monitor",
      "Serial monitor: action start {serial_baud, dtr?, rts?}, poll {since}, send {text|hex}, or stop.",
      "{\"type\":\"object\",\"properties\":{\"busid\":{\"type\":\"string\"},\"action\":{\"type\":\"string\"},"
      "\"since\":{\"type\":\"integer\"},\"serial_baud\":{\"type\":\"integer\"},\"text\":{\"type\":\"string\"},"
      "\"hex\":{\"type\":\"string\"},\"dtr\":{\"type\":\"boolean\"},\"rts\":{\"type\":\"boolean\"}},"
      "\"required\":[\"busid\",\"action\"]}" },
    { "rescue_msc",
      "A device's mass storage (FAT): action info, list {path}, delete {path}, rename {from, to}, mkdir {path}.",
      "{\"type\":\"object\",\"properties\":{\"busid\":{\"type\":\"string\"},\"action\":{\"type\":\"string\"},"
      "\"path\":{\"type\":\"string\"},\"from\":{\"type\":\"string\"},\"to\":{\"type\":\"string\"}},"
      "\"required\":[\"busid\",\"action\"]}" },
    { "list_hubs",
      "List USB hubs with their power switching mode (per-port, ganged or none) and the state of every "
      "port: power, connection, speed, over-current and what is attached.", NO_ARGS },
    { "set_port_power",
      "Switch a hub port's VBUS on or off (per-port power switching). Powering off disconnects the "
      "device like unplugging it. Refused for hubs without per-port switching, and when powering off a "
      "port that leads to another hub, unless force is true.",
      "{\"type\":\"object\",\"properties\":{\"port\":{\"type\":\"string\",\"description\":\"Port path, e.g. 1-1.3\"},"
      "\"on\":{\"type\":\"boolean\"},\"force\":{\"type\":\"boolean\",\"default\":false}},\"required\":[\"port\",\"on\"]}" },
    { "power_cycle_port",
      "Power a hub port off, wait off_ms, and power it back on (runs in the background; poll "
      "list_usb_devices to see the device come back).",
      "{\"type\":\"object\",\"properties\":{\"port\":{\"type\":\"string\"},\"off_ms\":{\"type\":\"integer\",\"default\":1000},"
      "\"force\":{\"type\":\"boolean\",\"default\":false}},\"required\":[\"port\"]}" },
    { "get_port_power_settings",
      "Port power settings: enforce_per_port_switching (refuse hubs without per-port power switching "
      "unless forced) and restore_port_power (keep ports that were switched off powered off when their "
      "hub re-enumerates after a reset, power loss or bridge reboot).", NO_ARGS },
    { "set_port_power_settings", "Change the port power settings (omitted fields are unchanged).",
      "{\"type\":\"object\",\"properties\":{\"enforce_per_port_switching\":{\"type\":\"boolean\"},"
      "\"restore_port_power\":{\"type\":\"boolean\"}}}" },
    { "get_enum_events",
      "Recent USB enumerations with failures diagnosed: a timeout means the device stayed connected but did "
      "not answer (slow or stalled); a disconnect or transfer error means it reset or rebooted. Each failure "
      "says whether the port enumerated again afterwards and how soon.", NO_ARGS },
    { "get_usb_claims",
      "Interface-claim and host-channel-pressure accounting: per exported device the claimed interfaces, "
      "their endpoints (each holds one of the 16 host channels until released), in-flight transfers and "
      "claim-failure history, plus totals with channel-exhaustion events. Totals are a documented lower "
      "bound, not the HCD's own channel table.", NO_ARGS },
    { "set_enum_timeout",
      "Set how long the USB host waits for a device to answer an enumeration control request before "
      "disabling its port (so one unresponsive device cannot stop others enumerating). Without path: the "
      "global default. With a hub or port path: an override for that device and everything behind it. "
      "timeout_ms 0 = no timeout; null removes an override.",
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
      "\"timeout_ms\":{\"type\":[\"integer\",\"null\"]}},\"required\":[\"timeout_ms\"]}" },
    { "restore_port_power",
      "Apply the saved power state to every hub port whose actual power does not match it (list_hubs "
      "shows desired and mismatch per port).", NO_ARGS },
    { "set_device_name", "Give the device on a port a friendly name (stored per port path).",
      "{\"type\":\"object\",\"properties\":{\"busid\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"}},\"required\":[\"busid\",\"name\"]}" },
    { "mux_status",
      "Analog I2C mux state: which DUT is connected to the shared I2C chain, groups and channel states.", NO_ARGS },
    { "mux_select",
      "Connect exactly one DUT to the shared I2C chain, break-before-make. Give either dut (a name "
      "from the topology) or group and channel.",
      "{\"type\":\"object\",\"properties\":{\"dut\":{\"type\":\"string\"},\"group\":{\"type\":\"string\"},\"channel\":{\"type\":\"integer\"}}}" },
    { "mux_isolate", "Open every analog mux channel so no DUT is connected.", NO_ARGS },
    { "mux_set_channel",
      "Manually open or close one mux route without touching the others (bring-up only; can connect "
      "several DUTs at once).",
      "{\"type\":\"object\",\"properties\":{\"group\":{\"type\":\"string\"},\"channel\":{\"type\":\"integer\"},"
      "\"closed\":{\"type\":\"boolean\"}},\"required\":[\"group\",\"channel\",\"closed\"]}" },
    { "mux_get_topology", "Get the analog mux topology (groups of ADG729/ADG728 switches and DUT names).", NO_ARGS },
    { "mux_set_topology",
      "Validate, apply and save a new analog mux topology. Groups: {name, type: adg729 (address) or "
      "adg728_pair (sda_address, scl_address)}; duts: {name, group, channel}.",
      "{\"type\":\"object\",\"properties\":{\"topology\":{\"type\":\"object\"}},\"required\":[\"topology\"]}" },
};

static cJSON *jsonrpc_response(const cJSON *id)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    cJSON_AddItemToObject(r, "id", id ? cJSON_Duplicate(id, true) : cJSON_CreateNull());
    return r;
}

static cJSON *jsonrpc_error(const cJSON *id, int code, const char *msg)
{
    cJSON *r = jsonrpc_response(id);
    cJSON *e = cJSON_AddObjectToObject(r, "error");
    cJSON_AddNumberToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", msg);
    return r;
}

static api_result_t mcp_call_tool(const char *name, const cJSON *args)
{
    if (strcmp(name, "get_bridge_info") == 0) {
        return core_info();
    }
    if (strcmp(name, "get_ota_status") == 0) {
        return result(200, ota_update_status_json());
    }
    if (strcmp(name, "list_usb_devices") == 0) {
        return core_devices();
    }
    if (strcmp(name, "get_usb_device") == 0) {
        const char *busid = json_string(args, "busid");
        return busid ? core_device(busid) : fail(400, "busid is required");
    }
    if (strcmp(name, "list_hubs") == 0) {
        return core_hubs();
    }
    if (strcmp(name, "rescue_device_info") == 0) {
        return core_rescue_info(json_string(args, "busid"));
    }
    if (strcmp(name, "rescue_serial_lines") == 0) {
        return core_rescue_serial(json_string(args, "busid"), args);
    }
    if (strcmp(name, "rescue_reset_device") == 0) {
        return core_rescue_reset(json_string(args, "busid"), args);
    }
    if (strcmp(name, "rescue_console_peek") == 0) {
        return core_rescue_console(json_string(args, "busid"), args);
    }
    if (strcmp(name, "rescue_start_job") == 0) {
        const char *kind = json_string(args, "kind");
        for (size_t i = 0; kind && i < sizeof(k_job_kinds) / sizeof(k_job_kinds[0]); i++) {
            if (strcmp(kind, k_job_kinds[i].kind) == 0) {
                return core_rescue_job(kind, json_string(args, "busid"),
                                       cJSON_GetObjectItemCaseSensitive(args, "args"), k_job_kinds[i].fn);
            }
        }
        return fail(400, "unknown kind");
    }
    if (strcmp(name, "rescue_dut_flash") == 0) {
        const char *busid = json_string(args, "busid");
        const char *action = json_string(args, "action");
        cJSON *j = NULL;
        const int st = (action && strcmp(action, "close") == 0) ? dut_flash_close(busid, &j)
                                                                : dut_flash_request(busid, args, &j);
        return result(st, j);
    }
    if (strcmp(name, "rescue_get_job") == 0) {
        int id = 0;
        json_int(args, "id", &id);
        cJSON *j = rescue_job_json((uint32_t)id);
        return j ? result(200, j) : fail(404, "no such job");
    }
    if (strcmp(name, "rescue_list_files") == 0) {
        return result(200, blob_list_json());
    }
    if (strcmp(name, "rescue_monitor") == 0 || strcmp(name, "rescue_msc") == 0) {
        const char *busid = json_string(args, "busid");
        const char *action = json_string(args, "action");
        if (busid == NULL || action == NULL) {
            return fail(400, "busid and action required");
        }
        char rest[32];
        int since = 0;
        json_int(args, "since", &since);
        char query[48];
        snprintf(query, sizeof(query), "since=%d&path=", since);
        const char *path = json_string(args, "path");
        if (strcmp(name, "rescue_monitor") == 0) {
            snprintf(rest, sizeof(rest), strcmp(action, "poll") == 0 ? "/monitor" : "/monitor/%s", action);
            return core_device_tools(strcmp(action, "poll") == 0 ? "GET" : "POST", busid, rest, query, args);
        }
        if (strcmp(action, "list") == 0) {
            cJSON *j = NULL;
            const int st = msc_list(busid, path ? path : "/", &j);
            return result(st, j);
        }
        snprintf(rest, sizeof(rest), strcmp(action, "info") == 0 ? "/msc" : "/msc/%s", action);
        return core_device_tools(strcmp(action, "info") == 0 ? "GET" : "POST", busid, rest, NULL, args);
    }
    if (strcmp(name, "set_port_power") == 0) {
        const char *port = json_string(args, "port");
        const cJSON *on = cJSON_GetObjectItemCaseSensitive(args, "on");
        if (port == NULL || on == NULL) {
            return fail(400, "port and on are required");
        }
        return core_port_power(port, json_bool(args, "on", false), json_bool(args, "force", false));
    }
    if (strcmp(name, "power_cycle_port") == 0) {
        const char *port = json_string(args, "port");
        int off_ms = API_DEFAULT_OFF_MS;
        json_int(args, "off_ms", &off_ms);
        return port ? core_port_cycle(port, off_ms, json_bool(args, "force", false))
                    : fail(400, "port is required");
    }
    if (strcmp(name, "get_port_power_settings") == 0) {
        return core_get_settings();
    }
    if (strcmp(name, "set_port_power_settings") == 0) {
        return core_set_settings(args);
    }
    if (strcmp(name, "get_enum_events") == 0) {
        return core_enum_events();
    }
    if (strcmp(name, "get_usb_claims") == 0) {
        return core_usb_claims();
    }
    if (strcmp(name, "set_enum_timeout") == 0) {
        return core_set_enum_timeout(args);
    }
    if (strcmp(name, "restore_port_power") == 0) {
        return core_restore();
    }
    if (strcmp(name, "set_device_name") == 0) {
        return core_set_device_name(json_string(args, "busid"), json_string(args, "name"));
    }
    if (strcmp(name, "mux_status") == 0) {
        return core_mux_status();
    }
    if (strcmp(name, "mux_select") == 0) {
        const char *dut = json_string(args, "dut");
        if (dut) {
            return core_mux_select_dut(dut);
        }
        const char *group = json_string(args, "group");
        int channel;
        if (group && json_int(args, "channel", &channel)) {
            return core_mux_select_channel(group, channel);
        }
        return mux_fail(400, "give dut, or group and channel");
    }
    if (strcmp(name, "mux_isolate") == 0) {
        return core_mux_isolate();
    }
    if (strcmp(name, "mux_set_channel") == 0) {
        const char *group = json_string(args, "group");
        int channel;
        if (group == NULL || !json_int(args, "channel", &channel) ||
                cJSON_GetObjectItemCaseSensitive(args, "closed") == NULL) {
            return mux_fail(400, "group, channel and closed are required");
        }
        return core_mux_set_channel(group, channel, json_bool(args, "closed", false));
    }
    if (strcmp(name, "mux_get_topology") == 0) {
        return core_mux_topology_get();
    }
    if (strcmp(name, "mux_set_topology") == 0) {
        return core_mux_topology_set(cJSON_GetObjectItemCaseSensitive(args, "topology"));
    }
    return result(-1, NULL);
}

/* Returns the JSON-RPC response, or NULL for a notification. */
static cJSON *mcp_dispatch(const cJSON *req)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
    const char *method = json_string(req, "method");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
    if (method == NULL) {
        return jsonrpc_error(id, -32600, "Invalid Request");
    }
    if (id == NULL) {
        return NULL;    /* notification, e.g. notifications/initialized */
    }

    if (strcmp(method, "initialize") == 0) {
        cJSON *r = jsonrpc_response(id);
        cJSON *res = cJSON_AddObjectToObject(r, "result");
        const char *requested = json_string(params, "protocolVersion");
        cJSON_AddStringToObject(res, "protocolVersion",
                                (requested && (strcmp(requested, "2025-06-18") == 0 ||
                                               strcmp(requested, "2025-03-26") == 0))
                                ? requested : MCP_PROTOCOL_VERSION);
        cJSON *caps = cJSON_AddObjectToObject(res, "capabilities");
        cJSON *tools = cJSON_AddObjectToObject(caps, "tools");
        cJSON_AddBoolToObject(tools, "listChanged", false);
        cJSON *info = cJSON_AddObjectToObject(res, "serverInfo");
        cJSON_AddStringToObject(info, "name", "esp-usbip-bridge");
        cJSON_AddStringToObject(info, "version", HARNESS_FW_VERSION);
        cJSON_AddStringToObject(res, "instructions",
            "USB/IP hardware-in-the-loop bridge. USB devices behind its hubs are exported over USB/IP "
            "(TCP 3240); attach them from Linux with `usbip attach -r <bridge> -b <busid>`. Use list_hubs "
            "and set_port_power/power_cycle_port to switch DUT power per hub port, and the mux_* tools to "
            "route the shared I2C sensor chain to one DUT. The rescue_* tools interrogate a device, set its serial "
            "lines, reset it into its bootloader and peek at its console (refused while it is imported over USB/IP).");
        return r;
    }
    if (strcmp(method, "ping") == 0) {
        cJSON *r = jsonrpc_response(id);
        cJSON_AddObjectToObject(r, "result");
        return r;
    }
    if (strcmp(method, "tools/list") == 0) {
        cJSON *r = jsonrpc_response(id);
        cJSON *res = cJSON_AddObjectToObject(r, "result");
        cJSON *arr = cJSON_AddArrayToObject(res, "tools");
        for (size_t i = 0; i < sizeof(k_tools) / sizeof(k_tools[0]); i++) {
            cJSON *t = cJSON_CreateObject();
            cJSON_AddStringToObject(t, "name", k_tools[i].name);
            cJSON_AddStringToObject(t, "description", k_tools[i].description);
            cJSON_AddItemToObject(t, "inputSchema", cJSON_Parse(k_tools[i].input_schema));
            cJSON_AddItemToArray(arr, t);
        }
        return r;
    }
    if (strcmp(method, "tools/call") == 0) {
        const char *name = json_string(params, "name");
        const cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
        if (name == NULL) {
            return jsonrpc_error(id, -32602, "params.name is required");
        }
        api_result_t tr = mcp_call_tool(name, args);
        if (tr.status < 0) {
            return jsonrpc_error(id, -32602, "Unknown tool");
        }
        cJSON *r = jsonrpc_response(id);
        cJSON *res = cJSON_AddObjectToObject(r, "result");
        char *text = tr.json ? cJSON_PrintUnformatted(tr.json) : NULL;
        cJSON *content = cJSON_AddArrayToObject(res, "content");
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "type", "text");
        cJSON_AddStringToObject(c, "text", text ? text : "{}");
        cJSON_AddItemToArray(content, c);
        if (text) {
            cJSON_free(text);
        }
        if (cJSON_IsObject(tr.json)) {
            cJSON_AddItemToObject(res, "structuredContent", tr.json);
        } else {
            cJSON_Delete(tr.json);
        }
        cJSON_AddBoolToObject(res, "isError", tr.status >= 400);
        return r;
    }
    return jsonrpc_error(id, -32601, "Method not found");
}

/* ===========================================================================
 *  REST routing
 * ======================================================================== */

static void respond_json(api_response_t *resp, api_result_t r)
{
    resp->status = r.status;
    resp->content_type = NULL;
    resp->body = NULL;
    resp->len = 0;
    if (r.json) {
        resp->body = cJSON_PrintUnformatted(r.json);
        resp->len = resp->body ? strlen(resp->body) : 0;
        cJSON_Delete(r.json);
    }
}

static void respond_empty(api_response_t *resp, int status)
{
    resp->status = status;
    resp->content_type = NULL;
    resp->body = NULL;
    resp->len = 0;
}

static void reboot_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static api_result_t core_reboot(void)
{
    static esp_timer_handle_t timer;
    if (timer == NULL) {
        esp_timer_create_args_t args = { .callback = reboot_cb, .name = "api_reboot" };
        if (esp_timer_create(&args, &timer) != ESP_OK) {
            return fail(503, "cannot schedule reboot");
        }
    }
    esp_timer_start_once(timer, API_REBOOT_DELAY_US);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "rebooting", true);
    return result(200, j);
}

/* POST /api/ota/boot {"partition":"ota_0","reboot":true} */
static api_result_t core_ota_boot(const cJSON *json)
{
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(json, "partition");
    ota_update_err_t e = { 0 };
    if (ota_update_set_boot(cJSON_IsString(p) ? p->valuestring : NULL, &e) != ESP_OK) {
        api_result_t r = fail(e.status, e.message);
        cJSON_AddStringToObject(r.json, "reason", e.error);
        return r;
    }
    const bool reboot = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "reboot"));
    if (reboot) {
        cJSON_Delete(core_reboot().json);
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddStringToObject(j, "boot", p->valuestring);
    cJSON_AddBoolToObject(j, "rebooting", reboot);
    return result(200, j);
}

void controller_api_schedule_reboot(void)
{
    cJSON_Delete(core_reboot().json);
}

static bool is_method(const char *method, const char *m)
{
    return strcmp(method, m) == 0;
}

static bool path_is(const char *path, const char *p)
{
    return strcmp(path, p) == 0;
}

/* ===========================================================================
 *  Capabilities and network (docs/bridge-discovery-and-capabilities.md)
 * ======================================================================== */

static cJSON *add_feature(cJSON *features, const char *name, int version)
{
    cJSON *f = cJSON_AddObjectToObject(features, name);
    cJSON_AddNumberToObject(f, "version", version);
    return f;
}

static api_result_t core_capabilities(void)
{
    cJSON *j = cJSON_CreateObject();
    char buf[72];
    cJSON_AddNumberToObject(j, "schema", 1);
    cJSON_AddStringToObject(j, "api_version", BRIDGE_API_VERSION);

    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *fw = cJSON_AddObjectToObject(j, "fw");
    cJSON_AddStringToObject(fw, "project", app->project_name);
    bridge_fw_version(buf, sizeof(buf));
    cJSON_AddStringToObject(fw, "version", buf);
    add_str_or_null(fw, "build_time", bridge_build_time());
    cJSON_AddStringToObject(fw, "idf", app->idf_ver);
    /* 16 hex digits as /api/ota and idf.py print them (esp_app_get_elf_sha256()
       stops at CONFIG_APP_RETRIEVE_LEN_ELF_SHA, 9 by default) */
    for (int i = 0; i < 8; i++) {
        snprintf(buf + 2 * i, 3, "%02x", app->app_elf_sha256[i]);
    }
    cJSON_AddStringToObject(fw, "elf_sha256", buf);
    const esp_partition_t *running = esp_ota_get_running_partition();
    add_str_or_null(fw, "running_partition", running ? running->label : NULL);

    cJSON *board = cJSON_AddObjectToObject(j, "board");
    cJSON_AddStringToObject(board, "target", CONFIG_IDF_TARGET);
    add_str_or_null(board, "profile", bridge_board_profile());
    uint8_t m[6] = {0};
    esp_read_mac(m, ESP_MAC_BASE);
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    cJSON_AddStringToObject(board, "mac", buf);
    char name[DEVICE_NAME_MAX_LEN];
    device_naming_get_hostname(name, sizeof(name));
    cJSON_AddStringToObject(board, "hostname", name);
    if (device_naming_get_board_id(name, sizeof(name)) == ESP_OK) {
        add_str_or_null(board, "board_id", name);
    } else {
        cJSON_AddNullToObject(board, "board_id");
    }

    cJSON_AddItemToObject(j, "network", network_describe());

    cJSON *ports = cJSON_AddObjectToObject(j, "ports");
    cJSON_AddNumberToObject(ports, "http", API_HTTP_PORT);
    cJSON_AddNumberToObject(ports, "usbip", USBIP_TCP_PORT);
    cJSON_AddNumberToObject(ports, "serial_stream", CAPTURE_STREAM_PORT);

    cJSON *auth = cJSON_AddObjectToObject(j, "auth");
    cJSON_AddBoolToObject(auth, "required", s_token[0] != '\0');

    cJSON *limits = cJSON_AddObjectToObject(j, "limits");
    cJSON_AddNumberToObject(limits, "max_devices", CONFIG_USBIP_MAX_DEVICES);
    cJSON_AddNumberToObject(limits, "max_interfaces_per_device", CONFIG_USBIP_MAX_INTERFACES);
    cJSON_AddNumberToObject(limits, "max_endpoints_per_device", CONFIG_USBIP_MAX_ENDPOINTS);
    cJSON_AddNumberToObject(limits, "max_inflight_transfers", CONFIG_USBIP_MAX_INFLIGHT_XFERS);
    cJSON_AddNumberToObject(limits, "rescue_staging_bytes", BLOB_TOTAL_MAX);

    /* Bump a feature's version when it gains or changes behaviour; never
       rename one. */
    cJSON *features = cJSON_AddObjectToObject(j, "features");
    add_feature(features, "usbip", 1);
    add_feature(features, "claims", 1);
    add_feature(features, "hub_power", 1);
    add_feature(features, "port_events", 1);
    {
        cJSON *mux = add_feature(features, "mux", 1);
        cJSON_AddBoolToObject(mux, "available", analog_mux_bus_ok());
        int groups = 0, duts = 0;
        const char *chips = "none";
        analog_mux_summary(&groups, &duts, &chips);
        cJSON_AddStringToObject(mux, "service", "_i2c-strand._tcp");   /* advertised when available */
        cJSON_AddStringToObject(mux, "chips", chips);
        cJSON_AddNumberToObject(mux, "groups", groups);
        cJSON_AddNumberToObject(mux, "duts", duts);
    }
    add_feature(features, "device_info", 1);
    add_feature(features, "serial_lines", 1);
    cJSON *modes = cJSON_AddArrayToObject(add_feature(features, "reset", 1), "modes");
    for (int mode = 0; mode <= RESCUE_MODE_AUTO_BOOTLOADER; mode++) {
        cJSON_AddItemToArray(modes, cJSON_CreateString(rescue_mode_name((rescue_mode_t)mode)));
    }
    add_feature(features, "console", 1);
    add_feature(features, "capture", 1);
    add_feature(features, "serial_stream", 1);
    for (size_t i = 0; i < sizeof(k_job_kinds) / sizeof(k_job_kinds[0]); i++) {
        if (k_job_kinds[i].feature == NULL) {
            continue;
        }
        cJSON *f = add_feature(features, k_job_kinds[i].feature, 1);
        if (strcmp(k_job_kinds[i].feature, "dut_fs") == 0) {
            cJSON *fs = cJSON_AddArrayToObject(f, "filesystems");
            for (size_t k = 0; k < dut_fs_kind_count; k++) {
                cJSON_AddItemToArray(fs, cJSON_CreateString(dpl_fs_name(dut_fs_kinds[k])));
            }
        }
    }
    /* A build whose partition table has no OTA slots cannot update itself. */
    if (esp_ota_get_next_update_partition(NULL) != NULL) {
        add_feature(features, "ota", 1);
    }
#if CONFIG_USBIP_WIFI_STA
    add_feature(features, "wifi_config", 1);
#endif
    add_feature(features, "debug_ctrl", 1);
    return result(200, j);
}

/* GET /api/network, PUT and DELETE /api/network/wifi */
static api_result_t core_network(const char *method, const char *path, const cJSON *json)
{
    if (path_is(path, "/api/network")) {
        return is_method(method, "GET") ? result(200, network_describe()) : fail(405, "use GET");
    }
    if (!path_is(path, "/api/network/wifi")) {
        return fail(404, "GET /api/network, PUT or DELETE /api/network/wifi");
    }
#if CONFIG_USBIP_WIFI_STA
    if (is_method(method, "PUT") || is_method(method, "POST")) {
        const char *ssid = json_string(json, "ssid");
        const char *password = json_string(json, "password");
        if (ssid == NULL) {
            return fail(400, "expected JSON body {\"ssid\": str, \"password\": str?, \"store\": bool?, \"wait_ms\": int?}");
        }
        int wait_ms = API_WIFI_WAIT_MS;
        json_int(json, "wait_ms", &wait_ms);
        if (wait_ms < 0 || wait_ms > API_WIFI_WAIT_MS) {
            wait_ms = API_WIFI_WAIT_MS;
        }
        wifi_sta_status_t st;
        esp_err_t err = wifi_sta_set(ssid, password ? password : "", json_bool(json, "store", false),
                                     (uint32_t)wait_ms, &st);
        if (err == ESP_ERR_INVALID_ARG) {
            return fail(400, "ssid: 1-32 characters; password: empty (open network) or 8-63 characters");
        }
        if (err != ESP_OK) {
            return fail(status_for_err(err), esp_err_to_name(err));
        }
        const bool connected = st.state == WIFI_STA_CONNECTED && st.has_ip;
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", true);
        cJSON_AddBoolToObject(j, "connected", connected);
        cJSON_AddBoolToObject(j, "stored", st.stored);
        cJSON_AddItemToObject(j, "network", network_describe());
        return result(connected ? 200 : 202, j);
    }
    if (is_method(method, "DELETE")) {
        esp_err_t err = wifi_sta_clear();
        if (err != ESP_OK) {
            return fail(status_for_err(err), esp_err_to_name(err));
        }
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", true);
        cJSON_AddItemToObject(j, "network", network_describe());
        return result(200, j);
    }
    return fail(405, "use PUT or DELETE");
#else
    (void)json;
    return fail(501, "this build has no Wi-Fi station to configure (no wifi_config feature)");
#endif
}

bool controller_api_handle(const char *method, const char *url, const char *auth,
                           const char *body, size_t body_len, api_response_t *resp)
{
    /* Split path and query */
    char path[256];
    const char *q = strchr(url, '?');
    size_t plen = q ? (size_t)(q - url) : strlen(url);
    if (plen >= sizeof(path)) {
        return false;
    }
    memcpy(path, url, plen);
    path[plen] = '\0';
    const char *query = q ? q + 1 : NULL;

    /* Normalise the solenoid-style "/ports" alias to "/api/ports" */
    char aliased[264];
    if (strncmp(path, "/ports", 6) == 0 && (path[6] == '\0' || path[6] == '/')) {
        snprintf(aliased, sizeof(aliased), "/api%s", path);
        strlcpy(path, aliased, sizeof(path));
    }

    const bool known = path_is(path, "/ping") || path_is(path, "/mcp") ||
                       strncmp(path, "/api/info", 9) == 0 || path_is(path, "/api/schema") ||
                       path_is(path, "/api/capabilities") ||
                       path_is(path, "/api/network") || strncmp(path, "/api/network/", 13) == 0 ||
                       strncmp(path, "/api/usb/", 9) == 0 || strncmp(path, "/api/ports", 10) == 0 ||
                       path_is(path, "/api/status") || path_is(path, "/api/duts") ||
                       strncmp(path, "/api/select", 11) == 0 || path_is(path, "/api/isolate") ||
                       strncmp(path, "/api/groups/", 12) == 0 || path_is(path, "/api/topology") ||
                       path_is(path, "/api/probe") || strncmp(path, "/api/auth", 9) == 0 ||
                       strncmp(path, "/api/settings", 13) == 0 ||
                       path_is(path, "/api/reboot") || strncmp(path, "/api/rescue/", 12) == 0 ||
                       path_is(path, "/api/rescue/files") || path_is(path, "/api/rescue/jobs") ||
                       path_is(path, "/api/capture") || strncmp(path, "/api/capture/", 13) == 0 ||
                       path_is(path, "/api/ota") || strncmp(path, "/api/ota/", 9) == 0;
    if (!known) {
        return false;
    }

    /* CORS preflight */
    if (is_method(method, "OPTIONS")) {
        respond_empty(resp, 204);
        return true;
    }

    const bool mutating = !(is_method(method, "GET") || is_method(method, "HEAD")) ||
                          strncmp(path, "/api/select", 11) == 0;
    if (mutating && !controller_api_authorized(auth)) {
        respond_json(resp, mux_fail(401, "unauthorized"));
        return true;
    }

    cJSON *json = (body != NULL && body_len > 0) ? cJSON_ParseWithLength(body, body_len) : NULL;
    api_result_t r = { .status = 0 };
    char seg[64];
    char seg2[64];

    if (path_is(path, "/ping")) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", true);
        r = result(200, j);
    } else if (path_is(path, "/api/info")) {
        r = core_info();
    } else if (path_is(path, "/api/schema")) {
        r = core_schema();
    } else if (path_is(path, "/api/capabilities")) {
        r = is_method(method, "GET") || is_method(method, "HEAD") ? core_capabilities() : fail(405, "use GET");
    } else if (path_is(path, "/api/network") || strncmp(path, "/api/network/", 13) == 0) {
        r = core_network(method, path, json);
    } else if (path_is(path, "/mcp")) {
        if (!is_method(method, "POST")) {
            r = mux_fail(405, "only POST is supported (stateless, no SSE stream)");
        } else if (!cJSON_IsObject(json)) {
            cJSON *e = jsonrpc_error(NULL, -32700, "Parse error");
            r = result(400, e);
        } else {
            cJSON *rpc = mcp_dispatch(json);
            if (rpc == NULL) {
                cJSON_Delete(json);
                respond_empty(resp, 202);
                return true;
            }
            r = result(200, rpc);
        }
    } else if (path_is(path, "/api/usb/devices")) {
        r = core_devices();
    } else if (strncmp(path, "/api/usb/devices/", 17) == 0) {
        const char *rest = take_segment(path + 17, seg, sizeof(seg));
        if (*rest == '\0') {
            r = (is_method(method, "GET") || is_method(method, "HEAD")) ? core_device(seg) : fail(405, "use GET");
        } else if (strcmp(rest, "/rescue") == 0) {
            r = is_method(method, "GET") ? core_rescue_info(seg) : fail(405, "use GET");
        } else if (is_method(method, "GET")) {
            r = core_device_tools(method, seg, rest, query, json);
        } else if (!is_method(method, "POST")) {
            r = fail(405, "use POST");
        } else if (strcmp(rest, "/serial") == 0) {
            r = core_rescue_serial(seg, json);
        } else if (strcmp(rest, "/reset") == 0) {
            r = core_rescue_reset(seg, json);
        } else if (strcmp(rest, "/console") == 0) {
            r = core_rescue_console(seg, json);
        } else {
            r = core_device_tools(method, seg, rest, query, json);
        }
    } else if (path_is(path, "/api/rescue/files") || strncmp(path, "/api/rescue/files/", 18) == 0) {
        if (path[17] == '/') {
            take_segment(path + 18, seg, sizeof(seg));
        } else {
            seg[0] = '\0';
        }
        r = core_rescue_files(method, seg);
    } else if (path_is(path, "/api/capture") || strncmp(path, "/api/capture/", 13) == 0) {
        r = core_capture(method, path, json);
    } else if (path_is(path, "/api/rescue/holders") && is_method(method, "GET")) {
        r = core_rescue_holders();
    } else if (path_is(path, "/api/rescue/jobs") || strncmp(path, "/api/rescue/jobs/", 17) == 0) {
        if (path[16] == '/') {
            take_segment(path + 17, seg, sizeof(seg));
        } else {
            seg[0] = '\0';
        }
        r = core_rescue_jobs(method, seg);
    } else if (path_is(path, "/api/usb/hubs")) {
        r = core_hubs();
    } else if (path_is(path, "/api/usb/enum_events")) {
        r = core_enum_events();
    } else if (path_is(path, "/api/usb/claims")) {
        r = core_usb_claims();
    } else if (path_is(path, "/api/usb/debug_ctrl") && is_method(method, "POST")) {
        r = core_debug_ctrl(json);
    } else if (path_is(path, "/api/usb/debug_dump_enum") && is_method(method, "POST")) {
        r = core_debug_dump_enum(json);
    } else if (path_is(path, "/api/usb/debug")) {
        /* Diagnostics: the USB host's hub/port/enumeration state goes to the console */
        esp_err_t err = usb_host_hub_debug_dump();
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", err == ESP_OK);
        cJSON_AddStringToObject(j, "note", "state logged to the console (UART0 and USB Serial/JTAG)");
        r = result(err == ESP_OK ? 200 : 503, j);
    } else if (path_is(path, "/api/ports/events") && is_method(method, "GET")) {
        r = core_port_events();
    } else if (path_is(path, "/api/ports/hwstatus") && is_method(method, "GET")) {
        r = core_port_hwstatus();
    } else if (path_is(path, "/api/ports")) {
        r = core_ports();
    } else if (path_is(path, "/api/ports/off") && is_method(method, "POST")) {
        r = core_ports_all(false);
    } else if (path_is(path, "/api/ports/on") && is_method(method, "POST")) {
        r = core_ports_all(true);
    } else if (path_is(path, "/api/ports/restore") && is_method(method, "POST")) {
        r = core_restore();
    } else if (path_is(path, "/api/settings/enum_timeout") && is_method(method, "POST")) {
        r = core_set_enum_timeout(json);
    } else if (path_is(path, "/api/settings")) {
        r = is_method(method, "GET") ? core_get_settings() : core_set_settings(json);
    } else if (strncmp(path, "/api/ports/", 11) == 0) {
        const char *rest = take_segment(path + 11, seg, sizeof(seg));
        bool force = json_bool(json, "force", query_flag(query, "force"));
        if (*rest == '\0') {
            r = is_method(method, "GET") ? core_port_get(seg) : fail(405, "use GET");
        } else if (!is_method(method, "POST")) {
            r = fail(405, "use POST");
        } else if (strcmp(rest, "/on") == 0) {
            r = core_port_power(seg, true, force);
        } else if (strcmp(rest, "/off") == 0) {
            r = core_port_power(seg, false, force);
        } else if (strcmp(rest, "/cycle") == 0) {
            int off_ms = query_int(query, "off_ms", API_DEFAULT_OFF_MS);
            json_int(json, "off_ms", &off_ms);
            r = core_port_cycle(seg, off_ms, force);
        } else {
            r = fail(404, "unknown port action (on, off, cycle)");
        }
    } else if (path_is(path, "/api/status")) {
        r = core_mux_status();
    } else if (path_is(path, "/api/duts")) {
        r = core_mux_duts();
    } else if (path_is(path, "/api/probe")) {
        r = core_mux_probe();
    } else if (path_is(path, "/api/select")) {
        const char *dut = json_string(json, "dut");
        r = (is_method(method, "POST") && dut) ? core_mux_select_dut(dut)
            : mux_fail(400, "expected JSON body {\"dut\": \"<name>\"}");
    } else if (strncmp(path, "/api/select/", 12) == 0) {
        take_segment(path + 12, seg, sizeof(seg));
        r = core_mux_select_dut(seg);
    } else if (path_is(path, "/api/isolate")) {
        r = is_method(method, "POST") ? core_mux_isolate() : mux_fail(405, "use POST");
    } else if (strncmp(path, "/api/groups/", 12) == 0) {
        const char *rest = take_segment(path + 12, seg, sizeof(seg));
        if (strncmp(rest, "/select/", 8) == 0 && is_method(method, "POST")) {
            take_segment(rest + 8, seg2, sizeof(seg2));
            char *end = NULL;
            long ch = strtol(seg2, &end, 10);
            r = (end != seg2 && *end == '\0') ? core_mux_select_channel(seg, (int)ch)
                : mux_fail(400, "channel must be an integer");
        } else if (strcmp(rest, "/channel") == 0 && is_method(method, "POST")) {
            int ch;
            if (!json_int(json, "channel", &ch) || cJSON_GetObjectItemCaseSensitive(json, "closed") == NULL) {
                r = mux_fail(400, "expected JSON body {\"channel\": <int>, \"closed\": <bool>}");
            } else {
                r = core_mux_set_channel(seg, ch, json_bool(json, "closed", false));
            }
        } else {
            r = mux_fail(404, "Not Found");
        }
    } else if (path_is(path, "/api/topology")) {
        if (is_method(method, "GET")) {
            r = core_mux_topology_get();
        } else if (is_method(method, "PUT") || is_method(method, "POST")) {
            r = core_mux_topology_set(json);
        } else {
            r = mux_fail(405, "use GET or PUT");
        }
    } else if (path_is(path, "/api/auth")) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "auth_required", s_token[0] != '\0');
        r = result(200, j);
    } else if (path_is(path, "/api/auth/token") && is_method(method, "POST")) {
        const char *token = json_string(json, "token");
        if (token == NULL) {
            r = fail(400, "expected JSON body {\"token\": \"<token>\"} (empty clears)");
        } else {
            esp_err_t err = set_token(token);
            if (err != ESP_OK) {
                r = fail(status_for_err(err), esp_err_to_name(err));
            } else {
                cJSON *j = cJSON_CreateObject();
                cJSON_AddBoolToObject(j, "ok", true);
                cJSON_AddBoolToObject(j, "auth_required", s_token[0] != '\0');
                r = result(200, j);
            }
        }
    } else if (path_is(path, "/api/reboot") && is_method(method, "POST")) {
        r = core_reboot();
    } else if (path_is(path, "/api/ota") && is_method(method, "GET")) {
        r = result(200, ota_update_status_json());
    } else if (path_is(path, "/api/ota/boot") && is_method(method, "POST")) {
        r = core_ota_boot(json);
    } else {
        r = mux_fail(404, "Not Found");
    }

    cJSON_Delete(json);
    respond_json(resp, r);
    return true;
}
