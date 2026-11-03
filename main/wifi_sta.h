#ifndef WIFI_STA_H
#define WIFI_STA_H

/*
 * Optional Wi-Fi station next to Ethernet (CONFIG_USBIP_WIFI_STA, boards
 * with a radio: the S31), or the only transport
 * (CONFIG_USBIP_TRANSPORT_WIFI_STA_ONLY: the Waveshare ESP32-P4-WIFI6, whose
 * esp_wifi API reaches an ESP32-C6 through esp_wifi_remote/esp_hosted).  Credentials live in NVS namespace "wifi", keys
 * "ssid" and "pass" (the S3 Wi-Fi transport's keys); without them the radio
 * stays idle.  Connecting never blocks: a lost or failed association is
 * retried with a backoff of 1 s doubling to 30 s, for ever.  DHCP, the
 * hostname and mDNS work as on Ethernet, and the servers listen on every
 * interface, so the bridge answers on both.
 *
 * Set from the serial console (serial_console.c), the controller API
 * (GET /api/network, PUT and DELETE /api/network/wifi) and the web page.
 * The password is never returned or logged.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define WIFI_STA_SSID_MAX 32
#define WIFI_STA_PASS_MAX 64

typedef enum {
    WIFI_STA_UNCONFIGURED = 0,  /* no credentials: radio idle */
    WIFI_STA_CONNECTING,        /* associating / waiting for DHCP */
    WIFI_STA_CONNECTED,         /* associated with an IPv4 address */
    WIFI_STA_RETRYING,          /* the last attempt failed; the next is scheduled */
} wifi_sta_state_t;

typedef struct {
    wifi_sta_state_t state;
    bool configured;            /* credentials in use (stored or applied) */
    bool stored;                /* the credentials in use are the ones in NVS */
    char ssid[WIFI_STA_SSID_MAX + 1];
    bool has_ip;
    uint32_t ip;                /* IPv4, network byte order (esp_ip4_addr_t.addr) */
    int8_t rssi;                /* valid while connected */
    uint8_t mac[6];
    uint32_t attempts;          /* failed attempts since the last connect */
    int last_reason;            /* wifi_err_reason_t of the last disconnect, 0 for none */
    uint32_t retry_in_ms;       /* RETRYING: time until the next attempt */
} wifi_sta_status_t;

/* Create the station netif, start the driver and, with stored credentials,
   the first connection attempt.  Returns at once; call after
   usb_backend_start() so Wi-Fi bring-up never delays the USB host. */
esp_err_t wifi_sta_start(void);

/* Use new credentials now (an empty password joins an open network).
   store: write them to NVS at once; otherwise they are written once the
   station gets an address with them.  Waits up to wait_ms for that
   address (0: do not wait); *status (may be NULL) is the state afterwards.
   ESP_ERR_INVALID_ARG for an empty or over-long SSID or password,
   ESP_ERR_INVALID_STATE before wifi_sta_start(). */
esp_err_t wifi_sta_set(const char *ssid, const char *password, bool store, uint32_t wait_ms,
                       wifi_sta_status_t *status);

/* Forget the credentials (NVS too) and disconnect. */
esp_err_t wifi_sta_clear(void);

void wifi_sta_get_status(wifi_sta_status_t *status);

/* "unconfigured", "connecting", "connected", "retrying" */
const char *wifi_sta_state_name(wifi_sta_state_t state);

/* Short text for a disconnect reason (e.g. "auth_fail", "no_ap_found"). */
const char *wifi_sta_reason_name(int reason);

#endif
