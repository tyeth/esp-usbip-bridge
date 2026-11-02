#include "discovery_service.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"
#include "sdkconfig.h"

#include "analog_mux.h"
#include "bridge_identity.h"
#include "controller_api.h"
#include "device_naming.h"
#include "usb_backend.h"
#include "usbip_protocol.h"

#define USBIP_MDNS_SERVICE_TYPE "_usbip"
#define USBIP_MDNS_SERVICE_PROTO "_tcp"
/* The bridge itself, for clients that drive it over HTTP (the HIL
   controller): a small, static TXT; GET /api/capabilities is the truth.
   ("_esp-usbip-bridge" is 16 characters, over the DNS-SD limit of 15.) */
#define BRIDGE_MDNS_SERVICE_TYPE "_esp-usbip"
#define BRIDGE_MDNS_SERVICE_PROTO "_tcp"
#define BRIDGE_HTTP_PORT 80
#define BRIDGE_PRODUCT "esp-usbip-bridge"
/* On _usbip._tcp and _esp-usbip._tcp: a browse of
   _esp-usbip-bridge._sub._usbip._tcp finds only these bridges among USB/IP
   servers. */
#define BRIDGE_MDNS_SUBTYPE "_esp-usbip-bridge"
/* The I2C strand mux, on the same HTTP port: the same API as the
   standalone sbc-dut-analog-mux box (product=sbc-dut-analog-mux). */
#define STRAND_MDNS_SERVICE_TYPE "_i2c-strand"
#define STRAND_MDNS_SERVICE_PROTO "_tcp"
#define STRAND_API_VERSION "1"
#define USBIP_DISCOVERY_TASK_STACK 4096
#define USBIP_DISCOVERY_TASK_PRIORITY 5
#define USBIP_DISCOVERY_REFRESH_MS 1000

static const char *TAG = "discovery";

typedef struct {
    uint32_t count;
    char primary_busid[sizeof(((usbip_backend_device_t *)0)->busid)];
    uint16_t primary_vid;
    uint16_t primary_pid;
} discovery_snapshot_t;

static discovery_snapshot_t s_last_snapshot;
static char s_board_id[DEVICE_NAME_MAX_LEN];
static char s_hostname[DEVICE_NAME_MAX_LEN];  /* the mDNS host and instance name in use */
static bool s_started;
static bool s_bridge_auth;                  /* the auth value last published */

typedef struct {
    bool advertised;
    int groups, duts;
    const char *mux;
} strand_state_t;
static strand_state_t s_strand;             /* what _i2c-strand._tcp last published */

static void base_mac_str(char *buf, size_t len)
{
    uint8_t m[6] = {0};
    esp_read_mac(m, ESP_MAC_BASE);
    snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static bool snapshot_changed(const discovery_snapshot_t *a, const discovery_snapshot_t *b)
{
    return (a->count != b->count) ||
           (a->primary_vid != b->primary_vid) ||
           (a->primary_pid != b->primary_pid) ||
           (strcmp(a->primary_busid, b->primary_busid) != 0);
}

static esp_err_t discovery_publish_txt_from_snapshot(const discovery_snapshot_t *snapshot)
{
    char state[12];
    char busid[sizeof(snapshot->primary_busid)];
    char vid[8];
    char pid[8];
    char count[12];

    snprintf(count, sizeof(count), "%" PRIu32, snapshot->count);

    if (snapshot->count == 0) {
        strlcpy(state, "idle", sizeof(state));
        strlcpy(busid, "-", sizeof(busid));
        strlcpy(vid, "-", sizeof(vid));
        strlcpy(pid, "-", sizeof(pid));
    } else if (snapshot->count == 1) {
        strlcpy(state, "exporting", sizeof(state));
        strlcpy(busid, snapshot->primary_busid, sizeof(busid));
        snprintf(vid, sizeof(vid), "%04x", snapshot->primary_vid);
        snprintf(pid, sizeof(pid), "%04x", snapshot->primary_pid);
    } else {
        strlcpy(state, "exporting", sizeof(state));
        strlcpy(busid, "multi", sizeof(busid));
        strlcpy(vid, "-", sizeof(vid));
        strlcpy(pid, "-", sizeof(pid));
    }

    mdns_txt_item_t txt[] = {
        {.key = "transport", .value = "usbip"},
        {.key = "state", .value = state},
        {.key = "count", .value = count},
        {.key = "busid", .value = busid},
        {.key = "vid", .value = vid},
        {.key = "pid", .value = pid},
        {.key = "target", .value = CONFIG_IDF_TARGET},
        {.key = "board_id", .value = s_board_id},
    };

    return mdns_service_txt_set(USBIP_MDNS_SERVICE_TYPE, USBIP_MDNS_SERVICE_PROTO, txt, sizeof(txt) / sizeof(txt[0]));
}

/* _esp-usbip._tcp: product, api, fw, target, board, mac, caps, auth.  add:
   register the service (instance name = hostname) with its subtype, else
   replace its TXT. */
static esp_err_t discovery_publish_bridge(bool add, const char *hostname)
{
    char fw[48];
    bridge_fw_version(fw, sizeof(fw));
    char mac[18];
    base_mac_str(mac, sizeof(mac));
    const char *board = bridge_board_profile();
    s_bridge_auth = controller_api_auth_required();

    mdns_txt_item_t txt[] = {
        {.key = "product", .value = BRIDGE_PRODUCT},
        {.key = "api", .value = BRIDGE_API_VERSION},
        {.key = "fw", .value = fw},
        {.key = "target", .value = CONFIG_IDF_TARGET},
        {.key = "board", .value = board ? board : "none"},
        {.key = "mac", .value = mac},
        {.key = "caps", .value = "/api/capabilities"},
        {.key = "auth", .value = s_bridge_auth ? "1" : "0"},
    };
    const size_t n = sizeof(txt) / sizeof(txt[0]);
    if (add) {
        esp_err_t err = mdns_service_add(hostname, BRIDGE_MDNS_SERVICE_TYPE, BRIDGE_MDNS_SERVICE_PROTO,
                                         BRIDGE_HTTP_PORT, txt, n);
        if (err == ESP_OK) {
            err = mdns_service_subtype_add_for_host(NULL, BRIDGE_MDNS_SERVICE_TYPE, BRIDGE_MDNS_SERVICE_PROTO,
                                                    NULL, BRIDGE_MDNS_SUBTYPE);
        }
        return err;
    }
    return mdns_service_txt_set(BRIDGE_MDNS_SERVICE_TYPE, BRIDGE_MDNS_SERVICE_PROTO, txt, n);
}

/* _i2c-strand._tcp while the mux control bus is up (even with no groups
   configured): product, api, mux, groups, duts, path, mac, caps.  Called
   every discovery pass; touches mDNS only when something changed. */
static void discovery_publish_strand(const char *hostname)
{
    const bool want = analog_mux_bus_ok();
    strand_state_t now = { .advertised = want, .mux = "none" };
    analog_mux_summary(&now.groups, &now.duts, &now.mux);
    if (!want) {
        if (s_strand.advertised && mdns_service_remove(STRAND_MDNS_SERVICE_TYPE, STRAND_MDNS_SERVICE_PROTO) == ESP_OK) {
            ESP_LOGI(TAG, "%s.%s withdrawn: mux control bus down", STRAND_MDNS_SERVICE_TYPE, STRAND_MDNS_SERVICE_PROTO);
            s_strand.advertised = false;
        }
        return;
    }
    if (s_strand.advertised && now.groups == s_strand.groups && now.duts == s_strand.duts &&
        strcmp(now.mux, s_strand.mux) == 0) {
        return;
    }
    char mac[18], groups[8], duts[8];
    base_mac_str(mac, sizeof(mac));
    snprintf(groups, sizeof(groups), "%d", now.groups);
    snprintf(duts, sizeof(duts), "%d", now.duts);
    mdns_txt_item_t txt[] = {
        {.key = "product", .value = BRIDGE_PRODUCT},
        {.key = "api", .value = STRAND_API_VERSION},
        {.key = "mux", .value = now.mux},
        {.key = "groups", .value = groups},
        {.key = "duts", .value = duts},
        {.key = "path", .value = "/api"},
        {.key = "mac", .value = mac},
        {.key = "caps", .value = "/api/capabilities"},
    };
    const size_t n = sizeof(txt) / sizeof(txt[0]);
    const esp_err_t err = s_strand.advertised
        ? mdns_service_txt_set(STRAND_MDNS_SERVICE_TYPE, STRAND_MDNS_SERVICE_PROTO, txt, n)
        : mdns_service_add(hostname, STRAND_MDNS_SERVICE_TYPE, STRAND_MDNS_SERVICE_PROTO, BRIDGE_HTTP_PORT, txt, n);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s.%s: %s", STRAND_MDNS_SERVICE_TYPE, STRAND_MDNS_SERVICE_PROTO, esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "%s %s.%s port=%d: mux=%s groups=%d duts=%d", s_strand.advertised ? "Updated" : "Advertised",
             STRAND_MDNS_SERVICE_TYPE, STRAND_MDNS_SERVICE_PROTO, BRIDGE_HTTP_PORT, now.mux, now.groups, now.duts);
    s_strand = now;
}

static void discovery_task(void *arg)
{
    (void)arg;

    usbip_backend_device_t *devices = malloc(CONFIG_USBIP_MAX_DEVICES * sizeof(usbip_backend_device_t));
    if (devices == NULL) {
        ESP_LOGE(TAG, "Failed to allocate device list");
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        const size_t count = usb_backend_get_devices(devices, CONFIG_USBIP_MAX_DEVICES);

        discovery_snapshot_t current = {
            .count = (uint32_t)count,
            .primary_vid = 0,
            .primary_pid = 0,
        };

        strlcpy(current.primary_busid, "-", sizeof(current.primary_busid));

        if (count > 0) {
            strlcpy(current.primary_busid, devices[0].busid, sizeof(current.primary_busid));
            current.primary_vid = devices[0].id_vendor;
            current.primary_pid = devices[0].id_product;
        }

        /* Always re-read the board ID so runtime changes (via HTTP API)
           are picked up without needing a reboot. */
        device_naming_get_board_id(s_board_id, sizeof(s_board_id));

        if (snapshot_changed(&current, &s_last_snapshot)) {
            esp_err_t err = discovery_publish_txt_from_snapshot(&current);
            if (err == ESP_OK) {
                s_last_snapshot = current;
                ESP_LOGI(TAG,
                         "Updated DNS-SD TXT: state=%s count=%" PRIu32 " busid=%s",
                         current.count ? "exporting" : "idle",
                         current.count,
                         current.count > 1 ? "multi" : current.primary_busid);
            } else {
                ESP_LOGW(TAG, "mdns_service_txt_set failed: %s", esp_err_to_name(err));
            }
        }

        discovery_publish_strand(s_hostname);

        vTaskDelay(pdMS_TO_TICKS(USBIP_DISCOVERY_REFRESH_MS));
    }
}

esp_err_t discovery_service_start(void)
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_BASE);
    if (err != ESP_OK) {
        return err;
    }

    char hostname[DEVICE_NAME_MAX_LEN];
    device_naming_get_hostname(hostname, sizeof(hostname));
    strlcpy(s_hostname, hostname, sizeof(s_hostname));

    char instance[DEVICE_NAME_MAX_LEN + 32];
    snprintf(instance, sizeof(instance), "%s (%s)", hostname, CONFIG_IDF_TARGET);

    err = mdns_init();
    if (err != ESP_OK) {
        return err;
    }

    err = mdns_hostname_set(hostname);
    if (err != ESP_OK) {
        return err;
    }

    err = mdns_instance_name_set(instance);
    if (err != ESP_OK) {
        return err;
    }

    /* Load the DUT board ID from NVS (static, doesn't change with devices) */
    device_naming_get_board_id(s_board_id, sizeof(s_board_id));

    discovery_snapshot_t initial = {
        .count = 0,
        .primary_vid = 0,
        .primary_pid = 0,
    };
    strlcpy(initial.primary_busid, "-", sizeof(initial.primary_busid));

    mdns_txt_item_t initial_txt[] = {
        {.key = "transport", .value = "usbip"},
        {.key = "state", .value = "idle"},
        {.key = "count", .value = "0"},
        {.key = "busid", .value = "-"},
        {.key = "vid", .value = "-"},
        {.key = "pid", .value = "-"},
        {.key = "target", .value = CONFIG_IDF_TARGET},
        {.key = "board_id", .value = s_board_id},
    };

    err = mdns_service_add(NULL,
                           USBIP_MDNS_SERVICE_TYPE,
                           USBIP_MDNS_SERVICE_PROTO,
                           USBIP_TCP_PORT,
                           initial_txt,
                           sizeof(initial_txt) / sizeof(initial_txt[0]));
    if (err != ESP_OK) {
        return err;
    }
    err = mdns_service_subtype_add_for_host(NULL, USBIP_MDNS_SERVICE_TYPE, USBIP_MDNS_SERVICE_PROTO,
                                            NULL, BRIDGE_MDNS_SUBTYPE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s subtype on %s: %s", BRIDGE_MDNS_SUBTYPE, USBIP_MDNS_SERVICE_TYPE, esp_err_to_name(err));
    }

    s_last_snapshot = initial;

    ESP_LOGI(TAG, "DNS-SD service advertised: %s.%s port=%d host=%s.local",
             USBIP_MDNS_SERVICE_TYPE, USBIP_MDNS_SERVICE_PROTO, USBIP_TCP_PORT, hostname);

    err = discovery_publish_bridge(true, hostname);
    if (err != ESP_OK) {
        return err;
    }
    s_started = true;
    ESP_LOGI(TAG, "DNS-SD service advertised: %s.%s port=%d instance=%s api=%s product=%s (subtype %s here and on %s.%s)",
             BRIDGE_MDNS_SERVICE_TYPE, BRIDGE_MDNS_SERVICE_PROTO, BRIDGE_HTTP_PORT, hostname,
             BRIDGE_API_VERSION, BRIDGE_PRODUCT, BRIDGE_MDNS_SUBTYPE, USBIP_MDNS_SERVICE_TYPE, USBIP_MDNS_SERVICE_PROTO);
    discovery_publish_strand(hostname);

    if (xTaskCreate(discovery_task,
                    "usbip_discovery",
                    USBIP_DISCOVERY_TASK_STACK,
                    NULL,
                    USBIP_DISCOVERY_TASK_PRIORITY,
                    NULL) != pdPASS) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t discovery_service_notify_config_changed(void)
{
    char hostname[DEVICE_NAME_MAX_LEN];
    device_naming_get_hostname(hostname, sizeof(hostname));

    esp_err_t err = mdns_hostname_set(hostname);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_hostname_set failed: %s", esp_err_to_name(err));
        return err;
    }

    char instance[DEVICE_NAME_MAX_LEN + 32];
    snprintf(instance, sizeof(instance), "%s (%s)", hostname, CONFIG_IDF_TARGET);
    err = mdns_instance_name_set(instance);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_instance_name_set failed: %s", esp_err_to_name(err));
        /* Non-fatal; continue to update TXT records. */
    }
    err = mdns_service_instance_name_set(BRIDGE_MDNS_SERVICE_TYPE, BRIDGE_MDNS_SERVICE_PROTO, hostname);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s instance name: %s", BRIDGE_MDNS_SERVICE_TYPE, esp_err_to_name(err));
    }
    if (s_strand.advertised) {
        err = mdns_service_instance_name_set(STRAND_MDNS_SERVICE_TYPE, STRAND_MDNS_SERVICE_PROTO, hostname);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "%s instance name: %s", STRAND_MDNS_SERVICE_TYPE, esp_err_to_name(err));
        }
    }
    strlcpy(s_hostname, hostname, sizeof(s_hostname));

    /* Re-read board ID and force re-publish of TXT records. */
    device_naming_get_board_id(s_board_id, sizeof(s_board_id));

    err = discovery_publish_txt_from_snapshot(&s_last_snapshot);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_service_txt_set failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "mDNS identity refreshed: host=%s.local, board_id=\"%s\"",
             hostname, s_board_id);
    return ESP_OK;
}

esp_err_t discovery_service_notify_auth_changed(void)
{
    if (!s_started || controller_api_auth_required() == s_bridge_auth) {
        return ESP_OK;
    }
    esp_err_t err = discovery_publish_bridge(false, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s TXT: %s", BRIDGE_MDNS_SERVICE_TYPE, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "%s TXT republished: auth=%d", BRIDGE_MDNS_SERVICE_TYPE, s_bridge_auth);
    }
    return err;
}
