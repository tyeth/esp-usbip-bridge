#include "picoboot_flasher.h"

#include <errno.h>
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
#include "usb_backend.h"

#define PB_SECTOR           4096
#define PB_PAGE             256
#define PB_FLASH_BASE       0x10000000u
#define PB_FLASH_END        0x11000000u     /* 16 MB XIP window */
#define PB_FAMILY_ABSOLUTE  0xe48bff57u
#define PB_MAX_SECTORS      4096
#define PB_CMD_TIMEOUT_MS   3000
#define PB_BOOTSEL_WAIT_MS  6000

typedef struct {
    char busid[32];
    uint32_t devnum;
    rescue_caps_t caps;
    rescue_job_t *job;
    uint32_t token;
} pb_t;

static int load_caps(pb_t *p)
{
    usbip_backend_device_t d;
    if (!usb_backend_get_device_by_busid(p->busid, &d)) {
        return -ENODEV;
    }
    uint8_t *cfg = malloc(1024);
    if (cfg == NULL) {
        return -ENOMEM;
    }
    rescue_config_t c;
    rescue_parse_config(cfg, usb_backend_get_config_descriptor(p->busid, cfg, 1024), &c);
    free(cfg);
    rescue_classify(d.id_vendor, d.id_product, d.bcd_device, &c, 0, &p->caps);
    p->devnum = d.devnum;
    return 0;
}

static int ctrl(pb_t *p, uint8_t bm, uint8_t req, uint16_t index, uint8_t *in, size_t in_len)
{
    usb_setup_packet_t setup = {
        .bmRequestType = bm, .bRequest = req, .wValue = 0, .wIndex = index, .wLength = (uint16_t)in_len,
    };
    return usb_backend_transfer_sync(p->busid, 0, &setup, NULL, 0, in, in_len, 1000, NULL, NULL);
}

/* After a failed command: what the device says, then un-stall it. */
static void report_status(pb_t *p, const char *what)
{
    uint8_t st[16] = {0};
    const int rc = ctrl(p, 0xC1, RESCUE_PICOBOOT_STATUS_REQ, (uint16_t)p->caps.picoboot_if, st, sizeof(st));
    if (rc == 0) {
        const uint32_t code = (uint32_t)st[4] | st[5] << 8 | st[6] << 16 | (uint32_t)st[7] << 24;
        rescue_job_log(p->job, "%s: PICOBOOT status %lu %s", what, (unsigned long)code, rescue_picoboot_status_name(code));
    }
    ctrl(p, 0x41, 0x41, (uint16_t)p->caps.picoboot_if, NULL, 0);     /* PICOBOOT_IF_RESET */
}

/* One command: 32-byte header OUT, data phase (OUT or IN), then the ACK in
   the opposite direction of the data (IN for no data). */
static int pb_exec(pb_t *p, const uint8_t cmd[32], const uint8_t *out, uint8_t *in, size_t len, const char *what)
{
    int rc = usb_backend_transfer_sync(p->busid, p->caps.picoboot_ep_out, NULL, cmd, 32, NULL, 0,
                                       PB_CMD_TIMEOUT_MS, NULL, NULL);
    const bool is_in = (cmd[8] & 0x80) != 0;
    if (rc == 0 && len > 0) {
        size_t got = 0;
        rc = is_in ? usb_backend_transfer_sync(p->busid, p->caps.picoboot_ep_in, NULL, NULL, 0, in, len,
                                               PB_CMD_TIMEOUT_MS, &got, NULL)
                   : usb_backend_transfer_sync(p->busid, p->caps.picoboot_ep_out, NULL, out, len, NULL, 0,
                                               PB_CMD_TIMEOUT_MS, NULL, NULL);
        if (rc == 0 && is_in && got != len) {
            rc = -EIO;
        }
    }
    if (rc == 0) {
        uint8_t ack[64];
        rc = is_in ? usb_backend_transfer_sync(p->busid, p->caps.picoboot_ep_out, NULL, NULL, 0, NULL, 0,
                                               PB_CMD_TIMEOUT_MS, NULL, NULL)
                   : usb_backend_transfer_sync(p->busid, p->caps.picoboot_ep_in, NULL, NULL, 0, ack, sizeof(ack),
                                               PB_CMD_TIMEOUT_MS, NULL, NULL);
    }
    if (rc != 0) {
        rescue_job_log(p->job, "%s: %s (%d)", what, serial_link_errno_name(rc), rc);
        report_status(p, what);
    }
    return rc;
}

/* Get the device into BOOTSEL (PICOBOOT) on the same busid. */
static int ensure_bootsel(pb_t *p, bool allowed)
{
    if (load_caps(p) != 0) {
        return rescue_job_fail(p->job, "no device on %s", p->busid);
    }
    if (p->caps.picoboot_if >= 0) {
        return 0;
    }
    if (!allowed) {
        return rescue_job_fail(p->job, "device is not in BOOTSEL (no PICOBOOT interface); allow enter_bootsel");
    }
    const uint32_t devnum = p->devnum;
    if (p->caps.rp_reset_if >= 0) {
        rescue_job_log(p->job, "rebooting into BOOTSEL through the reset interface");
        rescue_ctrl_t r;
        rescue_rp_reset_request(p->caps.rp_reset_if, true, &r);
        ctrl(p, r.bm, r.req, r.index, NULL, 0);
    } else if (p->caps.serial == RESCUE_SERIAL_CDC_ACM) {
        rescue_job_log(p->job, "1200-baud touch to enter the bootloader");
        serial_link_t *l = calloc(1, sizeof(*l));
        char msg[96];
        if (l != NULL && serial_link_open(l, p->busid, 0, msg, sizeof(msg)) == 0) {
            serial_link_set_lines(l, true, true);
            serial_link_set_baud(l, 1200);
            vTaskDelay(pdMS_TO_TICKS(50));
            serial_link_set_lines(l, false, false);
            serial_link_close(l);
        }
        free(l);
    } else {
        return rescue_job_fail(p->job, "device is not in BOOTSEL and has no reset interface or CDC to get it there");
    }
    usbip_backend_device_t now;
    bool dropped;
    if (!device_rescue_wait_reenumeration(p->busid, devnum, PB_BOOTSEL_WAIT_MS, &now, &dropped)) {
        return rescue_job_fail(p->job, "device did not come back in BOOTSEL (%s)", dropped ? "dropped, not back" : "did not reset");
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    if (load_caps(p) != 0 || p->caps.picoboot_if < 0) {
        return rescue_job_fail(p->job, "device came back as %04x:%04x without a PICOBOOT interface",
                               now.id_vendor, now.id_product);
    }
    rescue_job_log(p->job, "in BOOTSEL: %04x:%04x", now.id_vendor, now.id_product);
    return 0;
}

static int pb_begin(pb_t *p)
{
    if (ctrl(p, 0x41, 0x41, (uint16_t)p->caps.picoboot_if, NULL, 0) != 0) {
        return rescue_job_fail(p->job, "PICOBOOT interface reset failed");
    }
    /* Bulk toggles to DATA0 on both sides: a new claim starts the host's at
       DATA0, the device's continue from the last session. */
    ctrl(p, 0x02, 0x01, p->caps.picoboot_ep_in, NULL, 0);    /* CLEAR_FEATURE(ENDPOINT_HALT) */
    ctrl(p, 0x02, 0x01, p->caps.picoboot_ep_out, NULL, 0);
    uint8_t cmd[32];
    const uint8_t excl = 1;     /* EXCLUSIVE: the bootrom's mass storage goes away meanwhile */
    rescue_picoboot_cmd(RESCUE_PC_EXCLUSIVE_ACCESS, ++p->token, &excl, 1, 0, cmd);
    if (pb_exec(p, cmd, NULL, NULL, 0, "exclusive access") != 0) {
        return rescue_job_fail(p->job, "PICOBOOT exclusive access refused");
    }
    rescue_picoboot_cmd(RESCUE_PC_EXIT_XIP, ++p->token, NULL, 0, 0, cmd);
    if (pb_exec(p, cmd, NULL, NULL, 0, "exit XIP") != 0) {
        return rescue_job_fail(p->job, "PICOBOOT exit XIP failed");
    }
    return 0;
}

static void pb_reboot(pb_t *p)
{
    uint8_t cmd[32];
    rescue_picoboot_reboot_cmd(p->caps.rp_chip, false, ++p->token, 500, cmd);
    pb_exec(p, cmd, NULL, NULL, 0, "reboot");
}

static int cmp_u32(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

int picoboot_info_job(rescue_job_t *job, const char *busid, const cJSON *args)
{
    pb_t p = { .job = job };
    strlcpy(p.busid, busid, sizeof(p.busid));
    int rc = ensure_bootsel(&p, !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(args, "enter_bootsel")));
    if (rc == 0) {
        cJSON *res = rescue_job_result(job);
        cJSON_AddStringToObject(res, "chip", p.caps.rp_chip == RESCUE_CHIP_RP2040 ? "rp2040"
                                           : p.caps.rp_chip == RESCUE_CHIP_RP2350 ? "rp2350" : "unknown");
        cJSON_AddNumberToObject(res, "picoboot_interface", p.caps.picoboot_if);
        rc = pb_begin(&p);
        if (rc == 0) {
            /* The image header: first 256 bytes of flash. */
            uint8_t *buf = malloc(PB_PAGE);
            uint8_t cmd[32];
            rescue_picoboot_range_cmd(RESCUE_PC_READ, ++p.token, PB_FLASH_BASE, PB_PAGE, PB_PAGE, cmd);
            if (buf != NULL && pb_exec(&p, cmd, NULL, buf, PB_PAGE, "read flash") == 0) {
                bool blank = true;
                for (int i = 0; i < PB_PAGE; i++) {
                    blank &= buf[i] == 0xFF;
                }
                cJSON_AddBoolToObject(res, "flash_blank_at_start", blank);
            }
            free(buf);
        }
        if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(args, "reboot")) &&
                cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "reboot"))) {
            pb_reboot(&p);
        }
    }
    usb_backend_session_ended(busid);
    return rc;
}

int picoboot_flasher_job(rescue_job_t *job, const char *busid, const cJSON *args)
{
    const cJSON *file = cJSON_GetObjectItemCaseSensitive(args, "file");
    if (!cJSON_IsString(file)) {
        return rescue_job_fail(job, "file (staged UF2 or .bin) required");
    }
    const uint8_t *data;
    size_t len;
    if (!blob_acquire(file->valuestring, &data, &len)) {
        return rescue_job_fail(job, "no staged file named %s", file->valuestring);
    }
    pb_t p = { .job = job };
    strlcpy(p.busid, busid, sizeof(p.busid));
    uint32_t *sectors = heap_caps_malloc(PB_MAX_SECTORS * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    uint8_t *sec = malloc(PB_SECTOR);
    uint8_t *rd = malloc(PB_SECTOR);
    int rc = 0;
    if (sectors == NULL || sec == NULL || rd == NULL) {
        rc = rescue_job_fail(job, "out of memory");
        goto done;
    }

    /* What to write: UF2 blocks, or a raw image at an address. */
    rescue_uf2_block_t blk;
    const bool is_uf2 = rescue_uf2_block(data, len, 0, &blk);
    const size_t nblocks = is_uf2 ? len / RESCUE_UF2_BLOCK : 0;
    uint32_t raw_addr = PB_FLASH_BASE;
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(args, "address");
    if (cJSON_IsString(a)) {
        raw_addr = (uint32_t)strtoul(a->valuestring, NULL, 0);
    } else if (cJSON_IsNumber(a)) {
        raw_addr = (uint32_t)a->valuedouble;
    }
    size_t nsec = 0;
    uint32_t family = 0;
    bool skip_absolute = false;
    if (is_uf2) {
        /* RP2350 UF2s start with an absolute-family block at 0x10ffff00 (the
           RP2350-E10 workaround for drag-and-drop); picotool skips it, and on
           a smaller flash it is not there to write.  Skip absolute blocks when
           the image has a real family. */
        unsigned skipped = 0;
        for (size_t i = 0; i < nblocks && !skip_absolute; i++) {
            skip_absolute = rescue_uf2_block(data, len, i, &blk) && !(blk.flags & RESCUE_UF2_FLAG_NOT_MAIN) &&
                            blk.family_id && blk.family_id != PB_FAMILY_ABSOLUTE;
        }
        for (size_t i = 0; i < nblocks; i++) {
            if (!rescue_uf2_block(data, len, i, &blk)) {
                rc = rescue_job_fail(job, "UF2 block %u is malformed", (unsigned)i);
                goto done;
            }
            if (blk.flags & RESCUE_UF2_FLAG_NOT_MAIN) {
                continue;
            }
            if (skip_absolute && blk.family_id == PB_FAMILY_ABSOLUTE) {
                skipped++;
                continue;
            }
            if (blk.family_id && family == 0) {
                family = blk.family_id;
            }
            if (blk.target_addr < PB_FLASH_BASE || blk.target_addr + blk.payload_size > PB_FLASH_END) {
                rc = rescue_job_fail(job, "UF2 block %u targets 0x%08lx, outside flash (RAM UF2s are not supported)",
                                     (unsigned)i, (unsigned long)blk.target_addr);
                goto done;
            }
            for (uint32_t s = blk.target_addr & ~(PB_SECTOR - 1); s < blk.target_addr + blk.payload_size; s += PB_SECTOR) {
                bool seen = false;
                for (size_t k = 0; k < nsec && !seen; k++) {
                    seen = sectors[k] == s;
                }
                if (!seen) {
                    if (nsec >= PB_MAX_SECTORS) {
                        rc = rescue_job_fail(job, "image too large");
                        goto done;
                    }
                    sectors[nsec++] = s;
                }
            }
        }
        qsort(sectors, nsec, sizeof(uint32_t), cmp_u32);
        if (skipped) {
            rescue_job_log(job, "skipped %u absolute-family block(s) (RP2350-E10 workaround)", skipped);
        }
        rescue_job_log(job, "UF2: %u blocks, family %s, %u sectors to write", (unsigned)nblocks,
                       family ? (rescue_uf2_family_name(family) ? rescue_uf2_family_name(family) : "unknown") : "none",
                       (unsigned)nsec);
    } else {
        if (raw_addr < PB_FLASH_BASE || raw_addr + len > PB_FLASH_END || (raw_addr & (PB_PAGE - 1))) {
            rc = rescue_job_fail(job, "raw image: address must be 256-byte aligned inside flash (0x10000000..)");
            goto done;
        }
        for (uint32_t s = raw_addr & ~(PB_SECTOR - 1); s < raw_addr + len && nsec < PB_MAX_SECTORS; s += PB_SECTOR) {
            sectors[nsec++] = s;
        }
        rescue_job_log(job, "raw image: %u bytes at 0x%08lx, %u sectors", (unsigned)len, (unsigned long)raw_addr,
                       (unsigned)nsec);
    }
    if (nsec == 0) {
        rc = rescue_job_fail(job, "nothing to write");
        goto done;
    }

    rescue_job_progress(job, 2, "entering BOOTSEL");
    rc = ensure_bootsel(&p, !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(args, "enter_bootsel")));
    if (rc != 0) {
        goto done;
    }
    if (family && !rescue_uf2_family_fits_rp(family, p.caps.rp_chip) &&
            !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "force_family"))) {
        const char *fn = rescue_uf2_family_name(family);
        rc = rescue_job_fail(job, "UF2 family %s does not match this %s (force_family to override)",
                             fn ? fn : "unknown", p.caps.rp_chip == RESCUE_CHIP_RP2040 ? "RP2040" : "RP2350");
        goto done;
    }
    rc = pb_begin(&p);
    if (rc != 0) {
        goto done;
    }
    const bool verify = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(args, "verify"));
    uint8_t cmd[32];
    /* erase_all: wipe the whole flash first (old firmware, secrets, file
       systems).  The size is not guessed: erasing past the end of the part
       is undefined, and RP2040 cannot report it. */
    bool erased = false;
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "erase_all"))) {
        const cJSON *fs = cJSON_GetObjectItemCaseSensitive(args, "flash_size");
        uint32_t size = 0;
        if (cJSON_IsNumber(fs)) {
            size = (uint32_t)fs->valuedouble;
        } else if (cJSON_IsString(fs)) {
            char *end = NULL;
            size = (uint32_t)strtoul(fs->valuestring, &end, 0);
            if (end && (*end == 'M' || *end == 'm')) {
                size <<= 20;
            } else if (end && (*end == 'K' || *end == 'k')) {
                size <<= 10;
            }
        }
        if (size < PB_SECTOR || size > PB_FLASH_END - PB_FLASH_BASE || (size % PB_SECTOR)) {
            rc = rescue_job_fail(job, "erase_all needs flash_size (e.g. \"2MB\", \"4MB\", \"16MB\"): the size of this board's flash");
            goto done;
        }
        rescue_job_log(job, "erasing the whole flash: %lu KB", (unsigned long)(size >> 10));
        const uint32_t step = 0x10000;
        for (uint32_t off = 0; off < size; off += step) {
            const uint32_t n = size - off < step ? size - off : step;
            rescue_picoboot_range_cmd(RESCUE_PC_FLASH_ERASE, ++p.token, PB_FLASH_BASE + off, n, 0, cmd);
            if (pb_exec(&p, cmd, NULL, NULL, 0, "erase") != 0) {
                rc = rescue_job_fail(job, "erasing 0x%08lx failed", (unsigned long)(PB_FLASH_BASE + off));
                goto done;
            }
            rescue_job_progress(job, 3 + (int)(20 * (off + n) / size), "erasing flash");
        }
        erased = true;
        cJSON_AddNumberToObject(rescue_job_result(job), "erased", size);
    }
    const int64_t t0 = esp_timer_get_time();
    for (size_t k = 0; k < nsec && rc == 0; k++) {
        if (rescue_job_cancelled(job)) {
            rc = rescue_job_fail(job, "cancelled");
            break;
        }
        const uint32_t s = sectors[k];
        /* Build the sector: what the image has, the current flash elsewhere. */
        memset(sec, 0xFF, PB_SECTOR);
        uint32_t covered = 0;                   /* bitmap of 256-byte pages */
        if (is_uf2) {
            for (size_t i = 0; i < nblocks; i++) {
                rescue_uf2_block(data, len, i, &blk);
                if ((blk.flags & RESCUE_UF2_FLAG_NOT_MAIN) || (skip_absolute && blk.family_id == PB_FAMILY_ABSOLUTE) ||
                        blk.target_addr + blk.payload_size <= s ||
                        blk.target_addr >= s + PB_SECTOR) {
                    continue;
                }
                const uint32_t from = blk.target_addr > s ? blk.target_addr : s;
                const uint32_t to = (blk.target_addr + blk.payload_size < s + PB_SECTOR)
                                    ? blk.target_addr + blk.payload_size : s + PB_SECTOR;
                memcpy(&sec[from - s], &blk.data[from - blk.target_addr], to - from);
                for (uint32_t pg = (from - s) / PB_PAGE; pg <= (to - s - 1) / PB_PAGE; pg++) {
                    covered |= 1u << pg;
                }
            }
        } else {
            const uint32_t from = raw_addr > s ? raw_addr : s;
            const uint32_t to = (raw_addr + len < s + PB_SECTOR) ? raw_addr + (uint32_t)len : s + PB_SECTOR;
            memcpy(&sec[from - s], &data[from - raw_addr], to - from);
            for (uint32_t pg = (from - s) / PB_PAGE; pg <= (to - s - 1) / PB_PAGE; pg++) {
                covered |= 1u << pg;
            }
        }
        if (covered != 0xFFFF && !erased) {
            /* Keep the parts of this sector the image does not cover. */
            rescue_picoboot_range_cmd(RESCUE_PC_READ, ++p.token, s, PB_SECTOR, PB_SECTOR, cmd);
            if (pb_exec(&p, cmd, NULL, rd, PB_SECTOR, "read sector") != 0) {
                rc = rescue_job_fail(job, "reading sector 0x%08lx failed", (unsigned long)s);
                break;
            }
            for (int pg = 0; pg < PB_SECTOR / PB_PAGE; pg++) {
                if (!(covered & (1u << pg))) {
                    memcpy(&sec[pg * PB_PAGE], &rd[pg * PB_PAGE], PB_PAGE);
                }
            }
        }
        rescue_picoboot_range_cmd(RESCUE_PC_FLASH_ERASE, ++p.token, s, PB_SECTOR, 0, cmd);
        if (!erased && pb_exec(&p, cmd, NULL, NULL, 0, "erase") != 0) {
            rc = rescue_job_fail(job, "erasing sector 0x%08lx failed", (unsigned long)s);
            break;
        }
        rescue_picoboot_range_cmd(RESCUE_PC_WRITE, ++p.token, s, PB_SECTOR, PB_SECTOR, cmd);
        if (pb_exec(&p, cmd, sec, NULL, PB_SECTOR, "write") != 0) {
            rc = rescue_job_fail(job, "writing sector 0x%08lx failed", (unsigned long)s);
            break;
        }
        if (verify) {
            rescue_picoboot_range_cmd(RESCUE_PC_READ, ++p.token, s, PB_SECTOR, PB_SECTOR, cmd);
            if (pb_exec(&p, cmd, NULL, rd, PB_SECTOR, "verify read") != 0 || memcmp(rd, sec, PB_SECTOR) != 0) {
                rc = rescue_job_fail(job, "verify failed at sector 0x%08lx", (unsigned long)s);
                break;
            }
        }
        rescue_job_progress(job, (erased ? 25 : 5) + (int)((erased ? 70 : 90) * (k + 1) / nsec), "writing flash");
    }
    if (rc == 0) {
        const double secs = (double)(esp_timer_get_time() - t0) / 1e6;
        rescue_job_log(job, "%u sectors written%s in %.1f s", (unsigned)nsec, verify ? " and verified" : "", secs);
        cJSON *res = rescue_job_result(job);
        cJSON_AddStringToObject(res, "chip", p.caps.rp_chip == RESCUE_CHIP_RP2040 ? "rp2040" : "rp2350");
        cJSON_AddNumberToObject(res, "sectors", (double)nsec);
        cJSON_AddNumberToObject(res, "first", sectors[0]);
        cJSON_AddNumberToObject(res, "end", sectors[nsec - 1] + PB_SECTOR);
        cJSON_AddBoolToObject(res, "verified", verify);
        cJSON_AddNumberToObject(res, "seconds", secs);
        if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(args, "reboot"))) {
            rescue_job_log(job, "rebooting into the application");
            pb_reboot(&p);
            usbip_backend_device_t now;
            bool dropped;
            if (device_rescue_wait_reenumeration(busid, p.devnum, 4000, &now, &dropped)) {
                rescue_job_log(job, "came back as %04x:%04x %s", now.id_vendor, now.id_product, now.product);
                char vp[10];
                snprintf(vp, sizeof(vp), "%04x:%04x", now.id_vendor, now.id_product);
                cJSON_AddStringToObject(res, "came_back_as", vp);
            }
        }
    }
done:
    usb_backend_session_ended(busid);
    heap_caps_free(sectors);
    free(sec);
    free(rd);
    blob_release(file->valuestring);
    return rc;
}
