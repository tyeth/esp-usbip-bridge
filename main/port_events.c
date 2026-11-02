#include "port_events.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "hub_control.h"
#include "usb_backend.h"

#define PE_TAG "port_events"
#define PE_POLL_MS 500
#define PE_ACTION_WINDOW_MS 15000   /* a cycle's off period is up to HUB_CTL_MAX_OFF_MS */
#define PE_MAX_PORTS 32
#define PE_MAX_ACTIONS 32

typedef struct {
    bool used;
    bool seen;                  /* baseline taken */
    char path[32];
    bool powered;
    bool connected;
    bool enabled;
    bool over_current;
    bool has_last;              /* last_* are valid (under s_lock) */
    char last_what[16];
    char last_attr[12];
    uint32_t last_t;
} pe_port_t;

typedef struct {
    bool used;
    char path[32];
    uint32_t t_ms;
    port_event_src_t src;
} pe_action_t;

static SemaphoreHandle_t s_lock;
static port_event_t s_log[PORT_EVENT_LOG_LEN];
static size_t s_log_head;       /* next write slot */
static size_t s_log_count;
static uint32_t s_ext_power;
static uint32_t s_replugs;
static pe_action_t s_actions[PE_MAX_ACTIONS];
static size_t s_action_next;
static pe_port_t s_ports[PE_MAX_PORTS];     /* baseline fields: poll task only; last_*: under s_lock */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void copy_str(char *dst, size_t n, const char *src)
{
    strlcpy(dst, (src != NULL) ? src : "", n);
}

void port_events_note_action(const char *port_path, port_event_src_t src)
{
    if (s_lock == NULL || port_path == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pe_action_t *a = &s_actions[s_action_next];
    s_action_next = (s_action_next + 1) % PE_MAX_ACTIONS;
    a->used = true;
    copy_str(a->path, sizeof(a->path), port_path);
    a->t_ms = now_ms();
    a->src = src;
    xSemaphoreGive(s_lock);
}

/* Newest action on this port within the window. Caller holds s_lock. */
static const pe_action_t *recent_action_locked(const char *path, uint32_t now)
{
    const pe_action_t *best = NULL;
    for (size_t i = 0; i < PE_MAX_ACTIONS; i++) {
        const pe_action_t *a = &s_actions[i];
        if (!a->used || strncmp(a->path, path, sizeof(a->path)) != 0) {
            continue;
        }
        if ((uint32_t)(now - a->t_ms) > PE_ACTION_WINDOW_MS) {
            continue;
        }
        if (best == NULL || (int32_t)(a->t_ms - best->t_ms) > 0) {
            best = a;
        }
    }
    return best;
}

/* Caller holds s_lock. */
static void emit_locked(pe_port_t *pv, uint32_t t, const char *path, const char *what,
                        const char *from, const char *to, const char *attr, const char *via,
                        bool mismatch)
{
    port_event_t *e = &s_log[s_log_head];
    e->t_ms = t;
    copy_str(e->port, sizeof(e->port), path);
    copy_str(e->what, sizeof(e->what), what);
    copy_str(e->from, sizeof(e->from), from);
    copy_str(e->to, sizeof(e->to), to);
    copy_str(e->attribution, sizeof(e->attribution), attr);
    copy_str(e->via, sizeof(e->via), via);
    e->mismatch = mismatch;
    s_log_head = (s_log_head + 1) % PORT_EVENT_LOG_LEN;
    if (s_log_count < PORT_EVENT_LOG_LEN) {
        s_log_count++;
    }
    copy_str(pv->last_what, sizeof(pv->last_what), what);
    copy_str(pv->last_attr, sizeof(pv->last_attr), attr);
    pv->last_t = t;
    pv->has_last = true;
    ESP_LOGI(PE_TAG, "%s %s %s->%s (%s%s%s)%s", path, what, from, to, attr,
             via[0] != '\0' ? " via " : "", via, mismatch ? " MISMATCH" : "");
}

static pe_port_t *find_or_add_port(const char *path)
{
    pe_port_t *free_slot = NULL;
    for (size_t i = 0; i < PE_MAX_PORTS; i++) {
        if (s_ports[i].used && strncmp(s_ports[i].path, path, sizeof(s_ports[i].path)) == 0) {
            return &s_ports[i];
        }
        if (!s_ports[i].used && free_slot == NULL) {
            free_slot = &s_ports[i];
        }
    }
    if (free_slot != NULL) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->used = true;
        copy_str(free_slot->path, sizeof(free_slot->path), path);
    }
    return free_slot;
}

static void scan_port(const hub_ctl_port_t *ps)
{
    pe_port_t *pv = find_or_add_port(ps->path);
    if (pv == NULL) {
        return;     /* table full: this port is not tracked */
    }
    if (!pv->seen) {
        pv->seen = true;
        pv->powered = ps->powered;
        pv->connected = ps->connected;
        pv->enabled = ps->enabled;
        pv->over_current = ps->over_current;
        return;
    }
    const uint32_t t = now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (ps->powered != pv->powered) {
        const pe_action_t *act = recent_action_locked(ps->path, t);
        const bool restore = (act != NULL && act->src == PORT_EVENT_SRC_RESTORE);
        const char *attr = (act != NULL) ? "api" : "external";
        if (act == NULL) {
            s_ext_power++;
        }
        emit_locked(pv, t, ps->path, ps->powered ? "power_on" : "power_off",
                    ps->powered ? "off" : "on", ps->powered ? "on" : "off",
                    attr, restore ? "restore" : "", ps->mismatch);
    }
    if (ps->connected != pv->connected) {
        if (ps->connected) {
            s_replugs++;
        }
        emit_locked(pv, t, ps->path, ps->connected ? "connected" : "disconnected",
                    ps->connected ? "no" : "yes", ps->connected ? "yes" : "no", "device", "", false);
    }
    if (ps->enabled != pv->enabled) {
        emit_locked(pv, t, ps->path, ps->enabled ? "enabled" : "disabled",
                    ps->enabled ? "no" : "yes", ps->enabled ? "yes" : "no", "device", "", false);
    }
    if (ps->over_current != pv->over_current) {
        emit_locked(pv, t, ps->path, ps->over_current ? "overcurrent" : "overcurrent_cleared",
                    ps->over_current ? "no" : "yes", ps->over_current ? "yes" : "no", "external", "", false);
    }
    xSemaphoreGive(s_lock);
    pv->powered = ps->powered;
    pv->connected = ps->connected;
    pv->enabled = ps->enabled;
    pv->over_current = ps->over_current;
}

static void poll_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));
    hub_ctl_hub_status_t *snap = malloc(USB_BACKEND_MAX_HUBS * sizeof(*snap));
    if (snap == NULL) {
        ESP_LOGE(PE_TAG, "no memory for the port snapshot: port event log disabled");
        vTaskDelete(NULL);
        return;
    }
    while (true) {
        const size_t n = hub_ctl_snapshot(snap, USB_BACKEND_MAX_HUBS);
        for (size_t h = 0; h < n; h++) {
            for (uint8_t p = 0; p < snap[h].num_ports && p < HUB_CTL_MAX_PORTS; p++) {
                scan_port(&snap[h].ports[p]);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(PE_POLL_MS));
    }
}

void port_events_init(void)
{
    if (s_lock != NULL) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return;
    }
    if (xTaskCreate(poll_task, "port_events", 6144, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(PE_TAG, "poll task create failed");
    }
}

size_t port_events_get(port_event_t *out, size_t max)
{
    if (out == NULL || s_lock == NULL) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t n = (s_log_count < max) ? s_log_count : max;
    for (size_t i = 0; i < n; i++) {
        const size_t idx = (s_log_head + PORT_EVENT_LOG_LEN - 1 - i) % PORT_EVENT_LOG_LEN;
        out[i] = s_log[idx];
    }
    xSemaphoreGive(s_lock);
    return n;
}

void port_events_counters(uint32_t *external_power_changes, uint32_t *device_replugs)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (external_power_changes != NULL) {
        *external_power_changes = s_ext_power;
    }
    if (device_replugs != NULL) {
        *device_replugs = s_replugs;
    }
    xSemaphoreGive(s_lock);
}

bool port_events_last_change(const char *port_path, port_last_change_t *out)
{
    if (out == NULL || port_path == NULL || s_lock == NULL) {
        return false;
    }
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < PE_MAX_PORTS && !found; i++) {
        const pe_port_t *pv = &s_ports[i];
        if (pv->used && pv->has_last && strncmp(pv->path, port_path, sizeof(pv->path)) == 0) {
            out->t_ms = pv->last_t;
            copy_str(out->what, sizeof(out->what), pv->last_what);
            copy_str(out->attribution, sizeof(out->attribution), pv->last_attr);
            found = true;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}
