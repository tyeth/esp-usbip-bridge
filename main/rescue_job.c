#include "rescue_job.h"

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
#include "capture.h"
#include "device_rescue.h"
#include "serial_monitor.h"

static const char *TAG = "rescue_job";

#define JOB_SLOTS       4
#define JOB_LOG_MAX     8192
#define JOB_TASK_STACK  12288

typedef enum { JOB_QUEUED, JOB_RUNNING, JOB_DONE, JOB_FAILED } job_state_t;

struct rescue_job {
    uint32_t id;
    char kind[24];
    char busid[32];
    cJSON *args;
    rescue_job_fn_t fn;
    volatile job_state_t state;
    volatile int progress;
    volatile bool cancel;
    volatile bool timed_out;    /* cancelled by the RESCUE_JOB_MAX_S limit */
    char stage[64];
    char *log;                  /* PSRAM, JOB_LOG_MAX; oldest lines dropped */
    size_t log_len;
    cJSON *result;
    char error[192];
    int64_t started_us, finished_us;
};

static rescue_job_t s_jobs[JOB_SLOTS];
static uint32_t s_next_id = 1;
static rescue_job_t *s_running;
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

static const char *state_name(job_state_t s)
{
    switch (s) {
    case JOB_QUEUED: return "queued";
    case JOB_RUNNING: return "running";
    case JOB_DONE: return "done";
    default: return "failed";
    }
}

static void append_log_locked(rescue_job_t *job, const char *line)
{
    if (job->log == NULL) {
        return;
    }
    size_t n = strlen(line);
    if (n > JOB_LOG_MAX / 2) {
        n = JOB_LOG_MAX / 2;
    }
    if (job->log_len + n + 1 >= JOB_LOG_MAX) {
        /* Drop the oldest lines. */
        size_t drop = job->log_len + n + 1 - JOB_LOG_MAX + JOB_LOG_MAX / 4;
        const char *nl = memchr(job->log + drop, '\n', job->log_len - drop);
        drop = nl ? (size_t)(nl - job->log) + 1 : job->log_len;
        memmove(job->log, job->log + drop, job->log_len - drop);
        job->log_len -= drop;
    }
    memcpy(job->log + job->log_len, line, n);
    job->log_len += n;
    job->log[job->log_len++] = '\n';
    job->log[job->log_len] = '\0';
}

void rescue_job_log(rescue_job_t *job, const char *fmt, ...)
{
    char line[256];
    const int64_t t = esp_timer_get_time() - job->started_us;
    int o = snprintf(line, sizeof(line), "%6.2f ", (double)t / 1e6);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + o, sizeof(line) - o, fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "[%" PRIu32 " %s] %s", job->id, job->busid, line + o);
    lock();
    append_log_locked(job, line);
    unlock();
}

void rescue_job_progress(rescue_job_t *job, int percent, const char *stage)
{
    lock();
    if (percent >= 0) {             /* < 0 keeps the current value */
        job->progress = percent > 100 ? 100 : percent;
    }
    if (stage != NULL) {
        strlcpy(job->stage, stage, sizeof(job->stage));
    }
    unlock();
}

int rescue_job_fail(rescue_job_t *job, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    lock();
    vsnprintf(job->error, sizeof(job->error), fmt, ap);
    unlock();
    va_end(ap);
    rescue_job_log(job, "ERROR: %s", job->error);
    return -1;
}

cJSON *rescue_job_result(rescue_job_t *job)
{
    lock();
    if (job->result == NULL) {
        job->result = cJSON_CreateObject();
    }
    cJSON *r = job->result;
    unlock();
    return r;
}

void rescue_job_result_set(rescue_job_t *job, const char *key, cJSON *item)
{
    lock();
    if (job->result == NULL) {
        job->result = cJSON_CreateObject();
    }
    if (cJSON_HasObjectItem(job->result, key)) {
        cJSON_ReplaceItemInObjectCaseSensitive(job->result, key, item);
    } else {
        cJSON_AddItemToObject(job->result, key, item);
    }
    unlock();
}

void rescue_job_result_append(rescue_job_t *job, const char *key, cJSON *item, int keep)
{
    lock();
    if (job->result == NULL) {
        job->result = cJSON_CreateObject();
    }
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(job->result, key);
    if (!cJSON_IsArray(arr)) {
        cJSON_DeleteItemFromObjectCaseSensitive(job->result, key);
        arr = cJSON_AddArrayToObject(job->result, key);
    }
    cJSON_AddItemToArray(arr, item);
    while (keep > 0 && cJSON_GetArraySize(arr) > keep) {
        cJSON_DeleteItemFromArray(arr, 0);
    }
    unlock();
}

uint32_t rescue_job_id(const rescue_job_t *job)
{
    return job->id;
}

bool rescue_job_cancelled(const rescue_job_t *job)
{
    if (!job->cancel && esp_timer_get_time() - job->started_us > (int64_t)RESCUE_JOB_MAX_S * 1000000) {
        /* Nothing may hold a device forever: past the limit the job is
           cancelled as by DELETE, at its next check. */
        rescue_job_t *j = (rescue_job_t *)job;
        j->timed_out = true;
        j->cancel = true;
    }
    return job->cancel;
}

static void job_task(void *arg)
{
    rescue_job_t *job = (rescue_job_t *)arg;
    lock();
    job->state = JOB_RUNNING;
    job->started_us = esp_timer_get_time();
    unlock();
    rescue_job_log(job, "%s on %s", job->kind, job->busid);
    const int rc = job->fn(job, job->busid, job->args);
    if (job->timed_out) {
        rescue_job_fail(job, "timed out: jobs are stopped after %d s", RESCUE_JOB_MAX_S);
    } else if (rc == 0 && job->cancel) {
        rescue_job_fail(job, "cancelled");
    }
    busid_owner_end(job->busid, BUSID_OWNER_RESCUE);
    lock();
    job->state = (rc == 0 && !job->cancel) ? JOB_DONE : JOB_FAILED;
    if (job->state == JOB_DONE) {
        job->progress = 100;
    } else if (job->error[0] == '\0') {
        strlcpy(job->error, "failed", sizeof(job->error));
    }
    job->finished_us = esp_timer_get_time();
    s_running = NULL;
    unlock();
    /* "capture_after": start capturing the device's serial output once the
       job has let go of it (a freshly flashed board's first boot). */
    const cJSON *after = cJSON_GetObjectItemCaseSensitive(job->args, "capture_after");
    if (job->state == JOB_DONE && (cJSON_IsObject(after) || cJSON_IsTrue(after))) {
        cJSON *res = NULL;
        const int st = capture_start(job->busid, cJSON_IsObject(after) ? after : NULL, &res);
        rescue_job_log(job, "capture_after: %s", st == 200 ? "capturing" : "could not start capture");
        if (res != NULL) {
            cJSON_AddItemToObject(rescue_job_result(job), "capture", res);
        }
        const cJSON *mode = cJSON_GetObjectItemCaseSensitive(job->result, "deferred_reset");
        if (st == 200 && cJSON_IsString(mode)) {
            /* Reset only once the capture is reading at its chosen rate: a
               reset during the auto-baud probe let a wrong rate win. */
            if (!capture_wait_ready(job->busid, 10000)) {
                rescue_job_log(job, "capture not ready after 10 s, resetting anyway");
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            cJSON *rargs = cJSON_CreateObject();
            cJSON_AddStringToObject(rargs, "mode", mode->valuestring);
            cJSON *rres = NULL;
            const int rs = device_rescue_reset(job->busid, rargs, &rres);
            rescue_job_log(job, "deferred %s: %s", mode->valuestring, rs == 200 ? "done" : "failed");
            cJSON_Delete(rres);
            cJSON_Delete(rargs);
        }
    }
    rescue_job_log(job, "%s", job->state == JOB_DONE ? "done" : "failed");
    vTaskDelete(NULL);
}

uint32_t rescue_job_start(const char *kind, const char *busid, const cJSON *args,
                          rescue_job_fn_t fn, int *status, char *msg, size_t msg_len)
{
    if (busid == NULL || busid[0] == '\0' || strlen(busid) > 31) {
        *status = 400;
        snprintf(msg, msg_len, "busid required");
        return 0;
    }
    lock();
    const bool busy = s_running != NULL;
    unlock();
    if (busy) {
        *status = 409;
        snprintf(msg, msg_len, "another rescue job is running (one at a time)");
        return 0;
    }
    /* A monitor on the device would fight the job for its endpoints. */
    serial_monitor_stop(busid);
    char what[48];
    snprintf(what, sizeof(what), "rescue:job:%s", kind);
    busid_owner_info_t owner;
    if (!busid_owner_try_begin(busid, BUSID_OWNER_RESCUE, what, &owner)) {
        *status = 409;
        snprintf(msg, msg_len, owner.kind == BUSID_OWNER_USBIP
                 ? "device is imported over USB/IP by %s; detach it first" : "device is busy (%s)",
                 owner.peer[0] ? owner.peer : "?");
        return 0;
    }

    lock();
    /* Reuse the oldest finished slot. */
    rescue_job_t *job = NULL;
    for (int i = 0; i < JOB_SLOTS; i++) {
        rescue_job_t *j = &s_jobs[i];
        if (j->id == 0) {
            job = j;
            break;
        }
        if (j->state != JOB_RUNNING && j->state != JOB_QUEUED && (job == NULL || j->id < job->id)) {
            job = j;
        }
    }
    if (job == NULL) {
        unlock();
        busid_owner_end(busid, BUSID_OWNER_RESCUE);
        *status = 503;
        snprintf(msg, msg_len, "no job slot");
        return 0;
    }
    cJSON_Delete(job->args);
    cJSON_Delete(job->result);
    char *log = job->log;
    memset(job, 0, sizeof(*job));
    job->log = log ? log : heap_caps_malloc(JOB_LOG_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (job->log) {
        job->log[0] = '\0';
    }
    job->id = s_next_id++;
    strlcpy(job->kind, kind, sizeof(job->kind));
    strlcpy(job->busid, busid, sizeof(job->busid));
    job->args = args ? cJSON_Duplicate(args, true) : cJSON_CreateObject();
    job->fn = fn;
    job->state = JOB_QUEUED;
    job->started_us = esp_timer_get_time();
    s_running = job;
    const uint32_t id = job->id;
    unlock();

    if (xTaskCreate(job_task, "rescue_job", JOB_TASK_STACK, job, 4, NULL) != pdPASS) {
        lock();
        job->state = JOB_FAILED;
        strlcpy(job->error, "cannot start the job task", sizeof(job->error));
        s_running = NULL;
        unlock();
        busid_owner_end(busid, BUSID_OWNER_RESCUE);
        *status = 503;
        snprintf(msg, msg_len, "cannot start the job task");
        return 0;
    }
    *status = 202;
    return id;
}

static cJSON *job_json_locked(const rescue_job_t *j, bool with_log)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", j->id);
    cJSON_AddStringToObject(o, "kind", j->kind);
    cJSON_AddStringToObject(o, "busid", j->busid);
    cJSON_AddStringToObject(o, "state", state_name(j->state));
    cJSON_AddNumberToObject(o, "progress", j->progress);
    cJSON_AddStringToObject(o, "stage", j->stage);
    const int64_t end = j->finished_us ? j->finished_us : esp_timer_get_time();
    cJSON_AddNumberToObject(o, "elapsed_s", (double)((end - j->started_us) / 100000) / 10.0);
    cJSON_AddNumberToObject(o, "limit_s", RESCUE_JOB_MAX_S);
    if (j->error[0]) {
        cJSON_AddStringToObject(o, "error", j->error);
    } else {
        cJSON_AddNullToObject(o, "error");
    }
    if (j->result) {
        cJSON_AddItemToObject(o, "result", cJSON_Duplicate(j->result, true));
    } else {
        cJSON_AddNullToObject(o, "result");
    }
    if (with_log) {
        cJSON_AddStringToObject(o, "log", j->log ? j->log : "");
    }
    return o;
}

cJSON *rescue_job_json(uint32_t id)
{
    lock();
    const rescue_job_t *found = NULL;
    for (int i = 0; i < JOB_SLOTS; i++) {
        const rescue_job_t *j = &s_jobs[i];
        if (j->id == 0) {
            continue;
        }
        if ((id == 0 && (found == NULL || j->id > found->id)) || j->id == id) {
            found = j;
        }
    }
    cJSON *o = found ? job_json_locked(found, true) : NULL;
    unlock();
    return o;
}

cJSON *rescue_job_list_json(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(j, "jobs");
    lock();
    for (int i = 0; i < JOB_SLOTS; i++) {
        if (s_jobs[i].id != 0) {
            cJSON_AddItemToArray(arr, job_json_locked(&s_jobs[i], false));
        }
    }
    if (s_running) {
        cJSON_AddNumberToObject(j, "running", s_running->id);
    } else {
        cJSON_AddNullToObject(j, "running");
    }
    unlock();
    return j;
}

bool rescue_job_cancel(uint32_t id)
{
    lock();
    const bool ok = s_running != NULL && s_running->id == id;
    if (ok) {
        s_running->cancel = true;
    }
    unlock();
    return ok;
}

uint32_t rescue_job_running_on(const char *busid)
{
    lock();
    const uint32_t id = (s_running != NULL && (busid == NULL || strcmp(s_running->busid, busid) == 0))
                        ? s_running->id : 0;
    unlock();
    return id;
}

bool rescue_job_busy(const char *busid)
{
    lock();
    const bool busy = s_running != NULL && strcmp(s_running->busid, busid) == 0;
    unlock();
    return busy;
}
