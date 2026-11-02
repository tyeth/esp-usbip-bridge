#include "wifi_sta.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "device_naming.h"

#define WIFI_NVS_NAMESPACE "wifi"
#define WIFI_NVS_KEY_SSID  "ssid"
#define WIFI_NVS_KEY_PASS  "pass"

#define WIFI_BACKOFF_MIN_MS 1000
#define WIFI_BACKOFF_MAX_MS 30000

#define WIFI_GOT_IP_BIT BIT0

static const char *TAG = "wifi_sta";

static SemaphoreHandle_t s_lock;
static EventGroupHandle_t s_events;
static esp_timer_handle_t s_retry_timer;
static esp_netif_t *s_netif;
static bool s_driver_started;           /* WIFI_EVENT_STA_START seen */

/* Guarded by s_lock.  The password never leaves this file. */
static char s_ssid[WIFI_STA_SSID_MAX + 1];
static char s_pass[WIFI_STA_PASS_MAX + 1];
static bool s_configured;
static bool s_stored;
static bool s_pending_store;            /* write to NVS once they get an address */
static wifi_sta_state_t s_state;
static bool s_has_ip;
static uint32_t s_ip;
static uint32_t s_attempts;
static int s_last_reason;
static uint32_t s_backoff_ms = WIFI_BACKOFF_MIN_MS;

const char *wifi_sta_state_name(wifi_sta_state_t state)
{
    switch (state) {
    case WIFI_STA_CONNECTING: return "connecting";
    case WIFI_STA_CONNECTED:  return "connected";
    case WIFI_STA_RETRYING:   return "retrying";
    default:                  return "unconfigured";
    }
}

const char *wifi_sta_reason_name(int reason)
{
    switch (reason) {
    case 0:                                         return "none";
    case WIFI_REASON_AUTH_EXPIRE:                   return "auth_expire";
    case WIFI_REASON_AUTH_LEAVE:                    return "auth_leave";
    case WIFI_REASON_ASSOC_LEAVE:                   return "assoc_leave";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:        return "4way_handshake_timeout";
    case WIFI_REASON_BEACON_TIMEOUT:                return "beacon_timeout";
    case WIFI_REASON_NO_AP_FOUND:                   return "no_ap_found";
    case WIFI_REASON_AUTH_FAIL:                     return "auth_fail";
    case WIFI_REASON_ASSOC_FAIL:                    return "assoc_fail";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:             return "handshake_timeout";
    case WIFI_REASON_CONNECTION_FAIL:               return "connection_fail";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY: return "no_ap_found_compatible_security";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: return "no_ap_found_in_authmode_threshold";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD: return "no_ap_found_in_rssi_threshold";
    default:                                        return "other";
    }
}

/* ===========================================================================
 *  NVS (namespace "wifi", the S3 transport's keys)
 * ======================================================================== */

static esp_err_t creds_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, WIFI_NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, WIFI_NVS_KEY_PASS, pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static bool creds_load(char *ssid, size_t ssid_size, char *pass, size_t pass_size)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_get_str(h, WIFI_NVS_KEY_SSID, ssid, &ssid_size) == ESP_OK && ssid[0] != '\0';
    if (ok && nvs_get_str(h, WIFI_NVS_KEY_PASS, pass, &pass_size) != ESP_OK) {
        pass[0] = '\0';                         /* an open network */
    }
    nvs_close(h);
    return ok;
}

static esp_err_t creds_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, WIFI_NVS_KEY_SSID);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_erase_key(h, WIFI_NVS_KEY_PASS);
    }
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ===========================================================================
 *  Connection
 * ======================================================================== */

static wifi_auth_mode_t min_authmode(const char *pass)
{
    if (pass[0] == '\0') {
        return WIFI_AUTH_OPEN;
    }
    /* WPA2_WPA3 as a threshold would turn away a WPA2-only AP. */
    switch (CONFIG_USBIP_WIFI_AUTHMODE) {
    case 0:
    case 1:  return WIFI_AUTH_WPA_PSK;
    case 3:  return WIFI_AUTH_WPA2_WPA3_PSK;
    default: return WIFI_AUTH_WPA2_PSK;
    }
}

/* The driver config for the current credentials.  Called with s_lock held;
   the caller hands it to esp_wifi_set_config() after releasing the lock (the
   event handlers take it) and wipes it. */
static void build_config_locked(wifi_config_t *cfg)
{
    *cfg = (wifi_config_t){
        .sta = {
            .threshold.authmode = min_authmode(s_pass),
            .pmf_cfg = { .capable = true, .required = false },
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
        },
    };
    strlcpy((char *)cfg->sta.ssid, s_ssid, sizeof(cfg->sta.ssid));
    strlcpy((char *)cfg->sta.password, s_pass, sizeof(cfg->sta.password));
}

static void connect_now(void)
{
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    }
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    bool go = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_configured && s_state == WIFI_STA_RETRYING) {
        s_state = WIFI_STA_CONNECTING;
        go = true;
    }
    xSemaphoreGive(s_lock);
    if (go) {
        connect_now();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        bool go;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_driver_started = true;
        go = s_configured;
        if (go) {
            s_state = WIFI_STA_CONNECTING;
        }
        xSemaphoreGive(s_lock);
        if (go) {
            connect_now();
        }
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *ev = data;
        ESP_LOGI(TAG, "associated with \"%.*s\" (channel %u), waiting for DHCP",
                 (int)ev->ssid_len, (const char *)ev->ssid, ev->channel);
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        xEventGroupClearBits(s_events, WIFI_GOT_IP_BIT);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_has_ip = false;
        /* assoc_leave only follows our own esp_wifi_disconnect() (new
           credentials or clear), which reconnects or stops by itself. */
        if (!s_configured || ev->reason == WIFI_REASON_ASSOC_LEAVE) {
            if (!s_configured) {
                s_state = WIFI_STA_UNCONFIGURED;
            }
            xSemaphoreGive(s_lock);
            return;
        }
        s_attempts++;
        s_last_reason = ev->reason;
        s_state = WIFI_STA_RETRYING;
        const uint32_t delay_ms = s_backoff_ms;
        s_backoff_ms = (s_backoff_ms * 2 > WIFI_BACKOFF_MAX_MS) ? WIFI_BACKOFF_MAX_MS : s_backoff_ms * 2;
        const uint32_t attempts = s_attempts;
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "disconnected (reason %d %s, attempt %lu), retry in %lu ms",
                 ev->reason, wifi_sta_reason_name(ev->reason), (unsigned long)attempts, (unsigned long)delay_ms);
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        char ssid[WIFI_STA_SSID_MAX + 1];
        char pass[WIFI_STA_PASS_MAX + 1];
        bool store;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_has_ip = true;
        s_ip = ev->ip_info.ip.addr;
        s_state = WIFI_STA_CONNECTED;
        s_attempts = 0;
        s_backoff_ms = WIFI_BACKOFF_MIN_MS;
        store = s_pending_store;
        strlcpy(ssid, s_ssid, sizeof(ssid));
        strlcpy(pass, s_pass, sizeof(pass));
        xSemaphoreGive(s_lock);

        ESP_LOGI(TAG, "Wi-Fi IPv4: " IPSTR " (\"%s\")", IP2STR(&ev->ip_info.ip), ssid);
        if (store) {
            esp_err_t err = creds_save(ssid, pass);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            /* Unless they were replaced meanwhile */
            if (err == ESP_OK && s_pending_store && strcmp(ssid, s_ssid) == 0 && strcmp(pass, s_pass) == 0) {
                s_pending_store = false;
                s_stored = true;
            }
            xSemaphoreGive(s_lock);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "credentials for \"%s\" stored", ssid);
            } else {
                ESP_LOGW(TAG, "storing the credentials failed: %s", esp_err_to_name(err));
            }
        }
        memset(pass, 0, sizeof(pass));
        xEventGroupSetBits(s_events, WIFI_GOT_IP_BIT);
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_has_ip = false;
        xSemaphoreGive(s_lock);
        xEventGroupClearBits(s_events, WIFI_GOT_IP_BIT);
    }
}

/* ===========================================================================
 *  Public
 * ======================================================================== */

esp_err_t wifi_sta_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    if (s_lock == NULL || s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_timer_create_args_t targs = { .callback = retry_timer_cb, .name = "wifi_retry" };
    esp_err_t err = esp_timer_create(&targs, &s_retry_timer);
    if (err != ESP_OK) {
        return err;
    }

    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* DHCP option 12, as the Ethernet netif and mDNS use */
    char hostname[DEVICE_NAME_MAX_LEN];
    device_naming_get_hostname(hostname, sizeof(hostname));
    esp_netif_set_hostname(s_netif, hostname);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        return err;
    }
    /* NVS "wifi" is the one copy of the credentials: DELETE must really forget them. */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &wifi_event_handler, NULL, NULL);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t cfg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool configured = creds_load(s_ssid, sizeof(s_ssid), s_pass, sizeof(s_pass));
    if (configured) {
        s_configured = true;
        s_stored = true;
        s_state = WIFI_STA_CONNECTING;
        build_config_locked(&cfg);
        ESP_LOGI(TAG, "joining \"%s\"", s_ssid);
    } else {
        s_ssid[0] = s_pass[0] = '\0';
        ESP_LOGI(TAG, "no credentials stored (console: wifi set <ssid> <password>)");
    }
    xSemaphoreGive(s_lock);
    if (configured) {
        esp_wifi_set_config(WIFI_IF_STA, &cfg);
        memset(&cfg, 0, sizeof(cfg));
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }
#if CONFIG_USBIP_WIFI_DISABLE_MODEM_SLEEP
    esp_wifi_set_ps(WIFI_PS_NONE);
#endif
    return ESP_OK;
}

esp_err_t wifi_sta_set(const char *ssid, const char *password, bool store, uint32_t wait_ms,
                       wifi_sta_status_t *status)
{
    if (password == NULL) {
        password = "";
    }
    const size_t slen = ssid ? strlen(ssid) : 0;
    const size_t plen = strlen(password);
    /* WPA passphrases are 8-63 characters; empty means an open network. */
    if (slen == 0 || slen > WIFI_STA_SSID_MAX || plen > WIFI_STA_PASS_MAX - 1 || (plen > 0 && plen < 8)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (store) {
        esp_err_t err = creds_save(ssid, password);
        if (err != ESP_OK) {
            return err;
        }
    }

    esp_timer_stop(s_retry_timer);
    xEventGroupClearBits(s_events, WIFI_GOT_IP_BIT);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, password, sizeof(s_pass));
    s_configured = true;
    s_stored = store;
    s_pending_store = !store;
    s_attempts = 0;
    s_last_reason = 0;
    s_backoff_ms = WIFI_BACKOFF_MIN_MS;
    s_has_ip = false;
    s_state = WIFI_STA_CONNECTING;
    const bool started = s_driver_started;
    wifi_config_t cfg;
    build_config_locked(&cfg);
    xSemaphoreGive(s_lock);
    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    memset(&cfg, 0, sizeof(cfg));
    ESP_LOGI(TAG, "joining \"%s\"%s", ssid, store ? " (stored)" : " (stored once connected)");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config: %s", esp_err_to_name(err));
        return err;
    }
    /* Before STA_START the start event connects. */
    if (started) {
        connect_now();
    }

    if (wait_ms > 0) {
        xEventGroupWaitBits(s_events, WIFI_GOT_IP_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(wait_ms));
    }
    if (status) {
        wifi_sta_get_status(status);
    }
    return ESP_OK;
}

esp_err_t wifi_sta_clear(void)
{
    esp_err_t err = creds_erase();
    if (s_lock == NULL) {
        return err;
    }
    esp_timer_stop(s_retry_timer);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_pass, 0, sizeof(s_pass));
    s_ssid[0] = '\0';
    s_configured = false;
    s_stored = false;
    s_pending_store = false;
    s_attempts = 0;
    s_last_reason = 0;
    s_has_ip = false;
    s_state = WIFI_STA_UNCONFIGURED;
    xSemaphoreGive(s_lock);
    esp_wifi_disconnect();
    ESP_LOGI(TAG, "credentials cleared%s", err == ESP_OK ? "" : " (NVS erase failed)");
    return err;
}

void wifi_sta_get_status(wifi_sta_status_t *st)
{
    memset(st, 0, sizeof(*st));
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    st->state = s_state;
    st->configured = s_configured;
    st->stored = s_stored;
    strlcpy(st->ssid, s_ssid, sizeof(st->ssid));
    st->has_ip = s_has_ip;
    st->ip = s_ip;
    st->attempts = s_attempts;
    st->last_reason = s_last_reason;
    xSemaphoreGive(s_lock);

    esp_netif_get_mac(s_netif, st->mac);
    if (st->state == WIFI_STA_CONNECTED) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            st->rssi = ap.rssi;
        }
    }
    if (st->state == WIFI_STA_RETRYING && esp_timer_is_active(s_retry_timer)) {
        uint64_t expiry = 0;
        if (esp_timer_get_expiry_time(s_retry_timer, &expiry) == ESP_OK) {
            const int64_t left = (int64_t)expiry - esp_timer_get_time();
            st->retry_in_ms = left > 0 ? (uint32_t)(left / 1000) : 0;
        }
    }
}
