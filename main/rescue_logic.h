#ifndef RESCUE_LOGIC_H
#define RESCUE_LOGIC_H

/*
 * Device rescue: the protocol knowledge behind the per-device "interrogate
 * and rescue" panel, with no ESP-IDF dependencies so it can be compiled and
 * table-tested on a host (see tools/rescue_logic_test.c).
 *
 *   - classify a device from its configuration descriptor: CDC-ACM, the
 *     vendor UART bridges (CP210x, CH34x, FTDI), ESP USB-Serial/JTAG, the
 *     RP2040/RP2350 reset interface and the PICOBOOT interface
 *   - the control requests that set line coding and DTR/RTS on each of them
 *     (from the Linux cdc-acm, cp210x, ch341 and ftdi_sio drivers)
 *   - the canned reset sequences as explicit {dtr, rts, delay} tables (from
 *     esptool / esp_pylib serial_reset.py)
 *   - console helpers: named pokes, FTDI status-byte stripping, UTF-8
 *     sanitising and the serial_probe transcript classification
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RESCUE_MAX_IFACES 16

typedef enum {
    RESCUE_SERIAL_NONE = 0,
    RESCUE_SERIAL_CDC_ACM,
    RESCUE_SERIAL_CP210X,
    RESCUE_SERIAL_CH34X,
    RESCUE_SERIAL_FTDI,
} rescue_serial_kind_t;

typedef struct {
    uint8_t number;             /* bInterfaceNumber */
    uint8_t cls, subclass, protocol;
    uint8_t ep_in, ep_out;      /* first bulk IN / OUT endpoint, 0 = none */
    uint16_t mps_in, mps_out;
    uint8_t ep_intr_in;         /* first interrupt IN endpoint, 0 = none */
    uint8_t num_endpoints;
} rescue_iface_t;

typedef struct {
    rescue_iface_t ifaces[RESCUE_MAX_IFACES];   /* alternate setting 0 only */
    int num_ifaces;
} rescue_config_t;

/* Parse a configuration descriptor (wTotalLength bytes).  Returns the number
   of interfaces found (alternate setting 0). */
int rescue_parse_config(const uint8_t *desc, size_t len, rescue_config_t *out);

typedef enum {
    RESCUE_CHIP_UNKNOWN = 0,
    RESCUE_CHIP_RP2040,
    RESCUE_CHIP_RP2350,
} rescue_rp_chip_t;

typedef struct {
    rescue_serial_kind_t serial;
    int serial_ctrl_if;         /* interface addressed by line requests (CDC comm, vendor port); -1 none */
    int serial_data_if;         /* interface with the bulk endpoints; -1 none */
    uint8_t ep_in, ep_out;
    uint16_t mps_in, mps_out;
    int serial_ports;           /* vendor bridges: number of ports (CP2105/FT2232: 2) */
    uint16_t ftdi_channel;      /* wIndex low byte for FTDI requests */
    bool ftdi_hispeed;          /* FT2232H/FT4232H/FT232H family: 120 MHz divisor */
    bool ftdi_baud_supported;   /* false for SIO / FT232A (no baud encoder here) */
    bool esp_usb_jtag;          /* 303a:1001 USB-Serial/JTAG */
    int rp_reset_if;            /* RP2040/RP2350 reset interface (FF/00/01), -1 none */
    int picoboot_if;            /* PICOBOOT interface (FF/00/00 on 2e8a BOOTSEL), -1 none */
    uint8_t picoboot_ep_in, picoboot_ep_out;
    rescue_rp_chip_t rp_chip;
} rescue_caps_t;

/* What the bridge can do with a device.  `port` selects a vendor bridge port
   or a CDC function by index (0 = the first). */
void rescue_classify(uint16_t vid, uint16_t pid, uint16_t bcd_device,
                     const rescue_config_t *cfg, int port, rescue_caps_t *out);

const char *rescue_serial_kind_name(rescue_serial_kind_t kind);

/* One control request.  bmRequestType bit 7 gives the direction; OUT data is
   in data[0..len). */
typedef struct {
    uint8_t bm, req;
    uint16_t value, index;
    uint16_t len;
    uint8_t data[8];
} rescue_ctrl_t;

/* The request(s) that set DTR and RTS together (one request on every
   supported chip).  Returns the number of requests written to out (0 when
   the device has no serial function). */
int rescue_lines_requests(const rescue_caps_t *caps, bool dtr, bool rts, rescue_ctrl_t *out, int max);

/* CP210x needs its UART enabled before line and data requests. */
bool rescue_cp210x_enable_request(const rescue_caps_t *caps, rescue_ctrl_t *out);

/* The requests that set the baud rate (8N1, no flow control).  ch34x_version
   is the chip version read with CH341_REQ_READ_VERSION (it decides the LCR
   register write and bit 7 of the divisor); ch34x_limited is the
   limited-prescaler quirk.  Returns the request count, or -1 when this chip
   or rate cannot be encoded. */
int rescue_baud_requests(const rescue_caps_t *caps, uint32_t baud,
                         uint8_t ch34x_version, bool ch34x_limited,
                         rescue_ctrl_t *out, int max);

/* Encoders, exposed for the table tests. */
void rescue_cdc_line_coding(uint32_t baud, uint8_t out[7]);
int rescue_ch341_divisor(uint32_t baud, bool limited_prescaler);
uint32_t rescue_ftdi_232bm_divisor(uint32_t baud);
uint32_t rescue_ftdi_2232h_divisor(uint32_t baud);

/* CH34x setup requests (version read, quirk probe, serial init). */
#define RESCUE_CH341_REQ_READ_VERSION 0x5F
#define RESCUE_CH341_REQ_READ_REG     0x95
#define RESCUE_CH341_REQ_WRITE_REG    0x9A
#define RESCUE_CH341_REQ_SERIAL_INIT  0xA1
#define RESCUE_CH341_REQ_MODEM_CTRL   0xA4
#define RESCUE_CH341_REG_BREAK        0x05

/* Canned DTR/RTS sequences.  Each step sets both lines at once, then waits
   delay_ms.  Steps at index >= commit_step may fail because the target
   drops off the bus (an internal USB peripheral resetting); -1 = never. */
typedef struct {
    bool dtr, rts;
    uint16_t delay_ms;
} rescue_line_step_t;

typedef struct {
    const char *name;
    const char *description;
    const rescue_line_step_t *steps;
    int num_steps;
    int commit_step;
} rescue_line_seq_t;

typedef enum {
    RESCUE_MODE_ESP_BOOTLOADER = 0,     /* classic DTR/RTS auto-reset circuit, IO0 low */
    RESCUE_MODE_ESP_HARD_RESET,         /* classic circuit, EN pulse */
    RESCUE_MODE_ESP_USB_JTAG_BOOTLOADER,/* 303a:1001 USB-Serial/JTAG into download mode */
    RESCUE_MODE_ESP_USB_JTAG_RESET,     /* 303a:1001 hard reset */
    RESCUE_MODE_TOUCH_1200,             /* CDC 1200-baud touch: UF2 / SAM-BA / Arduino bootloader */
    RESCUE_MODE_RP_BOOTSEL,             /* RP2 reset interface: reboot to BOOTSEL */
    RESCUE_MODE_RP_REBOOT,              /* RP2 reset interface: reboot to flash */
    RESCUE_MODE_PICOBOOT_REBOOT,        /* PICOBOOT (in BOOTSEL): reboot to the application */
    RESCUE_MODE_PICOBOOT_BOOTSEL,       /* PICOBOOT on RP2350: reboot into BOOTSEL again */
    RESCUE_MODE_COUNT,
    RESCUE_MODE_AUTO_BOOTLOADER = RESCUE_MODE_COUNT,
    RESCUE_MODE_INVALID,
} rescue_mode_t;

const char *rescue_mode_name(rescue_mode_t mode);
const char *rescue_mode_description(rescue_mode_t mode);
rescue_mode_t rescue_mode_from_name(const char *name);
/* The DTR/RTS table of a line-sequence mode, NULL for the others. */
const rescue_line_seq_t *rescue_mode_line_seq(rescue_mode_t mode);
/* Whether the device has what `mode` needs (an explicit request for a line
   mode on any serial device is allowed; this is what the UI offers). */
bool rescue_mode_applicable(const rescue_caps_t *caps, rescue_mode_t mode);
/* The mode a plain "enter the bootloader" maps to, RESCUE_MODE_INVALID if none. */
rescue_mode_t rescue_auto_bootloader(const rescue_caps_t *caps, uint16_t vid);

/* RP2 reset interface: CLASS|INTERFACE request (as picotool sends it). */
void rescue_rp_reset_request(int iface, bool to_bootsel, rescue_ctrl_t *out);
/* PICOBOOT: the interface-reset control request and a 32-byte reboot command. */
void rescue_picoboot_if_reset_request(int iface, rescue_ctrl_t *out);
void rescue_picoboot_reboot_cmd(rescue_rp_chip_t chip, bool to_bootsel, uint32_t token,
                                uint32_t delay_ms, uint8_t out[32]);

/* Any PICOBOOT command: 32 bytes with args (at most 16 bytes) and the data
   phase length.  Bit 7 of cmd_id marks an IN (device-to-host) data phase. */
void rescue_picoboot_cmd(uint8_t cmd_id, uint32_t token, const uint8_t *args, uint8_t args_len,
                         uint32_t transfer_len, uint8_t out[32]);
/* PC_FLASH_ERASE / PC_READ / PC_WRITE range arguments. */
void rescue_picoboot_range_cmd(uint8_t cmd_id, uint32_t token, uint32_t addr, uint32_t size,
                               uint32_t transfer_len, uint8_t out[32]);
#define RESCUE_PC_EXCLUSIVE_ACCESS 0x01
#define RESCUE_PC_FLASH_ERASE      0x03
#define RESCUE_PC_READ             0x84
#define RESCUE_PC_WRITE            0x05
#define RESCUE_PC_EXIT_XIP         0x06
#define RESCUE_PICOBOOT_STATUS_REQ 0x42  /* IF_CMD_STATUS control request (16 bytes IN) */
const char *rescue_picoboot_status_name(uint32_t status);

/* UF2 (https://github.com/microsoft/uf2) */
#define RESCUE_UF2_BLOCK          512
#define RESCUE_UF2_FLAG_NOT_MAIN  0x00000001u
#define RESCUE_UF2_FLAG_FAMILY    0x00002000u

typedef struct {
    uint32_t flags;
    uint32_t target_addr;
    uint32_t payload_size;
    uint32_t block_no;
    uint32_t num_blocks;
    uint32_t family_id;         /* 0 when the block carries none */
    const uint8_t *data;        /* payload_size bytes */
} rescue_uf2_block_t;

/* Parse the index-th 512-byte block.  False when the magic numbers or the
   payload size are wrong. */
bool rescue_uf2_block(const uint8_t *file, size_t len, size_t index, rescue_uf2_block_t *out);
/* Family name for a UF2 family id ("rp2040", "rp2350-arm-s", "samd21", ...), NULL if unknown. */
const char *rescue_uf2_family_name(uint32_t family_id);
/* Whether a UF2 family can be loaded into this RP2 chip over PICOBOOT. */
bool rescue_uf2_family_fits_rp(uint32_t family_id, rescue_rp_chip_t chip);

/* USB mass storage, Bulk-Only Transport + SCSI */
void rescue_msc_cbw(uint32_t tag, uint32_t data_len, bool data_in, uint8_t lun,
                    const uint8_t *cdb, uint8_t cdb_len, uint8_t out[31]);
/* Parse a 13-byte CSW: true when the signature and tag match. */
bool rescue_msc_csw(const uint8_t *csw, size_t len, uint32_t tag, uint32_t *residue, uint8_t *status);
/* READ(10) (0x28) / WRITE(10) (0x2A) */
void rescue_scsi_rw10(uint8_t op, uint32_t lba, uint16_t count, uint8_t cdb[10]);
/* READ CAPACITY(10) data: last LBA and block length (big endian). */
void rescue_scsi_capacity(const uint8_t data[8], uint32_t *blocks, uint32_t *block_size);

/* SAM-BA (BOSSA) */
/* CRC-16/XMODEM as SAM-BA's Z command computes it (poly 0x1021, init 0). */
uint16_t rescue_crc16_xmodem(const uint8_t *data, size_t len, uint16_t crc);

typedef struct {
    const char *family;         /* "SAMD21", "SAMD51", "SAME51", ... or NULL when unsupported */
    uint32_t app_offset;        /* the usual bootloader size (Arduino/Adafruit bootloaders) */
    uint32_t user_ram;          /* RAM BOSSA uses for its buffers */
} rescue_samba_chip_t;

/* From CPUID (0xE000ED00) and the DSU device id (0x41002018); flash_size
   (from NVMCTRL PARAM) picks the SAMD21 RAM layout. */
void rescue_samba_chip(uint32_t cpuid, uint32_t device_id, uint32_t flash_size, rescue_samba_chip_t *out);
/* Flash size from NVMCTRL PARAM (0x41004008): NVMP pages of 8 << PSZ bytes. */
uint32_t rescue_samba_flash_size(uint32_t nvm_param, uint32_t *page_size);

/* Auto-baud: how much a burst of bytes looks like text received at the
   right baud rate, 0-100 (-1 when there are too few bytes to tell).
   Wrong rates show up as framing garbage: NULs, 0xFF, stray high bytes and
   control characters, invalid UTF-8. */
int rescue_baud_score(const uint8_t *buf, size_t len);
#define RESCUE_BAUD_SCORE_GOOD 85   /* accept a candidate at or above this */
#define RESCUE_BAUD_SCORE_BAD  50   /* re-probe a running stream below this */
/* The rates tried by "auto", in order, after any the caller prefers. */
extern const uint32_t k_rescue_auto_bauds[];
extern const int k_rescue_num_auto_bauds;

/* Console helpers */
/* A named poke ("enter", "ctrl_c", "ctrl_d") into out; returns its length, -1 if unknown. */
int rescue_named_poke(const char *name, uint8_t *out, size_t cap);
/* Hex string (spaces/underscores ignored) into out; returns the length, -1 if malformed. */
int rescue_hex_decode(const char *hex, uint8_t *out, size_t cap);
/* FTDI prefixes every IN packet (mps bytes) with two modem-status bytes:
   strip them in place.  Returns the payload length. */
size_t rescue_ftdi_strip_status(uint8_t *buf, size_t len, uint16_t mps);
/* Copy bytes as JSON-safe UTF-8 text: valid UTF-8 passes through, every
   other byte (and NUL) becomes '?'.  Returns the length written (out is
   NUL-terminated; cap must be >= 1). */
size_t rescue_sanitize_text(const uint8_t *in, size_t len, char *out, size_t cap);
/* serial_probe's transcript classes: silent, circuitpython_safe_mode,
   micropython_repl, circuitpython_repl, boot_banner_detected, other. */
const char *rescue_classify_transcript(const char *text);

#endif
