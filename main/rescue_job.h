#ifndef RESCUE_JOB_H
#define RESCUE_JOB_H

/*
 * Background jobs for device rescue work that takes longer than an HTTP
 * request should block the (single-threaded) bridge API: flashing an ESP,
 * PICOBOOT and SAM-BA loads, mass-storage copies.  One job runs at a time,
 * on its own task; it holds the device for rescue (USB/IP imports are
 * refused meanwhile) and stops a serial monitor on it first.  Progress, a
 * log and the result are polled with GET /api/rescue/jobs/{id}.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

typedef struct rescue_job rescue_job_t;

/* Runs on the job task.  Returns 0, or sets an error with rescue_job_fail()
   and returns non-zero.  `args` is the request body (owned by the job). */
typedef int (*rescue_job_fn_t)(rescue_job_t *job, const char *busid, const cJSON *args);

/* Queue a job.  Returns its id (> 0), or 0 with an HTTP status and message
   (409 when another job runs or the device is imported over USB/IP). */
uint32_t rescue_job_start(const char *kind, const char *busid, const cJSON *args,
                          rescue_job_fn_t fn, int *status, char *msg, size_t msg_len);

void rescue_job_log(rescue_job_t *job, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void rescue_job_progress(rescue_job_t *job, int percent, const char *stage);
int rescue_job_fail(rescue_job_t *job, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* The job's result object (created on first use; returned with the job). */
cJSON *rescue_job_result(rescue_job_t *job);
/* For a job that keeps running while it is polled: set or replace a result
   key, or append to a result array (keeping the last `keep`, 0 = all), under
   the job lock.  Takes `item`. */
void rescue_job_result_set(rescue_job_t *job, const char *key, cJSON *item);
void rescue_job_result_append(rescue_job_t *job, const char *key, cJSON *item, int keep);
uint32_t rescue_job_id(const rescue_job_t *job);
/* Set by DELETE /api/rescue/jobs/{id}: long loops should check and stop. */
bool rescue_job_cancelled(const rescue_job_t *job);

/* {id, kind, busid, state, progress, stage, log, result, error, ...};
   id 0 = the latest job.  NULL when unknown. */
cJSON *rescue_job_json(uint32_t id);
cJSON *rescue_job_list_json(void);
bool rescue_job_cancel(uint32_t id);
/* Whether a job is running on busid (rescue actions are then refused). */
bool rescue_job_busy(const char *busid);

#endif
