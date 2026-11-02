#ifndef SERIAL_CONSOLE_H
#define SERIAL_CONSOLE_H

/*
 * Line commands on the board's console UART (115200, the CP2102N on the
 * S31), read by a low-priority task that never blocks boot:
 *
 *   wifi status                      wifi: connected ssid="..." ip=... rssi=...
 *   wifi set <ssid> [<password>]     store the credentials and connect now
 *   wifi clear                       forget them and disconnect
 *   reboot                           restart the bridge
 *   help
 *
 * An argument with spaces goes in double quotes.  Answers are one line
 * starting with the command's first word ("wifi: ...").  Characters are
 * echoed, except the password, which shows as '*'.
 */

#include "esp_err.h"

esp_err_t serial_console_start(void);

#endif
