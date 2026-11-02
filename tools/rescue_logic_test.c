/*
 * Host table test for main/rescue_logic.c (no ESP-IDF needed):
 *
 *   gcc -std=c11 -Wall -Wextra -Werror -Imain main/rescue_logic.c tools/rescue_logic_test.c \
 *       -o /tmp/rescue_logic_test && /tmp/rescue_logic_test
 *
 * Expected values are independent of the implementation where possible:
 * CH341 divisors match the old Linux table algorithm (factor/divisor), FTDI
 * divisors are the well-known 232BM values, sequences are the esptool states.
 */
#include <stdio.h>
#include <string.h>

#include "rescue_logic.h"

static int s_fail;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { printf("FAIL %s:%d: %s = 0x%llx, want 0x%llx\n", __FILE__, __LINE__, #a, _a, _b); s_fail++; } } while (0)

/* Descriptor builders */
static size_t put(uint8_t *d, size_t off, const uint8_t *b, size_t n)
{
    memcpy(&d[off], b, n);
    return off + n;
}

static size_t cfg_hdr(uint8_t *d, uint8_t nif)
{
    const uint8_t h[9] = { 9, 2, 0, 0, nif, 1, 0, 0x80, 50 };
    return put(d, 0, h, 9);
}

static size_t intf(uint8_t *d, size_t off, uint8_t num, uint8_t alt, uint8_t neps, uint8_t c, uint8_t s, uint8_t p)
{
    const uint8_t b[9] = { 9, 4, num, alt, neps, c, s, p, 0 };
    return put(d, off, b, 9);
}

static size_t ep(uint8_t *d, size_t off, uint8_t addr, uint8_t attr, uint16_t mps)
{
    const uint8_t b[7] = { 7, 5, addr, attr, (uint8_t)mps, (uint8_t)(mps >> 8), 0 };
    return put(d, off, b, 7);
}

static size_t cdc_func(uint8_t *d, size_t off)
{
    /* header + call management + ACM + union class-specific descriptors */
    const uint8_t cs[] = { 5, 0x24, 0, 0x10, 0x01, 5, 0x24, 1, 0, 1, 4, 0x24, 2, 2, 5, 0x24, 6, 0, 1 };
    return put(d, off, cs, sizeof(cs));
}

static void finish(uint8_t *d, size_t len)
{
    d[2] = (uint8_t)len;
    d[3] = (uint8_t)(len >> 8);
}

static void test_esp_usb_jtag(void)
{
    uint8_t d[256];
    size_t o = cfg_hdr(d, 3);
    const uint8_t iad[8] = { 8, 0x0B, 0, 2, 2, 2, 0, 0 };
    o = put(d, o, iad, 8);
    o = intf(d, o, 0, 0, 1, 0x02, 0x02, 0x00);
    o = cdc_func(d, o);
    o = ep(d, o, 0x82, 0x03, 64);
    o = intf(d, o, 1, 0, 2, 0x0A, 0x00, 0x00);
    o = ep(d, o, 0x01, 0x02, 64);
    o = ep(d, o, 0x81, 0x02, 64);
    o = intf(d, o, 2, 0, 2, 0xFF, 0xFF, 0x01);
    o = ep(d, o, 0x02, 0x02, 64);
    o = ep(d, o, 0x83, 0x02, 64);
    finish(d, o);

    rescue_config_t cfg;
    CHECK_EQ(rescue_parse_config(d, o, &cfg), 3);
    CHECK_EQ(cfg.ifaces[0].ep_intr_in, 0x82);
    CHECK_EQ(cfg.ifaces[1].ep_in, 0x81);
    CHECK_EQ(cfg.ifaces[1].ep_out, 0x01);
    rescue_caps_t k;
    rescue_classify(0x303a, 0x1001, 0x0101, &cfg, 0, &k);
    CHECK_EQ(k.serial, RESCUE_SERIAL_CDC_ACM);
    CHECK_EQ(k.serial_ctrl_if, 0);
    CHECK_EQ(k.serial_data_if, 1);
    CHECK_EQ(k.ep_in, 0x81);
    CHECK_EQ(k.ep_out, 0x01);
    CHECK(k.esp_usb_jtag);
    CHECK_EQ(k.rp_reset_if, -1);
    CHECK_EQ(rescue_auto_bootloader(&k, 0x303a), RESCUE_MODE_ESP_USB_JTAG_BOOTLOADER);
    CHECK(rescue_mode_applicable(&k, RESCUE_MODE_ESP_USB_JTAG_RESET));
    CHECK(!rescue_mode_applicable(&k, RESCUE_MODE_ESP_BOOTLOADER));
    CHECK(!rescue_mode_applicable(&k, RESCUE_MODE_TOUCH_1200));

    rescue_ctrl_t r[4];
    CHECK_EQ(rescue_lines_requests(&k, true, false, r, 4), 1);
    CHECK_EQ(r[0].bm, 0x21);
    CHECK_EQ(r[0].req, 0x22);
    CHECK_EQ(r[0].value, 0x0001);
    CHECK_EQ(r[0].index, 0);
    CHECK_EQ(rescue_baud_requests(&k, 115200, 0, false, r, 4), 1);
    CHECK_EQ(r[0].req, 0x20);
    CHECK_EQ(r[0].len, 7);
    const uint8_t lc[7] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };
    CHECK(memcmp(r[0].data, lc, 7) == 0);
}

static void test_pico_sdk_cdc_reset(void)
{
    uint8_t d[256];
    size_t o = cfg_hdr(d, 3);
    o = intf(d, o, 0, 0, 1, 0x02, 0x02, 0x00);
    o = cdc_func(d, o);
    o = ep(d, o, 0x81, 0x03, 8);
    o = intf(d, o, 1, 0, 2, 0x0A, 0x00, 0x00);
    o = ep(d, o, 0x02, 0x02, 64);
    o = ep(d, o, 0x82, 0x02, 64);
    o = intf(d, o, 2, 0, 0, 0xFF, 0x00, 0x01);
    finish(d, o);
    rescue_config_t cfg;
    rescue_parse_config(d, o, &cfg);
    rescue_caps_t k;
    rescue_classify(0x2e8a, 0x000a, 0x0100, &cfg, 0, &k);
    CHECK_EQ(k.serial, RESCUE_SERIAL_CDC_ACM);
    CHECK_EQ(k.rp_reset_if, 2);
    CHECK_EQ(k.picoboot_if, -1);
    CHECK_EQ(rescue_auto_bootloader(&k, 0x2e8a), RESCUE_MODE_RP_BOOTSEL);
    CHECK(rescue_mode_applicable(&k, RESCUE_MODE_TOUCH_1200));
    rescue_ctrl_t r;
    rescue_rp_reset_request(k.rp_reset_if, true, &r);
    CHECK_EQ(r.bm, 0x21);
    CHECK_EQ(r.req, 0x01);
    CHECK_EQ(r.value, 0);
    CHECK_EQ(r.index, 2);
    rescue_rp_reset_request(k.rp_reset_if, false, &r);
    CHECK_EQ(r.req, 0x02);

    /* Plain CDC (e.g. a SAMD UF2 board): 1200-baud touch */
    rescue_classify(0x239a, 0x801f, 0x0100, &cfg, 0, &k);
    k.rp_reset_if = -1;
    CHECK_EQ(rescue_auto_bootloader(&k, 0x239a), RESCUE_MODE_TOUCH_1200);
    /* WCH CDC bridge (CH9102) in front of an ESP: classic reset */
    CHECK_EQ(rescue_auto_bootloader(&k, 0x1a86), RESCUE_MODE_ESP_BOOTLOADER);
}

static void test_rp_bootsel(void)
{
    uint8_t d[256];
    size_t o = cfg_hdr(d, 2);
    o = intf(d, o, 0, 0, 2, 0x08, 0x06, 0x50);
    o = ep(d, o, 0x01, 0x02, 64);
    o = ep(d, o, 0x82, 0x02, 64);
    o = intf(d, o, 1, 0, 2, 0xFF, 0x00, 0x00);
    o = ep(d, o, 0x03, 0x02, 64);
    o = ep(d, o, 0x84, 0x02, 64);
    finish(d, o);
    rescue_config_t cfg;
    rescue_parse_config(d, o, &cfg);
    rescue_caps_t k;
    rescue_classify(0x2e8a, 0x0003, 0x0100, &cfg, 0, &k);
    CHECK_EQ(k.serial, RESCUE_SERIAL_NONE);
    CHECK_EQ(k.picoboot_if, 1);
    CHECK_EQ(k.picoboot_ep_in, 0x84);
    CHECK_EQ(k.picoboot_ep_out, 0x03);
    CHECK_EQ(k.rp_chip, RESCUE_CHIP_RP2040);
    CHECK_EQ(rescue_auto_bootloader(&k, 0x2e8a), RESCUE_MODE_INVALID);
    CHECK(rescue_mode_applicable(&k, RESCUE_MODE_PICOBOOT_REBOOT));
    CHECK(!rescue_mode_applicable(&k, RESCUE_MODE_PICOBOOT_BOOTSEL));
    rescue_classify(0x2e8a, 0x000f, 0x0100, &cfg, 0, &k);
    CHECK_EQ(k.rp_chip, RESCUE_CHIP_RP2350);
    CHECK(rescue_mode_applicable(&k, RESCUE_MODE_PICOBOOT_BOOTSEL));

    uint8_t cmd[32];
    rescue_picoboot_reboot_cmd(RESCUE_CHIP_RP2040, false, 0x11223344, 500, cmd);
    const uint8_t want2040[32] = {
        0x0b, 0xd1, 0x1f, 0x43, 0x44, 0x33, 0x22, 0x11, 0x02, 12, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0x00, 0x20, 0x04, 0x20, 0xf4, 0x01, 0, 0, 0, 0, 0, 0,
    };
    CHECK(memcmp(cmd, want2040, 32) == 0);
    rescue_picoboot_reboot_cmd(RESCUE_CHIP_RP2350, true, 1, 500, cmd);
    CHECK_EQ(cmd[8], 0x0a);
    CHECK_EQ(cmd[9], 16);
    CHECK_EQ(cmd[16], 0x02);
    CHECK_EQ(cmd[20], 0xf4);
    CHECK_EQ(cmd[21], 0x01);
    rescue_ctrl_t r;
    rescue_picoboot_if_reset_request(1, &r);
    CHECK_EQ(r.bm, 0x41);
    CHECK_EQ(r.req, 0x41);
    CHECK_EQ(r.index, 1);
}

static rescue_config_t vendor_cfg(int ports)
{
    uint8_t d[256];
    size_t o = cfg_hdr(d, (uint8_t)ports);
    for (int i = 0; i < ports; i++) {
        o = intf(d, o, (uint8_t)i, 0, 2, 0xFF, 0x00, 0x00);
        o = ep(d, o, (uint8_t)(0x81 + i * 2), 0x02, 64);
        o = ep(d, o, (uint8_t)(0x02 + i * 2), 0x02, 64);
    }
    finish(d, o);
    rescue_config_t cfg;
    rescue_parse_config(d, o, &cfg);
    return cfg;
}

static void test_cp210x(void)
{
    rescue_config_t cfg = vendor_cfg(2);    /* CP2105 */
    rescue_caps_t k;
    rescue_classify(0x10c4, 0xea70, 0x0100, &cfg, 1, &k);
    CHECK_EQ(k.serial, RESCUE_SERIAL_CP210X);
    CHECK_EQ(k.serial_ports, 2);
    CHECK_EQ(k.serial_ctrl_if, 1);
    CHECK_EQ(k.ep_in, 0x83);
    rescue_ctrl_t r[4];
    CHECK_EQ(rescue_lines_requests(&k, true, false, r, 4), 1);
    CHECK_EQ(r[0].bm, 0x41);
    CHECK_EQ(r[0].req, 0x07);
    CHECK_EQ(r[0].value, 0x0301);
    CHECK_EQ(r[0].index, 1);
    CHECK_EQ(rescue_baud_requests(&k, 115200, 0, false, r, 4), 3);
    CHECK_EQ(r[0].req, 0x00);   /* IFC_ENABLE */
    CHECK_EQ(r[0].value, 1);
    CHECK_EQ(r[1].req, 0x03);   /* SET_LINE_CTL */
    CHECK_EQ(r[1].value, 0x0800);
    CHECK_EQ(r[2].req, 0x1E);   /* SET_BAUDRATE */
    CHECK_EQ(r[2].len, 4);
    const uint8_t b[4] = { 0x00, 0xC2, 0x01, 0x00 };
    CHECK(memcmp(r[2].data, b, 4) == 0);
    CHECK_EQ(rescue_auto_bootloader(&k, 0x10c4), RESCUE_MODE_ESP_BOOTLOADER);
    CHECK(rescue_mode_applicable(&k, RESCUE_MODE_ESP_HARD_RESET));
    CHECK(!rescue_mode_applicable(&k, RESCUE_MODE_TOUCH_1200));
}

static void test_ch34x(void)
{
    /* Old Linux table algorithm: factor = 1532620800 / baud, divisor 3,
       shifted while > 0xfff0; value (0x10000 - factor) & 0xff00 | divisor. */
    CHECK_EQ(rescue_ch341_divisor(115200, false), 0xCC03);
    CHECK_EQ(rescue_ch341_divisor(9600, false), 0xB202);
    CHECK_EQ(rescue_ch341_divisor(57600, false), 0x9803);
    rescue_config_t cfg = vendor_cfg(1);
    rescue_caps_t k;
    rescue_classify(0x1a86, 0x7523, 0x0254, &cfg, 0, &k);
    CHECK_EQ(k.serial, RESCUE_SERIAL_CH34X);
    rescue_ctrl_t r[4];
    CHECK_EQ(rescue_lines_requests(&k, true, true, r, 4), 1);
    CHECK_EQ(r[0].bm, 0x40);
    CHECK_EQ(r[0].req, 0xA4);
    CHECK_EQ(r[0].value, 0xFF9F);
    rescue_lines_requests(&k, false, false, r, 4);
    CHECK_EQ(r[0].value, 0xFFFF);
    rescue_lines_requests(&k, true, false, r, 4);
    CHECK_EQ(r[0].value, 0xFFDF);
    /* version 0x31: divisor with bit 7 set, then LCR */
    CHECK_EQ(rescue_baud_requests(&k, 115200, 0x31, false, r, 4), 2);
    CHECK_EQ(r[0].req, 0x9A);
    CHECK_EQ(r[0].value, 0x1312);
    CHECK_EQ(r[0].index, 0xCC83);
    CHECK_EQ(r[1].value, 0x2518);
    CHECK_EQ(r[1].index, 0x00C3);
    /* version 0x27: no bit 7, no LCR write */
    CHECK_EQ(rescue_baud_requests(&k, 115200, 0x27, false, r, 4), 1);
    CHECK_EQ(r[0].index, 0xCC03);
}

static void test_ftdi(void)
{
    CHECK_EQ(rescue_ftdi_232bm_divisor(9600), 0x4138);
    CHECK_EQ(rescue_ftdi_232bm_divisor(115200), 0x001A);
    CHECK_EQ(rescue_ftdi_232bm_divisor(3000000), 0);
    CHECK_EQ(rescue_ftdi_232bm_divisor(2000000), 1);
    CHECK_EQ(rescue_ftdi_2232h_divisor(115200), 0x2C068);

    rescue_config_t cfg = vendor_cfg(1);
    rescue_caps_t k;
    rescue_classify(0x0403, 0x6001, 0x0600, &cfg, 0, &k);       /* FT232R */
    CHECK_EQ(k.serial, RESCUE_SERIAL_FTDI);
    CHECK_EQ(k.ftdi_channel, 0);
    CHECK(!k.ftdi_hispeed);
    rescue_ctrl_t r[4];
    CHECK_EQ(rescue_baud_requests(&k, 115200, 0, false, r, 4), 3);
    CHECK_EQ(r[0].req, 0x03);
    CHECK_EQ(r[0].value, 0x001A);
    CHECK_EQ(r[0].index, 0x0000);
    CHECK_EQ(r[1].req, 0x04);
    CHECK_EQ(r[1].value, 0x0008);
    rescue_lines_requests(&k, false, true, r, 4);
    CHECK_EQ(r[0].bm, 0x40);
    CHECK_EQ(r[0].req, 0x01);
    CHECK_EQ(r[0].value, 0x0302);
    CHECK_EQ(r[0].index, 0);

    cfg = vendor_cfg(2);
    rescue_classify(0x0403, 0x6010, 0x0700, &cfg, 1, &k);       /* FT2232H port B */
    CHECK_EQ(k.ftdi_channel, 2);
    CHECK(k.ftdi_hispeed);
    rescue_baud_requests(&k, 115200, 0, false, r, 4);
    CHECK_EQ(r[0].value, 0xC068);
    CHECK_EQ(r[0].index, 0x0202);
    rescue_classify(0x0403, 0x6015, 0x1000, &cfg, 0, &k);       /* FT-X */
    CHECK_EQ(k.ftdi_channel, 1);
    rescue_baud_requests(&k, 9600, 0, false, r, 4);
    CHECK_EQ(r[0].value, 0x4138);
    CHECK_EQ(r[0].index, 0x0001);
    rescue_classify(0x0403, 0x6001, 0x0200, &cfg, 0, &k);       /* FT232A: refused */
    CHECK_EQ(rescue_baud_requests(&k, 9600, 0, false, r, 4), -1);

    /* packets of mps 4, the last one short */
    uint8_t buf[11] = { 0x01, 0x60, 'a', 'b', 0x01, 0x60, 'c', 'd', 0x01, 0x60, 'e' };
    CHECK_EQ(rescue_ftdi_strip_status(buf, 11, 4), 5);
    CHECK(memcmp(buf, "abcde", 5) == 0);
    uint8_t st[2] = { 0x01, 0x60 };
    CHECK_EQ(rescue_ftdi_strip_status(st, 2, 64), 0);
}

static void expect_seq(rescue_mode_t m, const int (*states)[3], int n, int commit)
{
    const rescue_line_seq_t *s = rescue_mode_line_seq(m);
    CHECK(s != NULL);
    if (s == NULL) {
        return;
    }
    CHECK_EQ(s->num_steps, n);
    CHECK_EQ(s->commit_step, commit);
    for (int i = 0; i < n && i < s->num_steps; i++) {
        CHECK_EQ(s->steps[i].dtr, states[i][0]);
        CHECK_EQ(s->steps[i].rts, states[i][1]);
        CHECK_EQ(s->steps[i].delay_ms, states[i][2]);
    }
}

static void test_sequences(void)
{
    /* esp_pylib unix_tight_bootloader_reset (PIN_LOW = asserted) */
    const int boot[][3] = { {0, 0, 0}, {1, 1, 0}, {0, 1, 100}, {1, 0, 50}, {0, 0, 0} };
    expect_seq(RESCUE_MODE_ESP_BOOTLOADER, boot, 5, -1);
    const int hard[][3] = { {0, 1, 100}, {0, 0, 0} };
    expect_seq(RESCUE_MODE_ESP_HARD_RESET, hard, 2, -1);
    /* usb_jtag_bootloader_reset: distinct (dtr, rts) states of its pyserial writes */
    const int jtag[][3] = { {0, 0, 100}, {1, 0, 100}, {1, 1, 0}, {0, 1, 100}, {0, 0, 0} };
    expect_seq(RESCUE_MODE_ESP_USB_JTAG_BOOTLOADER, jtag, 5, 3);
    const int jreset[][3] = { {0, 1, 200}, {0, 0, 200} };
    expect_seq(RESCUE_MODE_ESP_USB_JTAG_RESET, jreset, 2, 0);
    CHECK(rescue_mode_line_seq(RESCUE_MODE_TOUCH_1200) == NULL);

    for (int m = 0; m <= RESCUE_MODE_AUTO_BOOTLOADER; m++) {
        CHECK_EQ(rescue_mode_from_name(rescue_mode_name((rescue_mode_t)m)), m);
        CHECK(rescue_mode_description((rescue_mode_t)m)[0] != '\0');
    }
    CHECK_EQ(rescue_mode_from_name("nope"), RESCUE_MODE_INVALID);
}

static void test_console(void)
{
    uint8_t b[8];
    CHECK_EQ(rescue_named_poke("enter", b, sizeof(b)), 2);
    CHECK(b[0] == '\r' && b[1] == '\n');
    CHECK_EQ(rescue_named_poke("ctrl_c", b, sizeof(b)), 1);
    CHECK_EQ(b[0], 0x03);
    CHECK_EQ(rescue_named_poke("bogus", b, sizeof(b)), -1);
    CHECK_EQ(rescue_hex_decode("0d 0a", b, sizeof(b)), 2);
    CHECK(b[0] == 0x0d && b[1] == 0x0a);
    CHECK_EQ(rescue_hex_decode("0d0", b, sizeof(b)), -1);
    CHECK_EQ(rescue_hex_decode("zz", b, sizeof(b)), -1);
    CHECK_EQ(rescue_hex_decode("000102030405060708", b, sizeof(b)), -1);

    const uint8_t raw[] = { 0xff, ' ', 'o', 'k', ' ', 0xe2, 0x82, 0xac, 0x00, 0xe2, 0x82 };
    char out[32];
    rescue_sanitize_text(raw, sizeof(raw), out, sizeof(out));
    CHECK(strcmp(out, "? ok \xe2\x82\xac???") == 0);
    rescue_sanitize_text(raw, sizeof(raw), out, 4);
    CHECK(strcmp(out, "? o") == 0);

    CHECK(strcmp(rescue_classify_transcript(""), "silent") == 0);
    CHECK(strcmp(rescue_classify_transcript(" \r\n"), "silent") == 0);
    CHECK(strcmp(rescue_classify_transcript("Adafruit CircuitPython 9.2\r\n>>> "), "circuitpython_repl") == 0);
    CHECK(strcmp(rescue_classify_transcript("MicroPython v1.24\r\n>>> "), "micropython_repl") == 0);
    CHECK(strcmp(rescue_classify_transcript("You are in Safe Mode because"), "circuitpython_safe_mode") == 0);
    CHECK(strcmp(rescue_classify_transcript("ESP-ROM:esp32s3-20210327\r\n"), "boot_banner_detected") == 0);
    CHECK(strcmp(rescue_classify_transcript("hello"), "other") == 0);
}

int main(void)
{
    test_esp_usb_jtag();
    test_pico_sdk_cdc_reset();
    test_rp_bootsel();
    test_cp210x();
    test_ch34x();
    test_ftdi();
    test_sequences();
    test_console();
    if (s_fail) {
        printf("%d check(s) failed\n", s_fail);
        return 1;
    }
    printf("rescue_logic: all checks passed\n");
    return 0;
}
