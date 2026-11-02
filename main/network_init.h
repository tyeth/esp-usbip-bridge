#ifndef NETWORK_INIT_H
#define NETWORK_INIT_H

#include "cJSON.h"
#include "esp_err.h"

/* The primary transport: Ethernet (P4, S31) or the S3's Wi-Fi station. */
esp_err_t network_init_start(void);

/* What may start only after usb_backend_start(): the optional Wi-Fi station
   next to Ethernet and its serial console commands (CONFIG_USBIP_WIFI_STA).
   Never waits for a connection. */
esp_err_t network_init_start_secondary(void);

/* {"transports": [...], "interfaces": [{"name", "up", "ip", "mac", ...}]};
   the Wi-Fi interface adds ssid, rssi and configured.  Never the password. */
cJSON *network_describe(void);

#endif
