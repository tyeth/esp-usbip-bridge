#ifndef NETWORK_INIT_H
#define NETWORK_INIT_H

#include "cJSON.h"
#include "esp_err.h"

/* The primary transport: Ethernet (P4, S31) or the S3's Wi-Fi station.  A
   Wi-Fi-station-only board (CONFIG_USBIP_TRANSPORT_WIFI_STA_ONLY) starts
   nothing here: its station is the secondary start below. */
esp_err_t network_init_start(void);

/* What may start only after usb_backend_start(): the Wi-Fi station next to
   Ethernet, or alone, and its serial console commands (CONFIG_USBIP_WIFI_STA).
   Never waits for a connection. */
esp_err_t network_init_start_secondary(void);

/* {"transports": [...], "interfaces": [{"name", "up", "ip", "mac", ...}]};
   the Wi-Fi interface adds ssid, rssi and configured.  Never the password. */
cJSON *network_describe(void);

#endif
