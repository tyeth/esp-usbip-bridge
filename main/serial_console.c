#include "serial_console.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_netif_ip_addr.h"
#include "esp_system.h"
#include "sdkconfig.h"

#include "wifi_sta.h"

#define CONSOLE_UART         CONFIG_ESP_CONSOLE_UART_NUM
#define CONSOLE_RX_BUF       512
#define CONSOLE_LINE_MAX     160
#define CONSOLE_MAX_ARGS     4
#define CONSOLE_TASK_STACK   4096
#define CONSOLE_TASK_PRIO    2
#define CONSOLE_SET_WAIT_MS  0      /* wifi set answers at once; the IP shows in the log */

static const char *TAG = "console";

/* Split line in place into at most max arguments; "..." keeps spaces.
   Returns the count, -1 for an unterminated quote. */
static int split_args(char *line, char **argv, int max)
{
    int argc = 0;
    char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (argc == max) {
            return max + 1;
        }
        if (*p == '"') {
            argv[argc++] = ++p;
            while (*p && *p != '"') {
                p++;
            }
            if (*p != '"') {
                return -1;
            }
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') {
                p++;
            }
        }
        if (*p) {
            *p++ = '\0';
        }
    }
    return argc;
}

/* Index of the argument the next character of line[0..len) belongs to
   (counting a quoted argument as one), for masking the password echo. */
static int arg_index_at(const char *line, size_t len)
{
    int idx = -1;
    bool in_arg = false;
    bool quoted = false;
    for (size_t i = 0; i < len; i++) {
        const char c = line[i];
        if (quoted) {
            quoted = (c != '"');
            in_arg = quoted;
            continue;
        }
        if (c == ' ' || c == '\t') {
            in_arg = false;
        } else if (!in_arg) {
            idx++;
            in_arg = true;
            quoted = (c == '"');
        }
    }
    return in_arg ? idx : idx + 1;
}

static void print_wifi_status(void)
{
    wifi_sta_status_t st;
    wifi_sta_get_status(&st);
    if (!st.configured) {
        printf("wifi: not configured (wifi set <ssid> <password>)\n");
        return;
    }
    if (st.state == WIFI_STA_CONNECTED && st.has_ip) {
        esp_ip4_addr_t ip = { .addr = st.ip };
        printf("wifi: connected ssid=\"%s\" ip=" IPSTR " rssi=%d%s\n",
               st.ssid, IP2STR(&ip), st.rssi, st.stored ? "" : " (not stored yet)");
        return;
    }
    printf("wifi: %s ssid=\"%s\" attempts=%lu last_reason=%d (%s)%s\n",
           wifi_sta_state_name(st.state), st.ssid, (unsigned long)st.attempts,
           st.last_reason, wifi_sta_reason_name(st.last_reason), st.stored ? "" : " (not stored yet)");
}

static void run_line(char *line)
{
    char *argv[CONSOLE_MAX_ARGS];
    const int argc = split_args(line, argv, CONSOLE_MAX_ARGS);
    if (argc == 0) {
        return;
    }
    if (argc < 0) {
        printf("console: unterminated quote\n");
        return;
    }

    if (strcmp(argv[0], "wifi") == 0) {
        const char *sub = argc > 1 ? argv[1] : "status";
        if (strcmp(sub, "status") == 0 && argc <= 2) {
            print_wifi_status();
        } else if (strcmp(sub, "set") == 0 && (argc == 3 || argc == 4)) {
            const char *pass = argc == 4 ? argv[3] : "";
            esp_err_t err = wifi_sta_set(argv[2], pass, true, CONSOLE_SET_WAIT_MS, NULL);
            if (err == ESP_OK) {
                printf("wifi: saved ssid=\"%s\" (%s), connecting\n", argv[2], pass[0] ? "password set" : "open");
            } else if (err == ESP_ERR_INVALID_ARG) {
                printf("wifi: error: ssid 1-32 characters, password empty or 8-63 characters\n");
            } else {
                printf("wifi: error: %s\n", esp_err_to_name(err));
            }
            if (argc == 4) {
                memset(argv[3], 0, strlen(argv[3]));
            }
        } else if (strcmp(sub, "clear") == 0 && argc == 2) {
            esp_err_t err = wifi_sta_clear();
            printf("wifi: %s\n", err == ESP_OK ? "cleared" : esp_err_to_name(err));
        } else {
            printf("wifi: usage: wifi status | wifi set <ssid> [<password>] | wifi clear\n");
        }
    } else if (strcmp(argv[0], "reboot") == 0 && argc == 1) {
        printf("reboot: restarting\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else if (strcmp(argv[0], "help") == 0) {
        printf("console: wifi status | wifi set <ssid> [<password>] | wifi clear | reboot"
               " (quote arguments with spaces)\n");
    } else {
        printf("console: unknown command \"%s\" (help)\n", argv[0]);
    }
}

static void console_task(void *arg)
{
    (void)arg;
    char line[CONSOLE_LINE_MAX];
    size_t len = 0;
    bool overflow = false;

    while (true) {
        uint8_t c;
        if (uart_read_bytes(CONSOLE_UART, &c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (len == 0 && !overflow) {
                continue;               /* the \n of a \r\n, or an empty line */
            }
            printf("\n");
            if (overflow) {
                printf("console: line too long (max %d)\n", CONSOLE_LINE_MAX - 1);
            } else {
                line[len] = '\0';
                run_line(line);
            }
            memset(line, 0, sizeof(line));
            len = 0;
            overflow = false;
            fflush(stdout);
            continue;
        }
        if (c == '\b' || c == 0x7f) {
            if (len > 0) {
                len--;
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }
        if (c < 0x20 || c > 0x7e) {
            continue;
        }
        if (len >= sizeof(line) - 1) {
            overflow = true;
            continue;
        }
        /* The fourth argument of "wifi set" is the password. */
        const bool secret = len >= 9 && strncmp(line, "wifi set ", 9) == 0 &&
                            c != ' ' && arg_index_at(line, len) >= 3;
        line[len++] = (char)c;
        putchar(secret ? '*' : c);
        fflush(stdout);
    }
}

esp_err_t serial_console_start(void)
{
    if (!uart_is_driver_installed(CONSOLE_UART)) {
        /* RX only: log output keeps going through the console VFS. */
        esp_err_t err = uart_driver_install(CONSOLE_UART, CONSOLE_RX_BUF, 0, 0, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "uart_driver_install(%d): %s", CONSOLE_UART, esp_err_to_name(err));
            return err;
        }
    }
    if (xTaskCreate(console_task, "console", CONSOLE_TASK_STACK, NULL, CONSOLE_TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "console commands on UART%d: wifi status|set|clear, reboot, help", CONSOLE_UART);
    return ESP_OK;
}
