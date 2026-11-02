#include "serial_monitor.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "busid_owner.h"
#include "serial_link.h"

static const char *TAG = "monitor";

#define MON_SLOTS       2
#define MON_RING        (64 * 1024)
#define MON_TX_MAX      1024
#define MON_POLL_MAX    8192
#define MON_TASK_STACK  6144

typedef struct {
    bool used;
    char busid[32];
    volatile bool stop;
    volatile bool running;      /* the task is alive */
    char state[32];             /* "running", "reconnecting", "stopped: ..." */
    int port;
    uint32_t baud;
    int dtr, rts;               /* -1 = leave alone */
    uint8_t *ring;              /* PSRAM */
    uint64_t total;             /* bytes ever written to the ring */
    uint8_t tx[MON_TX_MAX];
    size_t tx_len;
    uint32_t reconnects;
    int64_t started_us;
} monitor_t;

static monitor_t s_mon[MON_SLOTS];
static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_init_lock = portMUX_INITIALIZER_UNLOCKED;

static void lock(void)
{
    if (s_mutex == NULL) {
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        taskENTER_CRITICAL(&s_init_lock);
        if (s_mutex == NULL) {
            s_mutex = m;
            m = NULL;
        }
        taskEXIT_CRITICAL(&s_init_lock);
        if (m != NULL) {
            vSemaphoreDelete(m);
        }
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mutex);
}

static int fail(cJSON **out, int status, const char *msg)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", false);
    cJSON_AddStringToObject(j, "error", msg);
    *out = j;
    return status;
}

static monitor_t *find_locked(const char *busid)
{
    for (int i = 0; i < MON_SLOTS; i++) {
        if (s_mon[i].used && strcmp(s_mon[i].busid, busid) == 0) {
            return &s_mon[i];
        }
    }
    return NULL;
}

static void ring_put_locked(monitor_t *m, const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        m->ring[(m->total + i) % MON_RING] = data[i];
    }
    m->total += n;
}

static void ring_note(monitor_t *m, const char *note)
{
    lock();
    ring_put_locked(m, (const uint8_t *)note, strlen(note));
    unlock();
}

static void set_state(monitor_t *m, const char *s)
{
    lock();
    strlcpy(m->state, s, sizeof(m->state));
    unlock();
}

/* Configure the link as asked: baud, and the lines only when given. */
static int configure(monitor_t *m, serial_link_t *l)
{
    int st = serial_link_prepare(l);
    if (st == 0) {
        st = serial_link_set_baud(l, m->baud);
    }
    if (st == 0 && (m->dtr >= 0 || m->rts >= 0)) {
        bool d, r;
        serial_link_get_lines(l, &d, &r);
        st = serial_link_set_lines(l, m->dtr >= 0 ? m->dtr == 1 : d, m->rts >= 0 ? m->rts == 1 : r);
    }
    return st;
}

static void monitor_task(void *arg)
{
    monitor_t *m = (monitor_t *)arg;
    serial_link_t *l = calloc(1, sizeof(*l));
    uint8_t *buf = malloc(SERIAL_LINK_IN_CHUNK);
    uint8_t *tx = malloc(MON_TX_MAX);
    char msg[128] = "";
    if (l == NULL || buf == NULL || tx == NULL) {
        set_state(m, "stopped: out of memory");
        goto out;
    }
    if (serial_link_open(l, m->busid, m->port, msg, sizeof(msg)) != 0 || configure(m, l) != 0) {
        char s[32];
        snprintf(s, sizeof(s), "stopped: open failed");
        set_state(m, s);
        ring_note(m, "\r\n[bridge: could not open the serial function]\r\n");
        goto out;
    }
    set_state(m, "running");
    while (!m->stop) {
        lock();
        const size_t n_tx = m->tx_len;
        memcpy(tx, m->tx, n_tx);
        m->tx_len = 0;
        unlock();
        if (n_tx > 0) {
            serial_link_write(l, tx, n_tx);
        }
        const int n = serial_link_read(l, buf, SERIAL_LINK_IN_CHUNK, 100);
        if (n > 0) {
            lock();
            ring_put_locked(m, buf, (size_t)n);
            unlock();
            continue;
        }
        if (n == 0) {
            continue;
        }
        /* The device went away (reset, re-enumeration): wait for it on the same busid. */
        ESP_LOGI(TAG, "%s: read %d, waiting for the device", m->busid, n);
        set_state(m, "reconnecting");
        ring_note(m, "\r\n[bridge: device dropped, waiting for it to come back]\r\n");
        const uint32_t old_devnum = l->devnum;
        bool back = false;
        while (!m->stop && !back) {
            vTaskDelay(pdMS_TO_TICKS(200));
            usbip_backend_device_t d;
            if (usb_backend_get_device_by_busid(l->busid, &d) && d.devnum != old_devnum) {
                vTaskDelay(pdMS_TO_TICKS(300));     /* let it finish enumerating */
                back = serial_link_reopen(l, msg, sizeof(msg)) == 0 && configure(m, l) == 0;
            } else if (usb_backend_get_device_by_busid(l->busid, &d) && d.devnum == old_devnum) {
                /* Same enumeration: a transient read error; carry on. */
                back = true;
            }
        }
        if (back) {
            lock();
            m->reconnects++;
            unlock();
            set_state(m, "running");
            ring_note(m, "\r\n[bridge: device back, monitoring]\r\n");
        }
    }
    set_state(m, "stopped");
out:
    if (l != NULL) {
        serial_link_close(l);
    }
    free(l);
    free(buf);
    free(tx);
    busid_owner_end(m->busid, BUSID_OWNER_RESCUE);
    lock();
    m->running = false;
    unlock();
    vTaskDelete(NULL);
}

static void stop_locked_wait(monitor_t *m)
{
    m->stop = true;
    unlock();
    for (int i = 0; i < 100; i++) {         /* the task notices within ~100 ms */
        lock();
        const bool running = m->running;
        unlock();
        if (!running) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    lock();
}

void serial_monitor_stop(const char *busid)
{
    lock();
    monitor_t *m = find_locked(busid);
    if (m != NULL && m->running) {
        stop_locked_wait(m);
    }
    unlock();
}

bool serial_monitor_active(const char *busid)
{
    lock();
    const monitor_t *m = find_locked(busid);
    const bool active = m != NULL && m->running;
    unlock();
    return active;
}

int serial_monitor_start(const char *busid, const cJSON *args, cJSON **out)
{
    if (busid == NULL || busid[0] == '\0' || strlen(busid) > 31) {
        return fail(out, 400, "busid required");
    }
    const cJSON *v;
    int baud = 115200, port = 0, dtr = -1, rts = -1;
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(args, "serial_baud")) ||
            cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(args, "baud"))) {
        baud = (int)v->valuedouble;
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(args, "port"))) {
        port = (int)v->valuedouble;
    }
    if (cJSON_IsBool(v = cJSON_GetObjectItemCaseSensitive(args, "dtr"))) {
        dtr = cJSON_IsTrue(v);
    }
    if (cJSON_IsBool(v = cJSON_GetObjectItemCaseSensitive(args, "rts"))) {
        rts = cJSON_IsTrue(v);
    }
    if (baud == 1200 || baud < 50 || baud > 12000000) {
        return fail(out, 400, "serial_baud 50-12000000, not 1200 (the bootloader touch)");
    }
    serial_monitor_stop(busid);
    busid_owner_info_t owner;
    if (!busid_owner_try_begin(busid, BUSID_OWNER_RESCUE, SERIAL_MONITOR_OWNER, &owner)) {
        char msg[96];
        snprintf(msg, sizeof(msg), owner.kind == BUSID_OWNER_USBIP
                 ? "device is imported over USB/IP by %s; detach it first" : "device is busy (%s)", owner.peer);
        return fail(out, 409, msg);
    }
    lock();
    monitor_t *m = find_locked(busid);
    for (int i = 0; i < MON_SLOTS && m == NULL; i++) {
        if (!s_mon[i].used || !s_mon[i].running) {
            m = &s_mon[i];
        }
    }
    if (m == NULL) {
        unlock();
        busid_owner_end(busid, BUSID_OWNER_RESCUE);
        return fail(out, 409, "two monitors are already running; stop one first");
    }
    uint8_t *ring = (m->ring != NULL) ? m->ring : heap_caps_malloc(MON_RING, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ring == NULL) {
        unlock();
        busid_owner_end(busid, BUSID_OWNER_RESCUE);
        return fail(out, 503, "out of memory");
    }
    const bool same = m->used && strcmp(m->busid, busid) == 0;
    const uint64_t total = same ? m->total : 0;     /* restarting keeps the cursor going */
    memset(m, 0, sizeof(*m));
    m->used = true;
    m->ring = ring;
    m->total = total;
    strlcpy(m->busid, busid, sizeof(m->busid));
    m->port = port;
    m->baud = (uint32_t)baud;
    m->dtr = dtr;
    m->rts = rts;
    m->running = true;
    m->started_us = esp_timer_get_time();
    strlcpy(m->state, "starting", sizeof(m->state));
    unlock();
    if (xTaskCreate(monitor_task, "monitor", MON_TASK_STACK, m, 3, NULL) != pdPASS) {
        lock();
        m->running = false;
        unlock();
        busid_owner_end(busid, BUSID_OWNER_RESCUE);
        return fail(out, 503, "cannot start the monitor task");
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddStringToObject(j, "busid", busid);
    cJSON_AddNumberToObject(j, "serial_baud", baud);
    cJSON_AddNumberToObject(j, "cursor", (double)total);
    *out = j;
    return 200;
}

int serial_monitor_stop_api(const char *busid, cJSON **out)
{
    if (!serial_monitor_active(busid)) {
        return fail(out, 404, "no monitor running on that device");
    }
    serial_monitor_stop(busid);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    *out = j;
    return 200;
}

int serial_monitor_poll(const char *busid, uint64_t since, int max, cJSON **out)
{
    if (max <= 0 || max > MON_POLL_MAX) {
        max = MON_POLL_MAX;
    }
    lock();
    monitor_t *m = find_locked(busid);
    if (m == NULL || m->ring == NULL) {
        unlock();
        return fail(out, 404, "no monitor on that device (start one first)");
    }
    const uint64_t oldest = m->total > MON_RING ? m->total - MON_RING : 0;
    const bool dropped = since < oldest;
    if (since < oldest || since > m->total) {
        since = since > m->total ? m->total : oldest;
    }
    size_t n = (size_t)(m->total - since);
    if (n > (size_t)max) {
        n = (size_t)max;
    }
    uint8_t *raw = malloc(n + 1);
    if (raw == NULL) {
        unlock();
        return fail(out, 503, "out of memory");
    }
    for (size_t i = 0; i < n; i++) {
        raw[i] = m->ring[(since + i) % MON_RING];
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddStringToObject(j, "state", m->running ? m->state : "stopped");
    cJSON_AddBoolToObject(j, "running", m->running);
    cJSON_AddNumberToObject(j, "serial_baud", m->baud);
    cJSON_AddNumberToObject(j, "reconnects", m->reconnects);
    cJSON_AddNumberToObject(j, "cursor", (double)since);
    cJSON_AddNumberToObject(j, "next", (double)(since + n));
    cJSON_AddNumberToObject(j, "total", (double)m->total);
    cJSON_AddBoolToObject(j, "dropped", dropped);
    unlock();
    char *text = malloc(n * 4 + 1);
    if (text != NULL) {
        rescue_sanitize_text(raw, n, text, n * 4 + 1);
        cJSON_AddStringToObject(j, "text", text);
        free(text);
    }
    free(raw);
    *out = j;
    return 200;
}

int serial_monitor_send(const char *busid, const cJSON *args, cJSON **out)
{
    uint8_t data[256];
    int len = -1;
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(args, "text");
    const cJSON *h = cJSON_GetObjectItemCaseSensitive(args, "hex");
    if (cJSON_IsString(t)) {
        len = (int)strlen(t->valuestring);
        if (len > (int)sizeof(data)) {
            return fail(out, 400, "text: at most 256 bytes per send");
        }
        memcpy(data, t->valuestring, (size_t)len);
    } else if (cJSON_IsString(h)) {
        len = rescue_hex_decode(h->valuestring, data, sizeof(data));
    }
    if (len < 0) {
        return fail(out, 400, "give text or hex (at most 256 bytes)");
    }
    lock();
    monitor_t *m = find_locked(busid);
    if (m == NULL || !m->running) {
        unlock();
        return fail(out, 404, "no monitor running on that device");
    }
    if (m->tx_len + (size_t)len > MON_TX_MAX) {
        unlock();
        return fail(out, 503, "send queue full, try again");
    }
    memcpy(&m->tx[m->tx_len], data, (size_t)len);
    m->tx_len += (size_t)len;
    unlock();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddNumberToObject(j, "queued", len);
    *out = j;
    return 200;
}
