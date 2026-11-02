#include "busid_owner.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

/* Every exported device plus slack for a busid whose session outlives a
   replug of its device. */
#define BUSID_OWNER_SLOTS (CONFIG_USBIP_MAX_DEVICES + 4)

typedef struct {
    char busid[32];
    busid_owner_info_t info;
} owner_slot_t;

static owner_slot_t s_slots[BUSID_OWNER_SLOTS];
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

static owner_slot_t *find_locked(const char *busid)
{
    for (int i = 0; i < BUSID_OWNER_SLOTS; i++) {
        if (s_slots[i].info.kind != BUSID_OWNER_NONE && strncmp(s_slots[i].busid, busid, 31) == 0) {
            return &s_slots[i];
        }
    }
    return NULL;
}

bool busid_owner_try_begin(const char *busid, busid_owner_kind_t kind, const char *what,
                           busid_owner_info_t *current)
{
    lock();
    owner_slot_t *s = find_locked(busid);
    if (s != NULL) {
        if (s->info.kind != kind || kind == BUSID_OWNER_RESCUE) {
            if (current) {
                *current = s->info;
            }
            unlock();
            return false;
        }
        s->info.sessions++;
        strlcpy(s->info.peer, what ? what : "", sizeof(s->info.peer));
        unlock();
        return true;
    }
    for (int i = 0; i < BUSID_OWNER_SLOTS; i++) {
        if (s_slots[i].info.kind == BUSID_OWNER_NONE) {
            s = &s_slots[i];
            break;
        }
    }
    if (s == NULL) {
        /* Table full: do not block USB/IP over bookkeeping; refuse rescue. */
        unlock();
        if (current) {
            memset(current, 0, sizeof(*current));
        }
        return kind == BUSID_OWNER_USBIP;
    }
    memset(s, 0, sizeof(*s));
    strlcpy(s->busid, busid, sizeof(s->busid));
    s->info.kind = kind;
    s->info.sessions = (kind == BUSID_OWNER_USBIP) ? 1 : 0;
    strlcpy(s->info.peer, what ? what : "", sizeof(s->info.peer));
    s->info.since_us = esp_timer_get_time();
    unlock();
    return true;
}

void busid_owner_end(const char *busid, busid_owner_kind_t kind)
{
    lock();
    owner_slot_t *s = find_locked(busid);
    if (s != NULL && s->info.kind == kind) {
        if (kind == BUSID_OWNER_USBIP && --s->info.sessions > 0) {
            unlock();
            return;
        }
        memset(s, 0, sizeof(*s));
    }
    unlock();
}

bool busid_owner_get(const char *busid, busid_owner_info_t *out)
{
    lock();
    owner_slot_t *s = find_locked(busid);
    if (s != NULL && out != NULL) {
        *out = s->info;
    }
    unlock();
    return s != NULL;
}
