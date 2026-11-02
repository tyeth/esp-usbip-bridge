#include "esp_flasher.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_loader.h"
#include "esp_loader_io.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "blob_store.h"
#include "device_rescue.h"
#include "rescue_logic.h"
#include "serial_link.h"

#define ESPF_MAX_IMAGES     8
#define ESPF_ROM_BAUD       115200
#define ESPF_DEFAULT_BAUD   460800
#define ESPF_STUB_BLOCK     0x4000
#define ESPF_ROM_BLOCK      0x400
#define ESPF_PT_OFFSET      0x8000
#define ESPF_PT_SIZE        0xC00
#define ESPF_CONNECT_TRIES  3
#define ESPF_REENUM_WAIT_MS 8000    /* an ESP32-C5 took ~5 s to come back in download mode */

/* ===========================================================================
 *  The bridge port: esp-serial-flasher over the device's USB serial function
 * ======================================================================== */

typedef enum { BEFORE_DEFAULT, BEFORE_USB, BEFORE_NONE } before_t;

typedef struct {
    esp_loader_port_t port;
    serial_link_t *link;
    rescue_job_t *job;
    before_t before;
    int64_t deadline_us;
    bool baud_settable;         /* a UART bridge: CHANGE_BAUDRATE matters */
} bridge_port_t;

static bridge_port_t *bp(esp_loader_port_t *port)
{
    return container_of(port, bridge_port_t, port);
}

static esp_loader_error_t port_init(esp_loader_port_t *port)
{
    (void)port;
    return ESP_LOADER_SUCCESS;
}

static void port_deinit(esp_loader_port_t *port)
{
    (void)port;
}

/* After a reset over USB-Serial/JTAG the device usually re-enumerates:
   follow it to its new address on the same busid. */
static void follow_reenumeration(bridge_port_t *p, uint32_t old_devnum, bool expect_drop)
{
    usbip_backend_device_t now;
    bool dropped = false;
    const int wait = expect_drop ? ESPF_REENUM_WAIT_MS : 600;
    if (device_rescue_wait_reenumeration(p->link->busid, old_devnum, wait, &now, &dropped)) {
        vTaskDelay(pdMS_TO_TICKS(300));         /* let it finish enumerating */
        char msg[96];
        if (serial_link_reopen(p->link, msg, sizeof(msg)) == 0) {
            serial_link_prepare(p->link);
            rescue_job_log(p->job, "device re-enumerated as %04x:%04x", p->link->vid, p->link->pid);
        } else {
            rescue_job_log(p->job, "device came back but cannot reopen: %s", msg);
        }
    } else if (dropped) {
        rescue_job_log(p->job, "device dropped off the bus and has not come back yet");
    }
}

static void port_enter_bootloader(esp_loader_port_t *port)
{
    bridge_port_t *p = bp(port);
    if (p->before == BEFORE_NONE) {
        serial_link_discard_input(p->link);
        return;
    }
    const bool usb = p->before == BEFORE_USB || p->link->caps.esp_usb_jtag;
    const uint32_t devnum = p->link->devnum;
    bool dropped = false;
    serial_link_set_baud(p->link, ESPF_ROM_BAUD);
    const int st = device_rescue_run_line_seq(p->link, usb ? RESCUE_MODE_ESP_USB_JTAG_BOOTLOADER
                                                           : RESCUE_MODE_ESP_BOOTLOADER, &dropped);
    if (st != 0) {
        rescue_job_log(p->job, "reset into the bootloader: %s", serial_link_errno_name(st));
    }
    if (usb) {
        /* Entering download mode usually keeps the USB-Serial/JTAG connected
           (ESP32-C6, C5 on .147), so wait long only when a drop was seen. */
        follow_reenumeration(p, devnum, dropped);
        serial_link_set_baud(p->link, ESPF_ROM_BAUD);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    serial_link_discard_input(p->link);
}

static void port_reset_target(esp_loader_port_t *port)
{
    bridge_port_t *p = bp(port);
    serial_link_flush(p->link, 1000);
    bool dropped = false;
    device_rescue_run_line_seq(p->link, p->link->caps.esp_usb_jtag ? RESCUE_MODE_ESP_USB_JTAG_RESET
                                                                  : RESCUE_MODE_ESP_HARD_RESET, &dropped);
}

static void port_start_timer(esp_loader_port_t *port, uint32_t ms)
{
    bp(port)->deadline_us = esp_timer_get_time() + (int64_t)ms * 1000;
}

static uint32_t port_remaining_time(esp_loader_port_t *port)
{
    const int64_t left = bp(port)->deadline_us - esp_timer_get_time();
    return left > 0 ? (uint32_t)(left / 1000) : 0;
}

static void port_delay_ms(esp_loader_port_t *port, uint32_t ms)
{
    serial_link_flush(bp(port)->link, 1000);
    vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
}

static void port_log(esp_loader_port_t *port, esp_loader_log_level_t level, const char *fmt, va_list args)
{
    char line[160];
    vsnprintf(line, sizeof(line), fmt, args);
    rescue_job_log(bp(port)->job, "loader%s: %s", level <= ESP_LOADER_LOG_LEVEL_WARN ? " WARN" : "", line);
}

static void port_log_hex(esp_loader_port_t *port, esp_loader_log_level_t level, const char *label,
                         const uint8_t *data, size_t size)
{
    (void)port;
    (void)level;
    (void)label;
    (void)data;
    (void)size;
}

static esp_loader_error_t port_change_rate(esp_loader_port_t *port, uint32_t rate)
{
    bridge_port_t *p = bp(port);
    if (!p->baud_settable) {
        return ESP_LOADER_SUCCESS;
    }
    serial_link_flush(p->link, 1000);
    vTaskDelay(pdMS_TO_TICKS(20));
    const int st = serial_link_set_baud(p->link, rate);
    vTaskDelay(pdMS_TO_TICKS(50));      /* get rid of what was sent during the change */
    serial_link_discard_input(p->link);
    return st == 0 ? ESP_LOADER_SUCCESS : ESP_LOADER_ERROR_FAIL;
}

static esp_loader_error_t port_write(esp_loader_port_t *port, const uint8_t *data, uint16_t size, uint32_t timeout)
{
    (void)timeout;      /* buffered: sent in 4 KB bulk OUTs and before each read */
    return serial_link_write(bp(port)->link, data, size) == 0 ? ESP_LOADER_SUCCESS : ESP_LOADER_ERROR_FAIL;
}

static esp_loader_error_t port_read(esp_loader_port_t *port, uint8_t *data, uint16_t size, uint32_t timeout)
{
    bridge_port_t *p = bp(port);
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout * 1000;
    size_t got = 0;
    while (got < size) {
        const int64_t left = deadline - esp_timer_get_time();
        if (left <= 0) {
            return ESP_LOADER_ERROR_TIMEOUT;
        }
        const int n = serial_link_read(p->link, data + got, size - got, (uint32_t)(left / 1000) + 1);
        if (n < 0) {
            return ESP_LOADER_ERROR_FAIL;
        }
        got += (size_t)n;
    }
    return ESP_LOADER_SUCCESS;
}

static const esp_loader_port_ops_t k_bridge_port_ops = {
    .init = port_init,
    .deinit = port_deinit,
    .enter_bootloader = port_enter_bootloader,
    .reset_target = port_reset_target,
    .start_timer = port_start_timer,
    .remaining_time = port_remaining_time,
    .delay_ms = port_delay_ms,
    .log = port_log,
    .log_hex = port_log_hex,
    .change_transmission_rate = port_change_rate,
    .write = port_write,
    .read = port_read,
};

/* ===========================================================================
 *  Chips
 * ======================================================================== */

static const char *chip_name(target_chip_t t)
{
    static const char *const k_names[] = {
        "ESP8266", "ESP32", "ESP32-S2", "ESP32-C3", "ESP32-S3", "ESP32-C2", "ESP32-C5",
        "ESP32-H2", "ESP32-C6", "ESP32-P4", "ESP32-C61", "ESP32-S31", "ESP32-H21", "ESP32-H4",
    };
    return (t < sizeof(k_names) / sizeof(k_names[0])) ? k_names[t] : "unknown";
}

/* esptool's disable_watchdogs() over USB-Serial/JTAG (RTC WDT off, SWD
   auto-feed), recorded from esptool 5.4.0's target classes: the RTC
   watchdogs are not reset with the chip and would reset it mid-flash. */
typedef struct {
    uint32_t addr, value;
    bool rmw;                   /* value = read(addr) | value */
} reg_op_t;

typedef struct {
    target_chip_t chip;
    reg_op_t ops[6];
} wdt_table_t;

static const wdt_table_t k_wdt_off[] = {
    { ESP32S3_CHIP, { { 0x600080b0, 0x50d83aa1, false }, { 0x60008098, 0, false }, { 0x600080b0, 0, false },
                      { 0x600080b8, 0x8f1d312a, false }, { 0x600080b4, 0x80000000, true }, { 0x600080b8, 0, false } } },
    { ESP32C3_CHIP, { { 0x600080a8, 0x50d83aa1, false }, { 0x60008090, 0, false }, { 0x600080a8, 0, false },
                      { 0x600080b0, 0x8f1d312a, false }, { 0x600080ac, 0x80000000, true }, { 0x600080b0, 0, false } } },
    { ESP32C2_CHIP, { { 0x6000809c, 0x50d83aa1, false }, { 0x60008084, 0, false }, { 0x6000809c, 0, false },
                      { 0x600080b0, 0x8f1d312a, false }, { 0x600080ac, 0x80000000, true }, { 0x600080b0, 0, false } } },
    { ESP32C6_CHIP, { { 0x600b1c18, 0x50d83aa1, false }, { 0x600b1c00, 0, false }, { 0x600b1c18, 0, false },
                      { 0x600b1c20, 0x50d83aa1, false }, { 0x600b1c1c, 0x00040000, true }, { 0x600b1c20, 0, false } } },
    { ESP32C61_CHIP, { { 0x600b1c18, 0x50d83aa1, false }, { 0x600b1c00, 0, false }, { 0x600b1c18, 0, false },
                       { 0x600b1c20, 0x50d83aa1, false }, { 0x600b1c1c, 0x00040000, true }, { 0x600b1c20, 0, false } } },
    { ESP32C5_CHIP, { { 0x600b1c18, 0x50d83aa1, false }, { 0x600b1c00, 0, false }, { 0x600b1c18, 0, false },
                      { 0x600b1c20, 0x50d83aa1, false }, { 0x600b1c1c, 0x00040000, true }, { 0x600b1c20, 0, false } } },
    { ESP32H2_CHIP, { { 0x600b1c1c, 0x50d83aa1, false }, { 0x600b1c00, 0, false }, { 0x600b1c1c, 0, false },
                      { 0x600b1c24, 0x50d83aa1, false }, { 0x600b1c20, 0x00040000, true }, { 0x600b1c24, 0, false } } },
    { ESP32H21_CHIP, { { 0x600b1c1c, 0x50d83aa1, false }, { 0x600b1c00, 0, false }, { 0x600b1c1c, 0, false },
                       { 0x600b1c24, 0x50d83aa1, false }, { 0x600b1c20, 0x00040000, true }, { 0x600b1c24, 0, false } } },
    { ESP32H4_CHIP, { { 0x60090064, 0x50d83aa1, false }, { 0x60090048, 0, false }, { 0x60090064, 0, false },
                      { 0x600b5420, 0x50d83aa1, false }, { 0x600b541c, 0x00040000, true }, { 0x600b5420, 0, false } } },
    { ESP32P4_CHIP, { { 0x50116018, 0x50d83aa1, false }, { 0x50116000, 0, false }, { 0x50116018, 0, false },
                      { 0x50116020, 0x50d83aa1, false }, { 0x5011601c, 0x00040000, true }, { 0x50116020, 0, false } } },
    { ESP32S31_CHIP, { { 0x20801018, 0x50d83aa1, false }, { 0x20801000, 0, false }, { 0x20801018, 0, false },
                       { 0x600b1c20, 0x50d83aa1, false }, { 0x600b1c1c, 0x00040000, true }, { 0x600b1c20, 0, false } } },
};

static void disable_watchdogs(esp_loader_t *loader, rescue_job_t *job)
{
    const target_chip_t chip = esp_loader_get_target(loader);
    for (size_t i = 0; i < sizeof(k_wdt_off) / sizeof(k_wdt_off[0]); i++) {
        if (k_wdt_off[i].chip != chip) {
            continue;
        }
        for (int k = 0; k < 6; k++) {
            const reg_op_t *op = &k_wdt_off[i].ops[k];
            uint32_t v = op->value;
            if (op->rmw) {
                uint32_t cur = 0;
                if (esp_loader_read_register(loader, op->addr, &cur) != ESP_LOADER_SUCCESS) {
                    rescue_job_log(job, "watchdog disable: read 0x%08lx failed", (unsigned long)op->addr);
                    return;
                }
                v |= cur;
            }
            if (esp_loader_write_register(loader, op->addr, v) != ESP_LOADER_SUCCESS) {
                rescue_job_log(job, "watchdog disable: write 0x%08lx failed", (unsigned long)op->addr);
                return;
            }
        }
        rescue_job_log(job, "RTC watchdog disabled, super watchdog auto-fed (USB-Serial/JTAG)");
        return;
    }
}

static const char *loader_err(esp_loader_error_t e)
{
    switch (e) {
    case ESP_LOADER_SUCCESS: return "ok";
    case ESP_LOADER_ERROR_TIMEOUT: return "timeout (no answer: not in the bootloader? wrong reset wiring?)";
    case ESP_LOADER_ERROR_IMAGE_SIZE: return "image larger than the flash";
    case ESP_LOADER_ERROR_INVALID_MD5: return "MD5 mismatch after writing";
    case ESP_LOADER_ERROR_INVALID_PARAM: return "invalid parameter";
    case ESP_LOADER_ERROR_INVALID_TARGET: return "invalid target";
    case ESP_LOADER_ERROR_UNSUPPORTED_CHIP: return "unsupported chip";
    case ESP_LOADER_ERROR_UNSUPPORTED_FUNC: return "not supported on this target";
    case ESP_LOADER_ERROR_INVALID_RESPONSE: return "invalid response";
    default: return "failed";
    }
}

/* ===========================================================================
 *  Connect
 * ======================================================================== */

struct espf {
    bridge_port_t port;
    serial_link_t link;
    esp_loader_t loader;
    bool stub;
    uint32_t flash_size;
    const char *chip;
    uint8_t *block;             /* esp_flasher_write: one write block (the loader pads it) */
};

static uint32_t parse_u32(const cJSON *v, bool *ok)
{
    *ok = true;
    if (cJSON_IsNumber(v) && v->valuedouble >= 0) {
        return (uint32_t)v->valuedouble;
    }
    if (cJSON_IsString(v)) {
        char *end = NULL;
        const unsigned long n = strtoul(v->valuestring, &end, 0);
        if (end != v->valuestring && *end == '\0') {
            return (uint32_t)n;
        }
    }
    *ok = false;
    return 0;
}

static int espf_connect(espf_t *f, rescue_job_t *job, const char *busid, const cJSON *args)
{
    const cJSON *v;
    int port = 0;
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(args, "port"))) {
        port = (int)v->valuedouble;
    }
    before_t before = BEFORE_DEFAULT;
    if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(args, "before"))) {
        if (strcmp(v->valuestring, "no_reset") == 0 || strcmp(v->valuestring, "no-reset") == 0) {
            before = BEFORE_NONE;
        } else if (strcmp(v->valuestring, "usb_reset") == 0 || strcmp(v->valuestring, "usb-reset") == 0) {
            before = BEFORE_USB;
        } else if (strcmp(v->valuestring, "default_reset") != 0 && strcmp(v->valuestring, "default-reset") != 0) {
            return rescue_job_fail(job, "before: default_reset, usb_reset or no_reset");
        }
    }
    f->stub = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(args, "stub"));

    char msg[128];
    if (serial_link_open(&f->link, busid, port, msg, sizeof(msg)) != 0) {
        return rescue_job_fail(job, "%s", msg);
    }
    serial_link_sync_toggles(&f->link);
    const int st = serial_link_prepare(&f->link);
    if (st != 0) {
        return rescue_job_fail(job, "serial setup failed: %s", serial_link_errno_name(st));
    }
    f->port.port.ops = &k_bridge_port_ops;
    f->port.link = &f->link;
    f->port.job = job;
    f->port.before = before;
    /* Baud matters only through a UART bridge; on a native USB CDC it is ignored. */
    f->port.baud_settable = !f->link.caps.esp_usb_jtag && f->link.vid != 0x303a;
    rescue_job_log(job, "serial: %s %04x:%04x, reset %s", rescue_serial_kind_name(f->link.caps.serial),
                   f->link.vid, f->link.pid,
                   before == BEFORE_NONE ? "none" : (before == BEFORE_USB || f->link.caps.esp_usb_jtag)
                   ? "USB-Serial/JTAG" : "classic DTR/RTS");

    esp_loader_connect_args_t cargs = ESP_LOADER_CONNECT_DEFAULT();
    esp_loader_error_t err = ESP_LOADER_ERROR_FAIL;
    for (int attempt = 1; attempt <= ESPF_CONNECT_TRIES && !rescue_job_cancelled(job); attempt++) {
        rescue_job_progress(job, 2, "connecting");
        esp_loader_init_serial(&f->loader, &f->port.port);
        err = f->stub ? esp_loader_connect_with_stub(&f->loader, &cargs)
                      : esp_loader_connect(&f->loader, &cargs);
        if (err == ESP_LOADER_ERROR_UNSUPPORTED_CHIP && f->stub) {
            rescue_job_log(job, "no flasher stub for this chip, using the ROM loader");
            f->stub = false;
            err = esp_loader_connect(&f->loader, &cargs);
        }
        if (err == ESP_LOADER_SUCCESS) {
            break;
        }
        rescue_job_log(job, "connect attempt %d: %s", attempt, loader_err(err));
    }
    if (err != ESP_LOADER_SUCCESS) {
        return rescue_job_fail(job, "could not connect to the ESP bootloader: %s", loader_err(err));
    }
    const target_chip_t chip = esp_loader_get_target(&f->loader);
    f->chip = chip_name(chip);
    rescue_job_log(job, "connected: %s (%s)", chip_name(chip), f->stub ? "flasher stub" : "ROM loader");
    if (f->link.caps.esp_usb_jtag) {
        disable_watchdogs(&f->loader, job);
    }
    cJSON *res = rescue_job_result(job);
    cJSON_AddStringToObject(res, "chip", chip_name(chip));
    cJSON_AddBoolToObject(res, "stub", f->stub);
    uint8_t mac[6];
    if (esp_loader_read_mac(&f->loader, mac) == ESP_LOADER_SUCCESS) {
        char m[18];
        snprintf(m, sizeof(m), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        cJSON_AddStringToObject(res, "mac", m);
        rescue_job_log(job, "MAC %s", m);
    }
    uint32_t flash_size = 0;
    if (esp_loader_flash_detect_size(&f->loader, &flash_size) == ESP_LOADER_SUCCESS) {
        f->flash_size = flash_size;
        cJSON_AddNumberToObject(res, "flash_size", flash_size);
        rescue_job_log(job, "flash size %lu MB", (unsigned long)(flash_size >> 20));
    }
    int baud = ESPF_DEFAULT_BAUD;
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(args, "baud"))) {
        baud = (int)v->valuedouble;
    }
    if (f->port.baud_settable && baud != ESPF_ROM_BAUD && baud > 0) {
        if (esp_loader_change_transmission_rate(&f->loader, (uint32_t)baud) == ESP_LOADER_SUCCESS) {
            rescue_job_log(job, "baud %d", baud);
        } else {
            rescue_job_log(job, "could not change the baud rate, staying at %d", ESPF_ROM_BAUD);
        }
    }
    return 0;
}

/* An ESP32-S2/S3 app that rebooted into the ROM loader (Arduino's 1200-baud
   touch) set RTC_CNTL_OPTION1.FORCE_DOWNLOAD_BOOT, which survives a reset:
   clear it so the next reset runs the flash, as esptool's hard_reset does
   (arduino-esp32 issue 6762). */
static void clear_force_download(espf_t *f, rescue_job_t *job)
{
    const target_chip_t chip = esp_loader_get_target(&f->loader);
    const uint32_t reg = chip == ESP32S3_CHIP ? 0x6000812Cu : chip == ESP32S2_CHIP ? 0x3F408128u : 0;
    uint32_t v;
    if (reg != 0 && esp_loader_read_register(&f->loader, reg, &v) == ESP_LOADER_SUCCESS && (v & 1u)) {
        if (esp_loader_write_register(&f->loader, reg, v & ~1u) == ESP_LOADER_SUCCESS) {
            rescue_job_log(job, "cleared the forced download boot (RTC_CNTL_OPTION1)");
        }
    }
}

static void espf_after(espf_t *f, rescue_job_t *job, const cJSON *args)
{
    clear_force_download(f, job);
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, "after");
    const bool no_reset = cJSON_IsString(v) &&
        (strcmp(v->valuestring, "no_reset") == 0 || strcmp(v->valuestring, "no-reset") == 0);
    if (no_reset) {
        rescue_job_log(job, "staying in the %s", f->stub ? "flasher stub" : "bootloader");
        return;
    }
    if (cJSON_GetObjectItemCaseSensitive(args, "capture_after") != NULL) {
        /* Reset once the capture is reading, so the first boot is recorded. */
        rescue_job_log(job, "hard reset deferred until the capture is running");
        cJSON_AddStringToObject(rescue_job_result(job), "deferred_reset",
                                f->link.caps.esp_usb_jtag ? "esp_usb_jtag_reset" : "esp_hard_reset");
        return;
    }
    rescue_job_log(job, "hard resetting");
    esp_loader_reset_target(&f->loader);
}

/* ===========================================================================
 *  Jobs
 * ======================================================================== */

int esp_flasher_info_job(rescue_job_t *job, const char *busid, const cJSON *args)
{
    espf_t *f = heap_caps_calloc(1, sizeof(*f), MALLOC_CAP_DEFAULT);
    if (f == NULL) {
        return rescue_job_fail(job, "out of memory");
    }
    int rc = espf_connect(f, job, busid, args);
    if (rc == 0) {
        esp_loader_target_security_info_t si;
        if (esp_loader_get_security_info(&f->loader, &si) == ESP_LOADER_SUCCESS) {
            cJSON *s = cJSON_AddObjectToObject(rescue_job_result(job), "security");
            cJSON_AddBoolToObject(s, "secure_boot", si.secure_boot_enabled);
            cJSON_AddBoolToObject(s, "secure_download_mode", si.secure_download_mode_enabled);
            cJSON_AddBoolToObject(s, "flash_encryption", si.flash_encryption_enabled);
            cJSON_AddBoolToObject(s, "usb_disabled", si.usb_disabled);
            cJSON_AddNumberToObject(s, "eco_version", si.eco_version);
        }
        espf_after(f, job, args);
    }
    serial_link_close(&f->link);
    heap_caps_free(f);
    return rc;
}

int esp_flasher_boot_uf2(rescue_job_t *job, const char *busid)
{
    espf_t *f = heap_caps_calloc(1, sizeof(*f), MALLOC_CAP_DEFAULT);
    uint8_t *pt = heap_caps_malloc(ESPF_PT_SIZE, MALLOC_CAP_DEFAULT);
    if (f == NULL || pt == NULL) {
        heap_caps_free(f);
        heap_caps_free(pt);
        return rescue_job_fail(job, "out of memory");
    }
    cJSON *args = cJSON_CreateObject();     /* defaults: reset into the ROM loader, stub */
    int rc = espf_connect(f, job, busid, args);
    cJSON_Delete(args);
    uint32_t ota_off = 0, ota_size = 0, factory_off = 0;
    char factory_label[17] = "";
    if (rc == 0 && esp_loader_flash_read(&f->loader, pt, ESPF_PT_OFFSET, ESPF_PT_SIZE) != ESP_LOADER_SUCCESS) {
        rc = rescue_job_fail(job, "could not read the partition table at 0x%x", ESPF_PT_OFFSET);
    }
    for (int i = 0; rc == 0 && i + 32 <= ESPF_PT_SIZE; i += 32) {
        const uint8_t *e = &pt[i];
        if (e[0] != 0xAA || e[1] != 0x50) {
            break;                                      /* end of the table (or its MD5 entry) */
        }
        const uint32_t off = e[4] | e[5] << 8 | e[6] << 16 | (uint32_t)e[7] << 24;
        const uint32_t size = e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24;
        if (e[2] == 1 && e[3] == 0) {                   /* data, ota */
            ota_off = off;
            ota_size = size;
        } else if (e[2] == 0 && e[3] == 0) {            /* app, factory */
            factory_off = off;
            memcpy(factory_label, &e[12], 16);
        }
    }
    if (rc == 0 && (ota_size == 0 || factory_off == 0)) {
        clear_force_download(f, job);
        esp_loader_reset_target(&f->loader);           /* back to its app */
        rc = rescue_job_fail(job, "no UF2 bootloader to start: the partition table has %s",
                             ota_size == 0 ? "no otadata" : "no factory app (TinyUF2's uf2 partition)");
    }
    if (rc == 0) {
        rescue_job_log(job, "factory app '%s' at 0x%lx: erasing otadata (0x%lx, %lu bytes) so it boots",
                       factory_label, (unsigned long)factory_off, (unsigned long)ota_off, (unsigned long)ota_size);
        if (esp_loader_flash_erase_region(&f->loader, ota_off, ota_size) != ESP_LOADER_SUCCESS) {
            rc = rescue_job_fail(job, "erasing otadata failed");
        } else {
            clear_force_download(f, job);
            esp_loader_reset_target(&f->loader);
        }
    }
    serial_link_close(&f->link);
    heap_caps_free(pt);
    heap_caps_free(f);
    return rc;
}

typedef struct {
    char file[BLOB_NAME_MAX];
    uint32_t offset;
    const uint8_t *data;
    size_t len;
} image_t;

int esp_flasher_job(rescue_job_t *job, const char *busid, const cJSON *args)
{
    image_t images[ESPF_MAX_IMAGES];
    int n = 0;
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(args, "images");
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) == 0) {
        return rescue_job_fail(job, "images: [{\"file\": <staged name>, \"offset\": <address>}] required");
    }
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (n >= ESPF_MAX_IMAGES) {
            return rescue_job_fail(job, "at most %d images", ESPF_MAX_IMAGES);
        }
        const cJSON *file = cJSON_GetObjectItemCaseSensitive(it, "file");
        bool ok = false;
        const uint32_t off = parse_u32(cJSON_GetObjectItemCaseSensitive(it, "offset"), &ok);
        if (!cJSON_IsString(file) || !ok || (off & 3)) {
            return rescue_job_fail(job, "image %d: file (string) and a 4-byte aligned offset required", n);
        }
        strlcpy(images[n].file, file->valuestring, sizeof(images[n].file));
        images[n].offset = off;
        n++;
    }
    for (int i = 0; i < n; i++) {
        if (!blob_acquire(images[i].file, &images[i].data, &images[i].len)) {
            for (int k = 0; k < i; k++) {
                blob_release(images[k].file);
            }
            return rescue_job_fail(job, "no staged file named %s (upload it with PUT /api/rescue/files/<name>)",
                                   images[i].file);
        }
    }
    size_t total = 0;
    for (int i = 0; i < n; i++) {
        total += images[i].len;
    }

    espf_t *f = heap_caps_calloc(1, sizeof(*f), MALLOC_CAP_DEFAULT);
    int rc = f ? espf_connect(f, job, busid, args) : rescue_job_fail(job, "out of memory");
    const bool verify = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(args, "verify"));
    if (rc == 0 && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "erase_all"))) {
        rescue_job_progress(job, 5, "erasing flash");
        rescue_job_log(job, "erasing the whole flash");
        const esp_loader_error_t e = esp_loader_flash_erase(&f->loader);
        if (e != ESP_LOADER_SUCCESS) {
            rc = rescue_job_fail(job, "erase failed: %s", loader_err(e));
        }
    }
    cJSON *written = cJSON_AddArrayToObject(rescue_job_result(job), "images");
    size_t done = 0;
    uint8_t *tail = heap_caps_malloc(ESPF_STUB_BLOCK, MALLOC_CAP_DEFAULT);
    if (tail == NULL && rc == 0) {
        rc = rescue_job_fail(job, "out of memory");
    }
    for (int i = 0; rc == 0 && i < n; i++) {
        image_t *im = &images[i];
        const uint32_t block = f->stub ? ESPF_STUB_BLOCK : ESPF_ROM_BLOCK;
        const size_t padded = (im->len + 3) & ~(size_t)3;     /* 0xFF-padded to a 4-byte multiple */
        esp_loader_flash_cfg_t cfg = {
            .offset = im->offset,
            .image_size = (uint32_t)padded,
            .block_size = block,
            .skip_verify = !verify,
        };
        char stage[96];
        snprintf(stage, sizeof(stage), "writing %s at 0x%lx", im->file, (unsigned long)im->offset);
        rescue_job_progress(job, 5 + (int)(90 * done / (total ? total : 1)), stage);
        rescue_job_log(job, "%s: %u bytes at 0x%08lx", im->file, (unsigned)im->len, (unsigned long)im->offset);
        const int64_t t0 = esp_timer_get_time();
        esp_loader_error_t e = esp_loader_flash_start(&f->loader, &cfg);
        size_t pos = 0;
        while (e == ESP_LOADER_SUCCESS && pos < padded) {
            if (rescue_job_cancelled(job)) {
                e = ESP_LOADER_ERROR_FAIL;
                break;
            }
            size_t chunk = padded - pos < block ? padded - pos : block;
            const uint8_t *src = im->data + pos;
            if (pos + chunk > im->len) {                       /* the padded tail */
                memset(tail, 0xFF, chunk);
                memcpy(tail, src, im->len - pos);
                src = tail;
            }
            e = esp_loader_flash_write(&f->loader, &cfg, src, (uint32_t)chunk);
            pos += chunk;
            rescue_job_progress(job, 5 + (int)(90 * (done + (pos < im->len ? pos : im->len)) / (total ? total : 1)), NULL);
        }
        if (e == ESP_LOADER_SUCCESS) {
            rescue_job_progress(job, -1, verify ? "verifying" : NULL);
            e = esp_loader_flash_finish(&f->loader, &cfg);
        }
        const double secs = (double)(esp_timer_get_time() - t0) / 1e6;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "file", im->file);
        cJSON_AddNumberToObject(o, "offset", im->offset);
        cJSON_AddNumberToObject(o, "size", (double)im->len);
        cJSON_AddBoolToObject(o, "ok", e == ESP_LOADER_SUCCESS);
        cJSON_AddBoolToObject(o, "verified", verify && e == ESP_LOADER_SUCCESS);
        cJSON_AddNumberToObject(o, "seconds", secs);
        cJSON_AddItemToArray(written, o);
        if (e != ESP_LOADER_SUCCESS) {
            rc = rescue_job_fail(job, "%s at 0x%lx: %s", im->file, (unsigned long)im->offset,
                                 rescue_job_cancelled(job) ? "cancelled" : loader_err(e));
            break;
        }
        rescue_job_log(job, "%s written%s in %.1f s (%.0f kbit/s)", im->file, verify ? " and verified (MD5)" : "",
                       secs, secs > 0 ? im->len * 8 / secs / 1000 : 0);
        done += im->len;
    }
    heap_caps_free(tail);
    if (rc == 0) {
        espf_after(f, job, args);
    }
    if (f != NULL) {
        serial_link_close(&f->link);
        heap_caps_free(f);
    }
    for (int i = 0; i < n; i++) {
        blob_release(images[i].file);
    }
    return rc;
}

/* ===========================================================================
 *  A connection for the DUT flash browser
 * ======================================================================== */

espf_t *esp_flasher_open(rescue_job_t *job, const char *busid, const cJSON *args, bool *encrypted)
{
    espf_t *f = heap_caps_calloc(1, sizeof(*f), MALLOC_CAP_DEFAULT);
    if (f == NULL) {
        rescue_job_fail(job, "out of memory");
        return NULL;
    }
    f->block = heap_caps_malloc(ESPF_STUB_BLOCK, MALLOC_CAP_DEFAULT);
    if (f->block == NULL || espf_connect(f, job, busid, args) != 0) {
        if (f->block == NULL) {
            rescue_job_fail(job, "out of memory");
        }
        serial_link_close(&f->link);
        heap_caps_free(f->block);
        heap_caps_free(f);
        return NULL;
    }
    if (f->flash_size == 0) {
        rescue_job_fail(job, "could not detect the flash size");
        serial_link_close(&f->link);
        heap_caps_free(f->block);
        heap_caps_free(f);
        return NULL;
    }
    esp_loader_target_security_info_t si;
    *encrypted = esp_loader_get_security_info(&f->loader, &si) == ESP_LOADER_SUCCESS && si.flash_encryption_enabled;
    if (!f->stub) {
        rescue_job_log(job, "no flasher stub: reads go 64 bytes per command (slow)");
    }
    return f;
}

uint32_t esp_flasher_flash_size(const espf_t *f)
{
    return f->flash_size;
}

const char *esp_flasher_chip(const espf_t *f)
{
    return f->chip;
}

int esp_flasher_read(espf_t *f, uint32_t addr, uint8_t *buf, uint32_t len)
{
    const esp_loader_error_t e = esp_loader_flash_read(&f->loader, buf, addr, len);
    return e == ESP_LOADER_SUCCESS ? 0 : -1;
}

int esp_flasher_write(espf_t *f, rescue_job_t *job, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    const uint32_t block = f->stub ? ESPF_STUB_BLOCK : ESPF_ROM_BLOCK;
    esp_loader_flash_cfg_t cfg = { .offset = addr, .image_size = len, .block_size = block, .skip_verify = false };
    /* FLASH_BEGIN erases the sectors the image covers, then the data
       follows in blocks the loader pads with 0xFF in place. */
    esp_loader_error_t e = esp_loader_flash_start(&f->loader, &cfg);
    for (uint32_t pos = 0; e == ESP_LOADER_SUCCESS && pos < len; pos += block) {
        const uint32_t n = len - pos < block ? len - pos : block;
        memcpy(f->block, buf + pos, n);
        e = esp_loader_flash_write(&f->loader, &cfg, f->block, n);
    }
    if (e == ESP_LOADER_SUCCESS) {
        e = esp_loader_flash_finish(&f->loader, &cfg);
    }
    if (e != ESP_LOADER_SUCCESS) {
        rescue_job_log(job, "writing 0x%lx (+%lu): %s", (unsigned long)addr, (unsigned long)len, loader_err(e));
        return -1;
    }
    return 0;
}

void esp_flasher_close(espf_t *f, rescue_job_t *job, const cJSON *args)
{
    if (f == NULL) {
        return;
    }
    espf_after(f, job, args);
    serial_link_close(&f->link);
    heap_caps_free(f->block);
    heap_caps_free(f);
}
