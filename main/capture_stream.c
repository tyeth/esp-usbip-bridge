/*
 * Live serial streams over TCP (CAPTURE_STREAM_PORT, 3241), so an agent or
 * the controller can follow a capture with nc/socat instead of polling:
 *
 *   echo "1-1.2:0 from=epoch ts" | nc <bridge> 3241
 *
 * The first line selects the stream: <busid>[:<port>] then options:
 *   since=<cursor>   start at a cursor (default: the newest byte, "tail")
 *   from=epoch       start where the current epoch (last reset) began
 *   from=oldest      everything still in the buffer
 *   ts               prefix lines with "[seconds.ms] " bridge uptime
 *   token=<token>    the API token; needed to send when one is set
 * The bridge answers "OK <cursor>\n" (or "ERR <reason>\n"), then streams.
 * Bytes the client sends go to the device's serial port (when allowed).
 * A capture must be running (or have run) for the busid; the stream ends
 * when the client disconnects.
 */
#include "capture.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "controller_api.h"

static const char *TAG = "cap_stream";

#define STREAM_MAX_CLIENTS  4
#define STREAM_BUF          4096

static volatile int s_clients;

typedef struct {
    int fd;
} client_t;

static bool send_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        const int n = send(fd, data, len, 0);
        if (n <= 0) {
            return false;
        }
        data += n;
        len -= (size_t)n;
    }
    return true;
}

static void client_task(void *arg)
{
    client_t *c = (client_t *)arg;
    const int fd = c->fd;
    free(c);
    char line[200];
    size_t n = 0;
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (n + 1 < sizeof(line)) {
        char ch;
        if (recv(fd, &ch, 1, 0) != 1) {
            goto done;
        }
        if (ch == '\n') {
            break;
        }
        if (ch != '\r') {
            line[n++] = ch;
        }
    }
    line[n] = '\0';

    char busid[32] = "";
    int port = 0;
    bool ts = false, can_write = controller_api_authorized(NULL);
    uint64_t cursor = UINT64_MAX;
    char *save = NULL;
    for (char *tok = strtok_r(line, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
        if (busid[0] == '\0') {
            char *colon = strchr(tok, ':');
            if (colon) {
                *colon = '\0';
                port = atoi(colon + 1);
            }
            strlcpy(busid, tok, sizeof(busid));
        } else if (strcmp(tok, "ts") == 0) {
            ts = true;
        } else if (strncmp(tok, "since=", 6) == 0) {
            cursor = strtoull(tok + 6, NULL, 10);
        } else if (strcmp(tok, "from=epoch") == 0 || strcmp(tok, "from=oldest") == 0) {
            uint64_t oldest = 0, epoch = 0, total = 0;
            if (capture_cursor_info(busid, port, &oldest, &epoch, &total)) {
                cursor = tok[5] == 'e' ? epoch : oldest;
            }
        } else if (strncmp(tok, "token=", 6) == 0) {
            char auth[80];
            snprintf(auth, sizeof(auth), "Bearer %s", tok + 6);
            can_write = controller_api_authorized(auth);
        }
    }
    uint64_t oldest = 0, epoch = 0, total = 0;
    if (!capture_cursor_info(busid, port, &oldest, &epoch, &total)) {
        const char *err = "ERR no capture on that busid/port (POST /api/usb/devices/<busid>/capture/start)\n";
        send_all(fd, err, strlen(err));
        goto done;
    }
    if (cursor == UINT64_MAX) {
        cursor = total;             /* tail */
    }
    char hello[64];
    snprintf(hello, sizeof(hello), "OK %llu\n", (unsigned long long)cursor);
    if (!send_all(fd, hello, strlen(hello))) {
        goto done;
    }
    ESP_LOGI(TAG, "stream %s:%d from %llu%s", busid, port, (unsigned long long)cursor, can_write ? " (rw)" : "");

    char *buf = malloc(STREAM_BUF);
    if (buf == NULL) {
        goto done;
    }
    tv.tv_sec = 0;
    tv.tv_usec = 50000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (true) {
        bool dropped = false;
        const int len = capture_render(busid, port, &cursor, ts, buf, STREAM_BUF, &dropped);
        if (len < 0) {
            break;                  /* the capture was cleared */
        }
        if (dropped && !send_all(fd, "\r\n[bridge] stream fell behind the buffer, bytes lost\r\n", 54)) {
            break;
        }
        if (len > 0 && !send_all(fd, buf, (size_t)len)) {
            break;
        }
        /* Input from the client goes to the device. */
        uint8_t in[128];
        const int r = recv(fd, in, sizeof(in), 0);
        if (r == 0) {
            break;
        }
        if (r > 0 && can_write) {
            char hex[sizeof(in) * 2 + 1];
            for (int i = 0; i < r; i++) {
                snprintf(&hex[i * 2], 3, "%02x", in[i]);
            }
            cJSON *args = cJSON_CreateObject();
            cJSON_AddStringToObject(args, "hex", hex);
            cJSON *res = NULL;
            capture_send(busid, port, args, &res);
            cJSON_Delete(res);
            cJSON_Delete(args);
        }
        if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }
    }
    free(buf);
done:
    close(fd);
    s_clients--;
    vTaskDelete(NULL);
}

static void server_task(void *arg)
{
    (void)arg;
    const int lfd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (lfd < 0) {
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CAPTURE_STREAM_PORT),
        .sin_addr = { htonl(INADDR_ANY) },
    };
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(lfd, 2) < 0) {
        ESP_LOGE(TAG, "cannot listen on %d", CAPTURE_STREAM_PORT);
        close(lfd);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "serial streams on TCP %d", CAPTURE_STREAM_PORT);
    while (true) {
        const int fd = accept(lfd, NULL, NULL);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (s_clients >= STREAM_MAX_CLIENTS) {
            const char *busy = "ERR too many streams\n";
            send_all(fd, busy, strlen(busy));
            close(fd);
            continue;
        }
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        client_t *c = malloc(sizeof(*c));
        if (c == NULL) {
            close(fd);
            continue;
        }
        c->fd = fd;
        s_clients++;
        if (xTaskCreate(client_task, "cap_client", 5120, c, 3, NULL) != pdPASS) {
            s_clients--;
            free(c);
            close(fd);
        }
    }
}

void capture_stream_start(void)
{
    xTaskCreate(server_task, "cap_stream", 3072, NULL, 3, NULL);
}
