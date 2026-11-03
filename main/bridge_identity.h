#ifndef BRIDGE_IDENTITY_H
#define BRIDGE_IDENTITY_H

/*
 * What a client reads to decide what a bridge can do (see
 * docs/bridge-discovery-and-capabilities.md): the API version published in
 * /api/info, /api/capabilities and the _esp-usbip._tcp TXT record, the
 * board profile name (scripts/build-board.sh) the firmware was built for and
 * the firmware version.
 */

#include <stddef.h>
#include <string.h>

#include "sdkconfig.h"
#include "version_autogen.h"

/* "MAJOR.MINOR": MINOR for additions, MAJOR when a published field,
   endpoint or behaviour changes or goes away. */
#define BRIDGE_API_VERSION "1.0"

/* The scripts/build-board.sh profile of the selected board, NULL for none. */
static inline const char *bridge_board_profile(void)
{
#if CONFIG_USBIP_BOARD_ESPRESSIF_ESP32P4_FUNCTION_EV
    return "p4-function-ev";
#elif CONFIG_USBIP_BOARD_ESPRESSIF_ESP32P4X_FUNCTION_EV
    return "p4x-function-ev";
#elif CONFIG_USBIP_BOARD_M5STACK_POE_P4
    return "m5stack-poe-p4";
#elif CONFIG_USBIP_BOARD_P4HIL_BOARD
    return "p4hil";
#elif CONFIG_USBIP_BOARD_WAVESHARE_ESP32P4_WIFI6
    return "waveshare-p4-wifi6";
#elif CONFIG_USBIP_BOARD_ESPRESSIF_ESP32S3_USB_OTG
    return "s3-usb-otg";
#elif CONFIG_USBIP_BOARD_ESPRESSIF_ESP32S31_FUNCTION_COREBOARD_1
    return "s31-function-coreboard-1";
#else
    return NULL;
#endif
}

/* The firmware version without the build time: HARNESS_FW_VERSION is
   "<git describe> <YYYY-MM-DD HH:MM:SS>". */
static inline void bridge_fw_version(char *buf, size_t len)
{
    strlcpy(buf, HARNESS_FW_VERSION, len);
    char *sp = strchr(buf, ' ');
    if (sp != NULL) {
        *sp = '\0';
    }
}

/* The build time part of HARNESS_FW_VERSION ("" when absent). */
static inline const char *bridge_build_time(void)
{
    const char *sp = strchr(HARNESS_FW_VERSION, ' ');
    return sp ? sp + 1 : "";
}

#endif
