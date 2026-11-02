#include "samba_flasher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "blob_store.h"
#include "device_rescue.h"
#include "rescue_logic.h"
#include "serial_link.h"

#define SAMBA_CHUNK         4096        /* Samba::writeBufferSize() */
#define SAMBA_QUICK_MS      200
#define SAMBA_NORMAL_MS     1000
#define SAMBA_LONG_MS       30000       /* chip erase, CRC of a large range */
#define SAMBA_BOOT_WAIT_MS  6000

typedef struct {
    serial_link_t link;
    rescue_job_t *job;
    char version[128];
    bool can_x, can_y, can_z;
    uint32_t cpuid, device_id, nvm_param, flash_size, page_size;
    rescue_samba_chip_t chip;
} samba_t;

static int send_cmd(samba_t *s, const char *cmd)
{
    /* The SAM-BA firmware misparses a command that shares a USB packet with
       what follows it (BOSSA flushes after every command): send it alone. */
    int rc = serial_link_write(&s->link, (const uint8_t *)cmd, strlen(cmd));
    if (rc == 0) {
        rc = serial_link_flush(&s->link, SAMBA_NORMAL_MS);
    }
    return rc;
}

/* Read exactly n bytes (0 on success). */
static int read_n(samba_t *s, uint8_t *buf, size_t n, uint32_t timeout_ms)
{
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    size_t got = 0;
    while (got < n) {
        const int64_t left = deadline - esp_timer_get_time();
        if (left <= 0) {
            return -1;
        }
        const int r = serial_link_read(&s->link, buf + got, n - got, (uint32_t)(left / 1000) + 1);
        if (r < 0) {
            return r;
        }
        got += (size_t)r;
    }
    return 0;
}

/* Whatever arrives within timeout_ms after the last byte. */
static size_t read_until_quiet(samba_t *s, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    size_t got = 0;
    while (got < cap) {
        const int r = serial_link_read(&s->link, buf + got, cap - got, timeout_ms);
        if (r <= 0) {
            break;
        }
        got += (size_t)r;
    }
    return got;
}

static int read_word(samba_t *s, uint32_t addr, uint32_t *value)
{
    char cmd[20];
    snprintf(cmd, sizeof(cmd), "w%08lX,4#", (unsigned long)addr);
    uint8_t b[4];
    int rc = send_cmd(s, cmd);
    if (rc == 0) {
        rc = read_n(s, b, 4, SAMBA_NORMAL_MS);
    }
    if (rc == 0) {
        *value = (uint32_t)b[3] << 24 | (uint32_t)b[2] << 16 | (uint32_t)b[1] << 8 | b[0];
    }
    return rc;
}

/* Samba::init() over USB: binary mode, then the version string. */
static int samba_init(samba_t *s)
{
    uint8_t dummy[256];
    serial_link_discard_input(&s->link);
    read_until_quiet(s, dummy, sizeof(dummy), 50);
    if (send_cmd(s, "N#") != 0) {
        return -1;
    }
    read_n(s, dummy, 2, SAMBA_QUICK_MS);                     /* "\n\r" */
    if (send_cmd(s, "V#") != 0) {
        return -1;
    }
    const size_t n = read_until_quiet(s, (uint8_t *)s->version, sizeof(s->version) - 1, SAMBA_QUICK_MS);
    s->version[n] = '\0';
    while (n > 0 && (s->version[strlen(s->version) - 1] == '\n' || s->version[strlen(s->version) - 1] == '\r')) {
        s->version[strlen(s->version) - 1] = '\0';
        if (s->version[0] == '\0') {
            break;
        }
    }
    if (strstr(s->version, "v") == NULL && strstr(s->version, "V") == NULL) {
        return -1;          /* not a SAM-BA monitor */
    }
    const char *ext = strstr(s->version, "[Arduino:");
    if (ext != NULL) {
        for (const char *p = ext + 9; *p && *p != ']'; p++) {
            s->can_x |= *p == 'X';
            s->can_y |= *p == 'Y';
            s->can_z |= *p == 'Z';
        }
    }
    return 0;
}

static int samba_connect(samba_t *s, rescue_job_t *job, const char *busid, const cJSON *args)
{
    s->job = job;
    char msg[128];
    if (serial_link_open(&s->link, busid, 0, msg, sizeof(msg)) != 0) {
        return rescue_job_fail(job, "%s", msg);
    }
    if (s->link.caps.serial != RESCUE_SERIAL_CDC_ACM) {
        return rescue_job_fail(job, "SAM-BA needs the board's native USB CDC (this is a %s)",
                               rescue_serial_kind_name(s->link.caps.serial));
    }
    serial_link_set_baud(&s->link, 921600);
    serial_link_set_lines(&s->link, true, true);
    rescue_job_progress(job, 2, "connecting to SAM-BA");
    if (samba_init(s) != 0) {
        if (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(args, "enter_bootloader"))) {
            return rescue_job_fail(job, "no SAM-BA monitor answers on this CDC (is the board in its bootloader?)");
        }
        rescue_job_log(job, "no SAM-BA answer: 1200-baud touch to enter the bootloader");
        const uint32_t devnum = s->link.devnum;
        serial_link_set_lines(&s->link, true, true);
        serial_link_set_baud(&s->link, 1200);
        vTaskDelay(pdMS_TO_TICKS(50));
        serial_link_set_lines(&s->link, false, false);
        usbip_backend_device_t now;
        bool dropped;
        if (!device_rescue_wait_reenumeration(busid, devnum, SAMBA_BOOT_WAIT_MS, &now, &dropped)) {
            return rescue_job_fail(job, "the board did not re-enumerate after the 1200-baud touch");
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        if (serial_link_reopen(&s->link, msg, sizeof(msg)) != 0) {
            return rescue_job_fail(job, "bootloader: %s", msg);
        }
        rescue_job_log(job, "bootloader: %04x:%04x %s", now.id_vendor, now.id_product, now.product);
        serial_link_set_baud(&s->link, 921600);
        serial_link_set_lines(&s->link, true, true);
        if (samba_init(s) != 0) {
            return rescue_job_fail(job, "no SAM-BA monitor answers after entering the bootloader");
        }
    }
    rescue_job_log(job, "SAM-BA: %s", s->version);
    if (read_word(s, 0xE000ED00, &s->cpuid) != 0 || read_word(s, 0x41002018, &s->device_id) != 0) {
        return rescue_job_fail(job, "cannot read the chip id");
    }
    read_word(s, 0x41004008, &s->nvm_param);                /* NVMCTRL PARAM */
    s->flash_size = rescue_samba_flash_size(s->nvm_param, &s->page_size);
    rescue_samba_chip(s->cpuid, s->device_id, s->flash_size, &s->chip);
    cJSON *res = rescue_job_result(job);
    cJSON_AddStringToObject(res, "bootloader", s->version);
    char hex[12];
    snprintf(hex, sizeof(hex), "0x%08lx", (unsigned long)s->device_id);
    cJSON_AddStringToObject(res, "device_id", hex);
    cJSON_AddStringToObject(res, "chip", s->chip.family ? s->chip.family : "unknown");
    cJSON_AddNumberToObject(res, "flash_size", s->flash_size);
    cJSON_AddNumberToObject(res, "page_size", s->page_size);
    cJSON_AddBoolToObject(res, "arduino_extensions", s->can_x && s->can_y && s->can_z);
    if (s->chip.family == NULL) {
        return rescue_job_fail(job, "unsupported chip (CPUID 0x%08lx, DID 0x%08lx): SAMD21/SAMD51/SAME5x only",
                               (unsigned long)s->cpuid, (unsigned long)s->device_id);
    }
    rescue_job_log(job, "%s, %lu KB flash, %lu-byte pages", s->chip.family, (unsigned long)(s->flash_size / 1024),
                   (unsigned long)s->page_size);
    return 0;
}

static int expect_ack(samba_t *s, char letter, uint32_t timeout_ms)
{
    uint8_t r[3] = {0};
    return (read_n(s, r, 3, timeout_ms) == 0 && r[0] == (uint8_t)letter) ? 0 : -1;
}

int samba_info_job(rescue_job_t *job, const char *busid, const cJSON *args)
{
    samba_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_DEFAULT);
    if (s == NULL) {
        return rescue_job_fail(job, "out of memory");
    }
    const int rc = samba_connect(s, job, busid, args);
    if (rc == 0 && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "reset"))) {
        /* Back to the app: SYSRESETREQ through AIRCR, as Device::reset() does. */
        rescue_job_log(job, "resetting the board");
        send_cmd(s, "WE000ED0C,05FA0004#");
    }
    serial_link_close(&s->link);
    heap_caps_free(s);
    return rc;
}

int samba_flasher_job(rescue_job_t *job, const char *busid, const cJSON *args)
{
    const cJSON *file = cJSON_GetObjectItemCaseSensitive(args, "file");
    if (!cJSON_IsString(file)) {
        return rescue_job_fail(job, "file (staged .bin) required");
    }
    const uint8_t *data;
    size_t len;
    if (!blob_acquire(file->valuestring, &data, &len)) {
        return rescue_job_fail(job, "no staged file named %s", file->valuestring);
    }
    rescue_uf2_block_t blk;
    if (rescue_uf2_block(data, len, 0, &blk)) {
        blob_release(file->valuestring);
        return rescue_job_fail(job, "%s is a UF2: copy it to the bootloader drive (flash/uf2) or give the .bin",
                               file->valuestring);
    }
    samba_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_DEFAULT);
    uint8_t *buf = malloc(SAMBA_CHUNK);
    int rc = (s && buf) ? samba_connect(s, job, busid, args) : rescue_job_fail(job, "out of memory");
    if (rc == 0 && !(s->can_x && s->can_y && s->can_z)) {
        rc = rescue_job_fail(job, "this SAM-BA has no Arduino X/Y/Z extensions (%s); not supported here", s->version);
    }
    uint32_t offset = rc == 0 ? s->chip.app_offset : 0;
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(args, "offset");
    if (cJSON_IsString(o)) {
        offset = (uint32_t)strtoul(o->valuestring, NULL, 0);
    } else if (cJSON_IsNumber(o)) {
        offset = (uint32_t)o->valuedouble;
    }
    if (rc == 0 && (s->page_size == 0 || offset % s->page_size || offset + len > s->flash_size)) {
        rc = rescue_job_fail(job, "offset 0x%lx: must be page aligned and the image must fit in the flash",
                             (unsigned long)offset);
    }
    if (rc == 0 && offset < s->chip.app_offset && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "force_offset"))) {
        rc = rescue_job_fail(job, "offset 0x%lx is inside the bootloader (ends at 0x%lx); force_offset to override",
                             (unsigned long)offset, (unsigned long)s->chip.app_offset);
    }
    char cmd[40];
    if (rc == 0 && !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(args, "erase"))) {
        rescue_job_progress(job, 5, "erasing");
        rescue_job_log(job, "erasing from 0x%lx", (unsigned long)offset);
        snprintf(cmd, sizeof(cmd), "X%08lX#", (unsigned long)offset);
        if (send_cmd(s, cmd) != 0 || expect_ack(s, 'X', SAMBA_LONG_MS) != 0) {
            rc = rescue_job_fail(job, "chip erase failed");
        }
    }
    const uint32_t ram = rc == 0 ? ((s->chip.user_ram + 0x34 + 3) & ~3u) : 0;   /* after BOSSA's WordCopy applet */
    const bool verify = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(args, "verify"));
    const int64_t t0 = esp_timer_get_time();
    for (size_t pos = 0; rc == 0 && pos < len; pos += SAMBA_CHUNK) {
        if (rescue_job_cancelled(job)) {
            rc = rescue_job_fail(job, "cancelled");
            break;
        }
        size_t n = len - pos < SAMBA_CHUNK ? len - pos : SAMBA_CHUNK;
        memcpy(buf, data + pos, n);
        if (n < SAMBA_CHUNK) {                              /* pad the tail to a page, as BOSSA does */
            const size_t padded = (n + s->page_size - 1) / s->page_size * s->page_size;
            memset(buf + n, 0, padded - n);
            n = padded;
        }
        const uint32_t dst = offset + (uint32_t)pos;
        snprintf(cmd, sizeof(cmd), "S%08lX,%08lX#", (unsigned long)ram, (unsigned long)n);
        int w = send_cmd(s, cmd);
        if (w == 0) {
            w = serial_link_write(&s->link, buf, n);
        }
        if (w == 0) {
            w = serial_link_flush(&s->link, SAMBA_NORMAL_MS);
        }
        if (w == 0) {
            snprintf(cmd, sizeof(cmd), "Y%08lX,0#", (unsigned long)ram);
            w = (send_cmd(s, cmd) == 0 && expect_ack(s, 'Y', SAMBA_NORMAL_MS) == 0) ? 0 : -1;
        }
        if (w == 0) {
            snprintf(cmd, sizeof(cmd), "Y%08lX,%08lX#", (unsigned long)dst, (unsigned long)n);
            w = (send_cmd(s, cmd) == 0 && expect_ack(s, 'Y', SAMBA_LONG_MS) == 0) ? 0 : -1;
        }
        if (w != 0) {
            rc = rescue_job_fail(job, "writing 0x%08lx failed", (unsigned long)dst);
            break;
        }
        if (verify) {
            snprintf(cmd, sizeof(cmd), "Z%08lX,%08lX#", (unsigned long)dst, (unsigned long)n);
            uint8_t z[12] = {0};
            if (send_cmd(s, cmd) != 0 || read_n(s, z, 12, SAMBA_LONG_MS) != 0 || z[0] != 'Z') {
                rc = rescue_job_fail(job, "CRC of 0x%08lx failed", (unsigned long)dst);
                break;
            }
            z[9] = '\0';
            const uint32_t dev_crc = (uint32_t)strtoul((char *)&z[1], NULL, 16);
            const uint16_t crc = rescue_crc16_xmodem(buf, n, 0);
            if (dev_crc != crc) {
                rc = rescue_job_fail(job, "verify failed at 0x%08lx (CRC %04lx, expected %04x)",
                                     (unsigned long)dst, (unsigned long)dev_crc, crc);
                break;
            }
        }
        rescue_job_progress(job, 8 + (int)(88 * (pos + n) / len), "writing");
    }
    if (rc == 0) {
        const double secs = (double)(esp_timer_get_time() - t0) / 1e6;
        rescue_job_log(job, "%u bytes written at 0x%lx%s in %.1f s", (unsigned)len, (unsigned long)offset,
                       verify ? " and verified (CRC16)" : "", secs);
        cJSON *res = rescue_job_result(job);
        cJSON_AddNumberToObject(res, "offset", offset);
        cJSON_AddNumberToObject(res, "bytes", (double)len);
        cJSON_AddBoolToObject(res, "verified", verify);
        cJSON_AddNumberToObject(res, "seconds", secs);
        if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(args, "reset"))) {
            /* Device::reset(): SYSRESETREQ through AIRCR. */
            rescue_job_log(job, "resetting the board");
            send_cmd(s, "WE000ED0C,05FA0004#");
        }
    }
    if (s != NULL) {
        serial_link_close(&s->link);
        heap_caps_free(s);
    }
    free(buf);
    blob_release(file->valuestring);
    return rc;
}
