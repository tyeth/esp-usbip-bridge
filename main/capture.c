#include "capture.h"

#include <errno.h>
#include <stdarg.h>
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
#include "nvs.h"

#include "blob_store.h"
#include "busid_owner.h"
#include "rescue_job.h"
#include "rescue_logic.h"
#include "serial_link.h"
#include "serial_monitor.h"

static const char *TAG = "capture";

#define CAP_MAX_DEVS        4
#define CAP_MAX_PORTS       4
#define CAP_MAX_ARMED       8
#define CAP_MAX_BAUDS       8
#define CAP_INDEX_LEN       2048        /* timestamp index entries per port */
#define CAP_INDEX_GAP_MS    20          /* a new index entry at most this often */
#define CAP_TX_MAX          1024
#define CAP_TASK_STACK      6144
#define CAP_READ_SLICE_MS   20
#define CAP_PROBE_MS        250
#define CAP_REPROBE_GAP_MS  5000
#define CAP_RECENT          256
#define CAP_DEFAULT_SIZE    (256 * 1024)
#define CAP_DEFAULT_TOTAL   (2 * 1024 * 1024)
#define CAP_MIN_SIZE        (16 * 1024)
#define CAP_MAX_TOTAL       (12 * 1024 * 1024)
#define CAP_NVS_NS          "capture"

typedef struct {
    uint64_t off;
    uint32_t t_ms;
} cap_idx_t;

typedef struct {
    bool used;
    uint8_t *ring;              /* PSRAM, reserved from the shared budget */
    size_t ring_size;
    uint64_t total;             /* bytes ever written */
    uint64_t epoch_start;       /* cursor where the current epoch began */
    cap_idx_t *idx;             /* PSRAM, CAP_INDEX_LEN entries */
    uint32_t idx_count;         /* entries ever written */
    uint32_t baud;
    int last_score;
    uint8_t recent[CAP_RECENT];
    size_t recent_len;
    int64_t last_probe_us;
    uint8_t tx[CAP_TX_MAX];
    size_t tx_len;
    char kind[12];
} cap_port_t;

typedef struct {
    bool used;
    char busid[32];
    volatile bool stop;
    volatile bool running;
    uint32_t epoch;
    char state[40];
    uint32_t bauds[CAP_MAX_BAUDS];
    int nbauds;
    bool auto_baud;
    int dtr, rts;               /* -1 = leave alone */
    size_t size;                /* per port */
    int nports;
    cap_port_t ports[CAP_MAX_PORTS];
    uint32_t reconnects;
    uint16_t vid, pid;
    int64_t started_us;
} cap_dev_t;

typedef struct {
    char busid[32];
    char spec[160];             /* the start arguments, JSON */
} armed_t;

static cap_dev_t s_dev[CAP_MAX_DEVS];
static armed_t s_armed[CAP_MAX_ARMED];
static size_t s_total_limit = CAP_DEFAULT_TOTAL;
static size_t s_default_size = CAP_DEFAULT_SIZE;
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

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static cap_dev_t *find_locked(const char *busid)
{
    for (int i = 0; i < CAP_MAX_DEVS; i++) {
        if (s_dev[i].used && strcmp(s_dev[i].busid, busid) == 0) {
            return &s_dev[i];
        }
    }
    return NULL;
}

static size_t reserved_locked(void)
{
    size_t n = 0;
    for (int i = 0; i < CAP_MAX_DEVS; i++) {
        for (int p = 0; p < CAP_MAX_PORTS; p++) {
            if (s_dev[i].ports[p].ring != NULL) {
                n += s_dev[i].ports[p].ring_size;
            }
        }
    }
    return n;
}

/* ===========================================================================
 *  Rings
 * ======================================================================== */

static void ring_put_locked(cap_port_t *p, const uint8_t *data, size_t n, bool marker)
{
    if (p->ring == NULL || n == 0) {
        return;
    }
    const uint32_t t = now_ms();
    const cap_idx_t *last = p->idx_count ? &p->idx[(p->idx_count - 1) % CAP_INDEX_LEN] : NULL;
    if (last == NULL || marker || t - last->t_ms >= CAP_INDEX_GAP_MS) {
        p->idx[p->idx_count % CAP_INDEX_LEN] = (cap_idx_t){ .off = p->total, .t_ms = t };
        p->idx_count++;
    }
    for (size_t i = 0; i < n; i++) {
        p->ring[(p->total + i) % p->ring_size] = data[i];
    }
    p->total += n;
}

/* A marker line in every port's stream: "\r\n[bridge] ...\r\n". */
static void marker_locked(cap_dev_t *d, int port, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void marker_locked(cap_dev_t *d, int port, const char *fmt, ...)
{
    char line[200];
    int o = snprintf(line, sizeof(line), "\r\n[bridge] ");
    va_list ap;
    va_start(ap, fmt);
    o += vsnprintf(line + o, sizeof(line) - o - 3, fmt, ap);
    va_end(ap);
    strlcat(line, "\r\n", sizeof(line));
    for (int p = 0; p < CAP_MAX_PORTS; p++) {
        if ((port < 0 || p == port) && d->ports[p].ring != NULL) {
            ring_put_locked(&d->ports[p], (const uint8_t *)line, strlen(line), true);
        }
    }
}

/* Time of the byte at off: the newest index entry at or before it. */
static uint32_t time_of_locked(const cap_port_t *p, uint64_t off)
{
    const uint32_t n = p->idx_count < CAP_INDEX_LEN ? p->idx_count : CAP_INDEX_LEN;
    uint32_t best = 0;
    for (uint32_t k = 0; k < n; k++) {
        const cap_idx_t *e = &p->idx[(p->idx_count - 1 - k) % CAP_INDEX_LEN];
        if (e->off <= off) {
            return e->t_ms;
        }
        best = e->t_ms;
    }
    return best;
}

/* Make sure port has a ring of d->size; returns false when the budget is spent. */
static bool ensure_ring_locked(cap_dev_t *d, int port)
{
    cap_port_t *p = &d->ports[port];
    if (p->ring != NULL) {
        return true;
    }
    size_t want = d->size;
    const size_t used = reserved_locked();
    if (used + want > s_total_limit) {
        want = s_total_limit > used ? s_total_limit - used : 0;
    }
    if (want < CAP_MIN_SIZE) {
        return false;
    }
    p->ring = blob_reserve(want, d->busid);
    p->idx = heap_caps_calloc(CAP_INDEX_LEN, sizeof(cap_idx_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p->ring == NULL || p->idx == NULL) {
        blob_unreserve(p->ring, want);
        heap_caps_free(p->idx);
        p->ring = NULL;
        p->idx = NULL;
        return false;
    }
    p->ring_size = want;
    p->total = 0;
    p->epoch_start = 0;
    p->idx_count = 0;
    p->used = true;
    return true;
}

static void free_rings_locked(cap_dev_t *d)
{
    for (int p = 0; p < CAP_MAX_PORTS; p++) {
        blob_unreserve(d->ports[p].ring, d->ports[p].ring_size);
        heap_caps_free(d->ports[p].idx);
        memset(&d->ports[p], 0, sizeof(d->ports[p]));
    }
}

/* ===========================================================================
 *  The capture task
 * ======================================================================== */

typedef struct {
    serial_link_t link;
    bool open;
} cap_link_t;

static bool uart_bridge(const serial_link_t *l)
{
    return l->caps.serial == RESCUE_SERIAL_CP210X || l->caps.serial == RESCUE_SERIAL_CH34X ||
           l->caps.serial == RESCUE_SERIAL_FTDI || (l->caps.serial == RESCUE_SERIAL_CDC_ACM && l->vid == 0x1a86);
}

/* Candidate rates in order: the requested ones, then (auto) the defaults. */
static int candidates(const cap_dev_t *d, uint32_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < d->nbauds && n < max; i++) {
        out[n++] = d->bauds[i];
    }
    if (d->auto_baud) {
        for (int i = 0; i < k_rescue_num_auto_bauds && n < max; i++) {
            bool dup = false;
            for (int k = 0; k < n; k++) {
                dup |= out[k] == k_rescue_auto_bauds[i];
            }
            if (!dup) {
                out[n++] = k_rescue_auto_bauds[i];
            }
        }
    }
    if (n == 0) {
        out[n++] = 115200;
    }
    return n;
}

/* Try the candidate rates on a UART bridge and keep the first that reads
   as text (or the best, or the first when the line is silent). */
static uint32_t probe_baud(cap_dev_t *d, cap_link_t *cl, int port)
{
    uint32_t cand[24];
    const int n = candidates(d, cand, 24);
    if (!uart_bridge(&cl->link) || n == 1) {
        serial_link_set_baud(&cl->link, cand[0]);
        return cand[0];
    }
    uint8_t *buf = malloc(512);
    uint32_t best = cand[0];
    int best_score = -1;
    for (int i = 0; i < n && buf != NULL && !d->stop; i++) {
        if (serial_link_set_baud(&cl->link, cand[i]) != 0) {
            continue;
        }
        serial_link_discard_input(&cl->link);
        size_t got = 0;
        const int64_t until = esp_timer_get_time() + CAP_PROBE_MS * 1000;
        while (got < 512 && esp_timer_get_time() < until) {
            const int r = serial_link_read(&cl->link, buf + got, 512 - got, CAP_READ_SLICE_MS);
            if (r < 0) {
                break;
            }
            got += (size_t)r;
        }
        const int score = rescue_baud_score(buf, got);
        if (score > best_score) {
            best_score = score;
            best = cand[i];
        }
        if (score >= RESCUE_BAUD_SCORE_GOOD) {
            break;
        }
    }
    free(buf);
    serial_link_set_baud(&cl->link, best);
    lock();
    d->ports[port].last_score = best_score;
    marker_locked(d, port, "baud %" PRIu32 " (%s, score %d)", best,
                  best_score < 0 ? "line silent, first candidate" : "auto", best_score);
    unlock();
    return best;
}

static void configure_link(cap_dev_t *d, cap_link_t *cl, int port)
{
    serial_link_prepare(&cl->link);
    if (d->dtr >= 0 || d->rts >= 0) {
        bool dd, rr;
        serial_link_get_lines(&cl->link, &dd, &rr);
        serial_link_set_lines(&cl->link, d->dtr >= 0 ? d->dtr == 1 : dd, d->rts >= 0 ? d->rts == 1 : rr);
    }
    const uint32_t baud = probe_baud(d, cl, port);
    lock();
    d->ports[port].baud = baud;
    d->ports[port].last_probe_us = esp_timer_get_time();
    strlcpy(d->ports[port].kind, rescue_serial_kind_name(cl->link.caps.serial), sizeof(d->ports[port].kind));
    unlock();
}

/* Open every serial function of the device.  Returns the number opened. */
static int open_all(cap_dev_t *d, cap_link_t *links)
{
    char msg[96];
    int n = 0;
    for (int p = 0; p < CAP_MAX_PORTS; p++) {
        cap_link_t *cl = &links[p];
        if (serial_link_open(&cl->link, d->busid, p, msg, sizeof(msg)) != 0) {
            cl->open = false;
            if (p >= cl->link.caps.serial_ports) {
                break;
            }
            continue;
        }
        cl->link.keep_claims = true;
        cl->open = true;
        n++;
        if (p + 1 >= cl->link.caps.serial_ports) {
            break;
        }
    }
    return n;
}

static void close_all(cap_dev_t *d, cap_link_t *links)
{
    for (int p = 0; p < CAP_MAX_PORTS; p++) {
        if (links[p].open) {
            serial_link_close(&links[p].link);
            links[p].open = false;
        }
    }
    /* Release the claimed interfaces once, after every port stopped reading. */
    usb_backend_session_ended(d->busid);
}

static void set_state(cap_dev_t *d, const char *s)
{
    lock();
    strlcpy(d->state, s, sizeof(d->state));
    unlock();
}

/* Open and configure; returns the port count (0: nothing to capture). */
static int bring_up(cap_dev_t *d, cap_link_t *links)
{
    const int n = open_all(d, links);
    usbip_backend_device_t dev;
    const bool present = usb_backend_get_device_by_busid(d->busid, &dev);
    lock();
    d->nports = 0;
    for (int p = 0; p < CAP_MAX_PORTS; p++) {
        if (links[p].open && ensure_ring_locked(d, p)) {
            d->nports = p + 1;
        } else if (links[p].open) {
            links[p].open = false;
            serial_link_close(&links[p].link);
            ESP_LOGW(TAG, "%s port %d: no buffer left in the capture budget", d->busid, p);
        }
    }
    if (present) {
        d->vid = dev.id_vendor;
        d->pid = dev.id_product;
    }
    unlock();
    if (n == 0) {
        return 0;
    }
    for (int p = 0; p < CAP_MAX_PORTS; p++) {
        if (links[p].open) {
            configure_link(d, &links[p], p);
            lock();
            d->ports[p].epoch_start = d->ports[p].total;
            marker_locked(d, p, "epoch %" PRIu32 " at +%" PRIu32 " ms: %04x:%04x %s port %d of %d, baud %" PRIu32,
                          d->epoch, now_ms(), d->vid, d->pid, d->ports[p].kind, p, d->nports, d->ports[p].baud);
            unlock();
        }
    }
    return n;
}

static void capture_task(void *arg)
{
    cap_dev_t *d = (cap_dev_t *)arg;
    cap_link_t *links = calloc(CAP_MAX_PORTS, sizeof(cap_link_t));
    uint8_t *buf = malloc(SERIAL_LINK_IN_CHUNK);
    uint8_t *tx = malloc(CAP_TX_MAX);
    if (links == NULL || buf == NULL || tx == NULL) {
        set_state(d, "stopped: out of memory");
        goto out;
    }
    if (bring_up(d, links) == 0) {
        set_state(d, "waiting for a serial function");
    } else {
        set_state(d, "running");
    }
    while (!d->stop) {
        bool any = false, gone = false;
        for (int p = 0; p < CAP_MAX_PORTS && !d->stop; p++) {
            cap_link_t *cl = &links[p];
            if (!cl->open) {
                continue;
            }
            any = true;
            lock();
            const size_t ntx = d->ports[p].tx_len;
            memcpy(tx, d->ports[p].tx, ntx);
            d->ports[p].tx_len = 0;
            unlock();
            if (ntx > 0) {
                serial_link_write(&cl->link, tx, ntx);
            }
            const int r = serial_link_read(&cl->link, buf, SERIAL_LINK_IN_CHUNK, CAP_READ_SLICE_MS);
            if (r < 0) {
                gone = true;
                break;
            }
            if (r == 0) {
                continue;
            }
            bool reprobe = false;
            lock();
            cap_port_t *cp = &d->ports[p];
            ring_put_locked(cp, buf, (size_t)r, false);
            /* Watch the text quality: an app that switches rate turns to garbage. */
            for (int i = 0; i < r; i++) {
                if (cp->recent_len == CAP_RECENT) {
                    memmove(cp->recent, cp->recent + 1, CAP_RECENT - 1);
                    cp->recent_len--;
                }
                cp->recent[cp->recent_len++] = buf[i];
            }
            if (d->auto_baud && uart_bridge(&cl->link) && cp->recent_len >= CAP_RECENT / 2 &&
                    esp_timer_get_time() - cp->last_probe_us > (int64_t)CAP_REPROBE_GAP_MS * 1000) {
                const int score = rescue_baud_score(cp->recent, cp->recent_len);
                if (score < RESCUE_BAUD_SCORE_BAD) {
                    marker_locked(d, p, "output looks like the wrong baud (score %d), probing again", score);
                    cp->recent_len = 0;
                    reprobe = true;
                }
            }
            unlock();
            if (reprobe) {
                configure_link(d, cl, p);
            }
        }
        if (!any && !gone) {
            gone = true;            /* nothing open: wait for a serial function */
        }
        if (!gone) {
            continue;
        }
        /* Dropped, or nothing to read yet: wait for the device on the same busid. */
        if (any) {
            set_state(d, "waiting for the device");
            lock();
            marker_locked(d, -1, "device dropped at +%" PRIu32 " ms, waiting for it", now_ms());
            unlock();
            close_all(d, links);
        }
        uint32_t old_devnum = 0;
        usbip_backend_device_t dev;
        if (usb_backend_get_device_by_busid(d->busid, &dev)) {
            old_devnum = dev.devnum;
        }
        bool back = false;
        int quiet = 0;
        while (!d->stop && !back) {
            vTaskDelay(pdMS_TO_TICKS(200));
            if (!usb_backend_get_device_by_busid(d->busid, &dev)) {
                quiet = 0;
                continue;
            }
            /* A new enumeration, or the same one after a transient error (1 s). */
            if (dev.devnum != old_devnum || ++quiet >= 5) {
                vTaskDelay(pdMS_TO_TICKS(300));
                back = true;
            }
        }
        if (d->stop) {
            break;
        }
        if (any) {
            lock();
            d->epoch++;
            d->reconnects++;
            unlock();
        }
        if (bring_up(d, links) > 0) {
            set_state(d, "running");
        }
    }
    set_state(d, "stopped");
out:
    if (links != NULL) {
        close_all(d, links);
    }
    free(links);
    free(buf);
    free(tx);
    busid_owner_end(d->busid, BUSID_OWNER_RESCUE);
    lock();
    d->running = false;
    unlock();
    vTaskDelete(NULL);
}

/* ===========================================================================
 *  Start / stop
 * ======================================================================== */

static void stop_wait(cap_dev_t *d)
{
    lock();
    d->stop = true;
    unlock();
    for (int i = 0; i < 150; i++) {
        lock();
        const bool running = d->running;
        unlock();
        if (!running) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void capture_halt(const char *busid)
{
    lock();
    cap_dev_t *d = find_locked(busid);
    const bool running = d != NULL && d->running;
    unlock();
    if (running) {
        stop_wait(d);
    }
}

bool capture_active(const char *busid)
{
    lock();
    const cap_dev_t *d = find_locked(busid);
    const bool a = d != NULL && d->running;
    unlock();
    return a;
}

static void save_armed_locked(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < CAP_MAX_ARMED; i++) {
        if (s_armed[i].busid[0]) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "busid", s_armed[i].busid);
            cJSON_AddItemToObject(o, "spec", cJSON_Parse(s_armed[i].spec));
            cJSON_AddItemToArray(arr, o);
        }
    }
    char *txt = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    nvs_handle_t h;
    if (txt != NULL && nvs_open(CAP_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "armed", txt);
        nvs_set_u32(h, "total", (uint32_t)s_total_limit);
        nvs_set_u32(h, "size", (uint32_t)s_default_size);
        nvs_commit(h);
        nvs_close(h);
    }
    cJSON_free(txt);
}

static void set_armed_locked(const char *busid, const cJSON *spec, bool arm)
{
    int slot = -1;
    for (int i = 0; i < CAP_MAX_ARMED; i++) {
        if (strcmp(s_armed[i].busid, busid) == 0) {
            slot = i;
        }
    }
    if (!arm) {
        if (slot >= 0) {
            memset(&s_armed[slot], 0, sizeof(s_armed[slot]));
        }
        return;
    }
    for (int i = 0; i < CAP_MAX_ARMED && slot < 0; i++) {
        if (s_armed[i].busid[0] == '\0') {
            slot = i;
        }
    }
    if (slot < 0) {
        return;
    }
    strlcpy(s_armed[slot].busid, busid, sizeof(s_armed[slot].busid));
    /* Keep only what start needs, without "arm" (the watcher re-starts it). */
    cJSON *copy = spec ? cJSON_Duplicate(spec, true) : cJSON_CreateObject();
    cJSON_DeleteItemFromObject(copy, "arm");
    char *txt = cJSON_PrintUnformatted(copy);
    cJSON_Delete(copy);
    strlcpy(s_armed[slot].spec, txt ? txt : "{}", sizeof(s_armed[slot].spec));
    cJSON_free(txt);
}

static int parse_bauds(const cJSON *v, uint32_t *bauds, int *n, bool *auto_baud)
{
    *n = 0;
    *auto_baud = false;
    if (v == NULL || cJSON_IsNull(v)) {
        bauds[(*n)++] = 115200;
        return 0;
    }
    if (cJSON_IsString(v) && strcmp(v->valuestring, "auto") == 0) {
        *auto_baud = true;
        return 0;
    }
    if (cJSON_IsNumber(v)) {
        bauds[(*n)++] = (uint32_t)v->valuedouble;
        return 0;
    }
    if (cJSON_IsArray(v)) {
        const cJSON *e;
        cJSON_ArrayForEach(e, v) {
            if (cJSON_IsString(e) && strcmp(e->valuestring, "auto") == 0) {
                *auto_baud = true;      /* [921600, "auto"]: try it first, then the defaults */
            } else if (cJSON_IsNumber(e) && *n < CAP_MAX_BAUDS) {
                bauds[(*n)++] = (uint32_t)e->valuedouble;
            } else {
                return -1;
            }
        }
        return 0;
    }
    return -1;
}

int capture_start(const char *busid, const cJSON *args, cJSON **out)
{
    if (busid == NULL || busid[0] == '\0' || strlen(busid) > 31) {
        return fail(out, 400, "busid required");
    }
    const cJSON *src = cJSON_GetObjectItemCaseSensitive(args, "source");
    if (cJSON_IsString(src) && strcmp(src->valuestring, "usb") != 0) {
        return fail(out, strcmp(src->valuestring, "gpio_uart") == 0 ? 501 : 400,
                    strcmp(src->valuestring, "gpio_uart") == 0
                    ? "source gpio_uart (a bridge UART on GPIO pins: {uart, rx_gpio, tx_gpio, baud}) is planned, not implemented yet"
                    : "source: usb (or gpio_uart, planned)");
    }
    uint32_t bauds[CAP_MAX_BAUDS];
    int nb = 0;
    bool auto_baud = false;
    const cJSON *bv = cJSON_GetObjectItemCaseSensitive(args, "baud");
    if (bv == NULL) {
        bv = cJSON_GetObjectItemCaseSensitive(args, "serial_baud");
    }
    if (parse_bauds(bv, bauds, &nb, &auto_baud) != 0) {
        return fail(out, 400, "baud: a rate, a list of rates (optionally ending with \"auto\"), or \"auto\"");
    }
    for (int i = 0; i < nb; i++) {
        if (bauds[i] == 1200 || bauds[i] < 50 || bauds[i] > 12000000) {
            return fail(out, 400, "rates 50-12000000, not 1200 (the bootloader touch)");
        }
    }
    const cJSON *v;
    size_t size = 0;
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(args, "size"))) {
        size = (size_t)v->valuedouble;
    }
    int dtr = -1, rts = -1;
    if (cJSON_IsBool(v = cJSON_GetObjectItemCaseSensitive(args, "dtr"))) {
        dtr = cJSON_IsTrue(v);
    }
    if (cJSON_IsBool(v = cJSON_GetObjectItemCaseSensitive(args, "rts"))) {
        rts = cJSON_IsTrue(v);
    }
    const bool arm = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "arm"));
    if (rescue_job_busy(busid)) {
        return fail(out, 409, "a rescue job is running on this device (capture_after in the job starts one when it ends)");
    }

    capture_halt(busid);
    busid_owner_info_t owner;
    if (!busid_owner_try_begin(busid, BUSID_OWNER_RESCUE, SERIAL_MONITOR_OWNER, &owner)) {
        char msg[96];
        snprintf(msg, sizeof(msg), owner.kind == BUSID_OWNER_USBIP
                 ? "device is imported over USB/IP by %s; detach it first" : "device is busy (%s)", owner.peer);
        return fail(out, 409, msg);
    }
    lock();
    if (size == 0) {
        size = s_default_size;
    }
    if (size < CAP_MIN_SIZE || size > s_total_limit) {
        unlock();
        busid_owner_end(busid, BUSID_OWNER_RESCUE);
        char msg[96];
        snprintf(msg, sizeof(msg), "size %u-%u bytes per port (capture.total_limit)", (unsigned)CAP_MIN_SIZE,
                 (unsigned)s_total_limit);
        return fail(out, 400, msg);
    }
    cap_dev_t *d = find_locked(busid);
    if (d == NULL) {
        for (int i = 0; i < CAP_MAX_DEVS && d == NULL; i++) {
            if (!s_dev[i].used) {
                d = &s_dev[i];
            }
        }
        /* Reuse a stopped capture's slot (its buffers go). */
        for (int i = 0; i < CAP_MAX_DEVS && d == NULL; i++) {
            if (!s_dev[i].running) {
                d = &s_dev[i];
                free_rings_locked(d);
            }
        }
    }
    if (d == NULL) {
        unlock();
        busid_owner_end(busid, BUSID_OWNER_RESCUE);
        return fail(out, 409, "4 captures are running; stop one first");
    }
    const bool same = d->used && strcmp(d->busid, busid) == 0;
    if (same && d->size != size) {
        free_rings_locked(d);       /* a new size: new buffers */
    } else if (!same) {
        free_rings_locked(d);
    }
    const uint32_t epoch = same ? d->epoch + 1 : 1;
    d->used = true;
    strlcpy(d->busid, busid, sizeof(d->busid));
    d->stop = false;
    d->running = true;
    d->epoch = epoch;
    d->size = size;
    memcpy(d->bauds, bauds, sizeof(bauds));
    d->nbauds = nb;
    d->auto_baud = auto_baud;
    d->dtr = dtr;
    d->rts = rts;
    d->reconnects = 0;
    d->started_us = esp_timer_get_time();
    strlcpy(d->state, "starting", sizeof(d->state));
    if (arm) {                      /* disarming is stop's job: a watcher restart keeps it */
        set_armed_locked(busid, args, true);
        save_armed_locked();
    }
    unlock();
    if (xTaskCreate(capture_task, "capture", CAP_TASK_STACK, d, 3, NULL) != pdPASS) {
        lock();
        d->running = false;
        unlock();
        busid_owner_end(busid, BUSID_OWNER_RESCUE);
        return fail(out, 503, "cannot start the capture task");
    }
    /* Report where this epoch starts once the ports are up (or soon after). */
    vTaskDelay(pdMS_TO_TICKS(50));
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddStringToObject(j, "busid", busid);
    cJSON_AddNumberToObject(j, "epoch", epoch);
    cJSON_AddNumberToObject(j, "size_per_port", (double)size);
    cJSON_AddBoolToObject(j, "armed", arm);
    lock();
    cJSON *cur = cJSON_AddArrayToObject(j, "cursors");
    for (int p = 0; p < CAP_MAX_PORTS; p++) {
        if (d->ports[p].ring != NULL) {
            cJSON_AddItemToArray(cur, cJSON_CreateNumber((double)d->ports[p].total));
        }
    }
    cJSON_AddNumberToObject(j, "cursor", d->ports[0].ring ? (double)d->ports[0].total : 0);
    unlock();
    cJSON_AddStringToObject(j, "note", "read each port from its cursor (or ?from=epoch); the ports appear as the device enumerates");
    *out = j;
    return 200;
}

int capture_stop(const char *busid, const cJSON *args, cJSON **out)
{
    const bool disarm = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "disarm"));
    const bool clear = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "clear"));
    lock();
    cap_dev_t *d = find_locked(busid);
    bool was_armed = false;
    for (int i = 0; i < CAP_MAX_ARMED; i++) {
        was_armed |= strcmp(s_armed[i].busid, busid) == 0;
    }
    if (disarm && was_armed) {
        set_armed_locked(busid, NULL, false);
        save_armed_locked();
    }
    unlock();
    if (d == NULL && !was_armed) {
        return fail(out, 404, "no capture on that device");
    }
    if (d != NULL) {
        capture_halt(busid);
        if (clear) {
            lock();
            free_rings_locked(d);
            d->used = false;
            unlock();
        }
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddBoolToObject(j, "armed", was_armed && !disarm);
    if (was_armed && !disarm) {
        cJSON_AddStringToObject(j, "note", "still armed: it starts again when the device next appears (disarm: true to stop that)");
    }
    *out = j;
    return 200;
}

/* ===========================================================================
 *  Reading
 * ======================================================================== */

static cap_port_t *port_locked(const char *busid, int port)
{
    cap_dev_t *d = find_locked(busid);
    if (d == NULL || port < 0 || port >= CAP_MAX_PORTS || d->ports[port].ring == NULL) {
        return NULL;
    }
    return &d->ports[port];
}

bool capture_cursor_info(const char *busid, int port, uint64_t *oldest, uint64_t *epoch_start, uint64_t *total)
{
    lock();
    const cap_port_t *p = port_locked(busid, port);
    if (p != NULL) {
        *total = p->total;
        *oldest = p->total > p->ring_size ? p->total - p->ring_size : 0;
        *epoch_start = p->epoch_start;
    }
    unlock();
    return p != NULL;
}

int capture_render(const char *busid, int port, uint64_t *cursor, bool timestamps,
                   char *out, size_t cap, bool *dropped)
{
    *dropped = false;
    if (cap < 64) {
        return -1;
    }
    lock();
    const cap_port_t *p = port_locked(busid, port);
    if (p == NULL) {
        unlock();
        return -1;
    }
    const uint64_t oldest = p->total > p->ring_size ? p->total - p->ring_size : 0;
    if (*cursor < oldest) {
        *dropped = true;
        *cursor = oldest;
    }
    if (*cursor > p->total) {
        *cursor = p->total;
    }
    /* Raw bytes first (a bounded copy), then sanitise and stamp outside the lock. */
    size_t n = (size_t)(p->total - *cursor);
    const size_t raw_cap = timestamps ? cap / 3 : cap / 4;
    if (n > raw_cap) {
        n = raw_cap;
    }
    uint8_t *raw = malloc(n + 1);
    if (raw == NULL) {
        unlock();
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        raw[i] = p->ring[(*cursor + i) % p->ring_size];
    }
    /* Line starts: the byte after each '\n' (and the first byte when the
       previous one was a newline). */
    bool at_line_start = *cursor == 0 || p->ring[(*cursor - 1) % p->ring_size] == '\n';
    const uint64_t base = *cursor;
    char *text = malloc(n * 4 + 1);
    size_t o = 0;
    if (text != NULL) {
        size_t i = 0;
        while (i < n && o + 32 < cap) {
            if (timestamps && at_line_start) {
                const uint32_t t = time_of_locked(p, base + i);
                o += (size_t)snprintf(out + o, cap - o, "[%5" PRIu32 ".%03" PRIu32 "] ", t / 1000, t % 1000);
            }
            /* Up to the end of this line. */
            size_t e = i;
            while (e < n && raw[e] != '\n') {
                e++;
            }
            if (e < n) {
                e++;                /* include the '\n' */
            }
            const size_t tl = rescue_sanitize_text(&raw[i], e - i, text, n * 4 + 1);
            if (o + tl + 1 >= cap) {
                break;
            }
            memcpy(out + o, text, tl);
            o += tl;
            at_line_start = e > i && raw[e - 1] == '\n';
            i = e;
        }
        *cursor = base + i;
    }
    unlock();
    out[o] = '\0';
    free(raw);
    free(text);
    return (int)o;
}

int capture_poll(const char *busid, int port, uint64_t since, int max, bool lines, cJSON **out)
{
    if (max <= 0 || max > 16384) {
        max = 16384;
    }
    lock();
    cap_dev_t *d = find_locked(busid);
    if (d == NULL || port_locked(busid, port) == NULL) {
        unlock();
        return fail(out, 404, d ? "no such port in this capture" : "no capture on that device (start one first)");
    }
    const cap_port_t *p = &d->ports[port];
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddStringToObject(j, "busid", busid);
    cJSON_AddNumberToObject(j, "port", port);
    cJSON_AddNumberToObject(j, "ports", d->nports);
    cJSON_AddStringToObject(j, "state", d->running ? d->state : "stopped");
    cJSON_AddBoolToObject(j, "running", d->running);
    cJSON_AddNumberToObject(j, "epoch", d->epoch);
    cJSON_AddNumberToObject(j, "epoch_cursor", (double)p->epoch_start);
    cJSON_AddNumberToObject(j, "serial_baud", p->baud);
    cJSON_AddBoolToObject(j, "auto_baud", d->auto_baud);
    cJSON_AddNumberToObject(j, "reconnects", d->reconnects);
    cJSON_AddNumberToObject(j, "total", (double)p->total);
    cJSON_AddNumberToObject(j, "uptime_ms", now_ms());
    char vp[10];
    snprintf(vp, sizeof(vp), "%04x:%04x", d->vid, d->pid);
    cJSON_AddStringToObject(j, "device", vp);
    unlock();
    char *buf = malloc((size_t)max * 4 + 64);
    if (buf == NULL) {
        cJSON_Delete(j);
        return fail(out, 503, "out of memory");
    }
    uint64_t cur = since;
    bool dropped = false;
    capture_render(busid, port, &cur, lines, buf, (size_t)max * 4 + 64, &dropped);
    cJSON_AddNumberToObject(j, "cursor", (double)since);
    cJSON_AddNumberToObject(j, "next", (double)cur);
    cJSON_AddBoolToObject(j, "dropped", dropped);
    cJSON_AddStringToObject(j, "text", buf);
    free(buf);
    *out = j;
    return 200;
}

int capture_send(const char *busid, int port, const cJSON *args, cJSON **out)
{
    uint8_t data[256];
    int len = -1;
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(args, "text");
    const cJSON *h = cJSON_GetObjectItemCaseSensitive(args, "hex");
    if (cJSON_IsString(t) && strlen(t->valuestring) <= sizeof(data)) {
        len = (int)strlen(t->valuestring);
        memcpy(data, t->valuestring, (size_t)len);
    } else if (cJSON_IsString(h)) {
        len = rescue_hex_decode(h->valuestring, data, sizeof(data));
    }
    if (len < 0) {
        return fail(out, 400, "give text or hex (at most 256 bytes)");
    }
    lock();
    cap_dev_t *d = find_locked(busid);
    if (d == NULL || !d->running || port < 0 || port >= CAP_MAX_PORTS || d->ports[port].ring == NULL) {
        unlock();
        return fail(out, 404, "no running capture on that device/port");
    }
    cap_port_t *p = &d->ports[port];
    if (p->tx_len + (size_t)len > CAP_TX_MAX) {
        unlock();
        return fail(out, 503, "send queue full, try again");
    }
    memcpy(&p->tx[p->tx_len], data, (size_t)len);
    p->tx_len += (size_t)len;
    unlock();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddNumberToObject(j, "queued", len);
    *out = j;
    return 200;
}

/* ===========================================================================
 *  Status, config, arming
 * ======================================================================== */

cJSON *capture_status_json(void)
{
    cJSON *j = cJSON_CreateObject();
    lock();
    cJSON_AddNumberToObject(j, "total_limit", (double)s_total_limit);
    cJSON_AddNumberToObject(j, "default_size", (double)s_default_size);
    cJSON_AddNumberToObject(j, "reserved", (double)reserved_locked());
    cJSON_AddNumberToObject(j, "uptime_ms", now_ms());
    cJSON_AddNumberToObject(j, "stream_port", CAPTURE_STREAM_PORT);
    cJSON *armed = cJSON_AddArrayToObject(j, "armed");
    for (int i = 0; i < CAP_MAX_ARMED; i++) {
        if (s_armed[i].busid[0]) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "busid", s_armed[i].busid);
            cJSON_AddItemToObject(o, "spec", cJSON_Parse(s_armed[i].spec));
            cJSON_AddItemToArray(armed, o);
        }
    }
    cJSON *caps = cJSON_AddArrayToObject(j, "captures");
    for (int i = 0; i < CAP_MAX_DEVS; i++) {
        const cap_dev_t *d = &s_dev[i];
        if (!d->used) {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "busid", d->busid);
        cJSON_AddBoolToObject(o, "running", d->running);
        cJSON_AddStringToObject(o, "state", d->running ? d->state : "stopped");
        cJSON_AddNumberToObject(o, "epoch", d->epoch);
        cJSON_AddNumberToObject(o, "reconnects", d->reconnects);
        char vp[10];
        snprintf(vp, sizeof(vp), "%04x:%04x", d->vid, d->pid);
        cJSON_AddStringToObject(o, "device", vp);
        cJSON *ports = cJSON_AddArrayToObject(o, "ports");
        for (int p = 0; p < CAP_MAX_PORTS; p++) {
            const cap_port_t *cp = &d->ports[p];
            if (cp->ring == NULL) {
                continue;
            }
            cJSON *po = cJSON_CreateObject();
            cJSON_AddNumberToObject(po, "port", p);
            cJSON_AddStringToObject(po, "kind", cp->kind);
            cJSON_AddNumberToObject(po, "baud", cp->baud);
            cJSON_AddNumberToObject(po, "size", (double)cp->ring_size);
            cJSON_AddNumberToObject(po, "total", (double)cp->total);
            cJSON_AddNumberToObject(po, "epoch_cursor", (double)cp->epoch_start);
            char url[96];
            snprintf(url, sizeof(url), "/api/rescue/capture/%s/%d.log", d->busid, p);
            cJSON_AddStringToObject(po, "download", url);
            cJSON_AddItemToArray(ports, po);
        }
        cJSON_AddItemToArray(caps, o);
    }
    unlock();
    return j;
}

int capture_set_config(const cJSON *args, cJSON **out)
{
    const cJSON *v;
    lock();
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(args, "total_limit"))) {
        if (v->valuedouble < CAP_MIN_SIZE || v->valuedouble > CAP_MAX_TOTAL) {
            unlock();
            return fail(out, 400, "total_limit 16 KB - 12 MB");
        }
        s_total_limit = (size_t)v->valuedouble;
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(args, "default_size"))) {
        if (v->valuedouble < CAP_MIN_SIZE || v->valuedouble > s_total_limit) {
            unlock();
            return fail(out, 400, "default_size 16 KB - total_limit");
        }
        s_default_size = (size_t)v->valuedouble;
    }
    save_armed_locked();
    unlock();
    *out = capture_status_json();
    return 200;
}

/* Starts armed captures whenever their device is present and free. */
static void watcher_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        for (int i = 0; i < CAP_MAX_ARMED; i++) {
            char busid[32], spec[160];
            lock();
            strlcpy(busid, s_armed[i].busid, sizeof(busid));
            strlcpy(spec, s_armed[i].spec, sizeof(spec));
            const cap_dev_t *d = busid[0] ? find_locked(busid) : NULL;
            const bool running = d != NULL && d->running;
            unlock();
            usbip_backend_device_t dev;
            if (busid[0] == '\0' || running || busid_owner_get(busid, NULL) ||
                    !usb_backend_get_device_by_busid(busid, &dev)) {
                continue;
            }
            cJSON *args = cJSON_Parse(spec);
            cJSON *res = NULL;
            const int st = capture_start(busid, args, &res);
            ESP_LOGI(TAG, "armed %s: start -> %d", busid, st);
            cJSON_Delete(res);
            cJSON_Delete(args);
        }
    }
}

void capture_init(void)
{
    nvs_handle_t h;
    if (nvs_open(CAP_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint32_t v;
        if (nvs_get_u32(h, "total", &v) == ESP_OK && v >= CAP_MIN_SIZE && v <= CAP_MAX_TOTAL) {
            s_total_limit = v;
        }
        if (nvs_get_u32(h, "size", &v) == ESP_OK && v >= CAP_MIN_SIZE && v <= s_total_limit) {
            s_default_size = v;
        }
        size_t len = 0;
        if (nvs_get_str(h, "armed", NULL, &len) == ESP_OK && len > 0 && len < 4096) {
            char *txt = malloc(len);
            if (txt != NULL && nvs_get_str(h, "armed", txt, &len) == ESP_OK) {
                cJSON *arr = cJSON_Parse(txt);
                const cJSON *e;
                int n = 0;
                cJSON_ArrayForEach(e, arr) {
                    const cJSON *b = cJSON_GetObjectItemCaseSensitive(e, "busid");
                    if (cJSON_IsString(b) && n < CAP_MAX_ARMED) {
                        strlcpy(s_armed[n].busid, b->valuestring, sizeof(s_armed[n].busid));
                        char *spec = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(e, "spec"));
                        strlcpy(s_armed[n].spec, spec ? spec : "{}", sizeof(s_armed[n].spec));
                        cJSON_free(spec);
                        n++;
                    }
                }
                cJSON_Delete(arr);
            }
            free(txt);
        }
        nvs_close(h);
    }
    xTaskCreate(watcher_task, "cap_watch", 4096, NULL, 2, NULL);
    capture_stream_start();
}

/* ===========================================================================
 *  The serial monitor API (serial_monitor.h) on top of capture
 * ======================================================================== */

int serial_monitor_start(const char *busid, const cJSON *args, cJSON **out)
{
    return capture_start(busid, args, out);
}

int serial_monitor_stop_api(const char *busid, cJSON **out)
{
    return capture_stop(busid, NULL, out);
}

int serial_monitor_poll(const char *busid, uint64_t since, int max, cJSON **out)
{
    return capture_poll(busid, 0, since, max, false, out);
}

int serial_monitor_send(const char *busid, const cJSON *args, cJSON **out)
{
    int port = 0;
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(args, "port");
    if (cJSON_IsNumber(p)) {
        port = (int)p->valuedouble;
    }
    return capture_send(busid, port, args, out);
}

void serial_monitor_stop(const char *busid)
{
    capture_halt(busid);
}

bool serial_monitor_active(const char *busid)
{
    return capture_active(busid);
}
