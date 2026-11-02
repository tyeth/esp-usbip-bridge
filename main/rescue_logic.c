#include "rescue_logic.h"

#include <ctype.h>
#include <string.h>

/* Descriptor types and classes */
#define DESC_INTERFACE  0x04
#define DESC_ENDPOINT   0x05
#define CLASS_CDC       0x02
#define CDC_SUBCLASS_ACM 0x02
#define CLASS_CDC_DATA  0x0A
#define CLASS_VENDOR    0xFF

/* bmRequestType */
#define RT_CLASS_IF_OUT  0x21
#define RT_VENDOR_DEV_OUT 0x40
#define RT_VENDOR_IF_OUT 0x41

/* ===========================================================================
 *  Descriptors and classification
 * ======================================================================== */

int rescue_parse_config(const uint8_t *desc, size_t len, rescue_config_t *out)
{
    memset(out, 0, sizeof(*out));
    rescue_iface_t *cur = NULL;     /* the alt-0 interface the endpoints belong to */
    size_t off = 0;
    while (off + 2 <= len) {
        const uint8_t blen = desc[off];
        const uint8_t type = desc[off + 1];
        if (blen < 2 || off + blen > len) {
            break;
        }
        if (type == DESC_INTERFACE && blen >= 9) {
            cur = NULL;
            if (desc[off + 3] == 0 && out->num_ifaces < RESCUE_MAX_IFACES) {
                cur = &out->ifaces[out->num_ifaces++];
                cur->number = desc[off + 2];
                cur->num_endpoints = desc[off + 4];
                cur->cls = desc[off + 5];
                cur->subclass = desc[off + 6];
                cur->protocol = desc[off + 7];
            }
        } else if (type == DESC_ENDPOINT && blen >= 7 && cur != NULL) {
            const uint8_t addr = desc[off + 2];
            const uint8_t xfer = desc[off + 3] & 0x03;
            const uint16_t mps = (uint16_t)(desc[off + 4] | (desc[off + 5] << 8)) & 0x7ff;
            if (xfer == 0x02) {             /* bulk */
                if ((addr & 0x80) && cur->ep_in == 0) {
                    cur->ep_in = addr;
                    cur->mps_in = mps;
                } else if (!(addr & 0x80) && cur->ep_out == 0) {
                    cur->ep_out = addr;
                    cur->mps_out = mps;
                }
            } else if (xfer == 0x03 && (addr & 0x80) && cur->ep_intr_in == 0) {
                cur->ep_intr_in = addr;
            }
        }
        off += blen;
    }
    return out->num_ifaces;
}

static bool is_ch34x(uint16_t vid, uint16_t pid)
{
    return (vid == 0x1a86 && (pid == 0x7523 || pid == 0x5523 || pid == 0x7522)) ||
           (vid == 0x4348 && pid == 0x5523) || (vid == 0x9986 && pid == 0x7523) ||
           (vid == 0x2184 && pid == 0x0057);
}

static bool has_bulk_pair(const rescue_iface_t *it)
{
    return it->ep_in != 0 && it->ep_out != 0;
}

/* FTDI chip family from bcdDevice, as ftdi_determine_type() does it. */
static void ftdi_chip(uint16_t bcd, uint8_t ifnum, rescue_caps_t *c)
{
    uint16_t channel = (uint16_t)(1 + ifnum);   /* CHANNEL_A + interface number */
    bool hispeed = false, supported = true;
    switch (bcd) {
    case 0x200: /* FT232A (or FT232B without a serial number): 232AM divisor, not encoded here */
        supported = false;
        channel = 0;
        break;
    case 0x400: /* FT232B */
    case 0x600: /* FT232R */
        channel = 0;
        break;
    case 0x500: /* FT2232C */
    case 0x1000: /* FT-X */
        break;
    case 0x700: case 0x800: case 0x900:                 /* FT2232H, FT4232H, FT232H */
    case 0x2800: case 0x2900: case 0x3000: case 0x3100: /* FT2233HP .. FT4232HP */
    case 0x3200: case 0x3300: case 0x3600:              /* FT233HP, FT232HP, FT4232HA */
        hispeed = true;
        break;
    default:
        supported = false;
        if (bcd < 0x200) {
            channel = 0;                                /* SIO */
        }
        break;
    }
    c->ftdi_channel = channel;
    c->ftdi_hispeed = hispeed;
    c->ftdi_baud_supported = supported;
}

void rescue_classify(uint16_t vid, uint16_t pid, uint16_t bcd_device,
                     const rescue_config_t *cfg, int port, rescue_caps_t *out)
{
    memset(out, 0, sizeof(*out));
    out->serial_ctrl_if = -1;
    out->serial_data_if = -1;
    out->rp_reset_if = -1;
    out->picoboot_if = -1;
    out->esp_usb_jtag = (vid == 0x303a && pid == 0x1001);
    if (port < 0) {
        port = 0;
    }

    for (int i = 0; i < cfg->num_ifaces; i++) {
        const rescue_iface_t *it = &cfg->ifaces[i];
        if (it->cls == CLASS_VENDOR && it->subclass == 0x00 && it->protocol == 0x01 &&
                out->rp_reset_if < 0) {
            out->rp_reset_if = it->number;
        }
        if (vid == 0x2e8a && (pid == 0x0003 || pid == 0x000f) && it->cls == CLASS_VENDOR &&
                it->subclass == 0x00 && it->protocol == 0x00 && has_bulk_pair(it) &&
                out->picoboot_if < 0) {
            out->picoboot_if = it->number;
            out->picoboot_ep_in = it->ep_in;
            out->picoboot_ep_out = it->ep_out;
        }
    }
    if (vid == 0x2e8a) {
        out->rp_chip = (pid == 0x0003) ? RESCUE_CHIP_RP2040
                     : (pid == 0x000f) ? RESCUE_CHIP_RP2350 : RESCUE_CHIP_UNKNOWN;
    }

    /* Vendor UART bridges: each vendor interface with a bulk pair is a port. */
    rescue_serial_kind_t vendor = RESCUE_SERIAL_NONE;
    if (vid == 0x10c4) {
        vendor = RESCUE_SERIAL_CP210X;
    } else if (is_ch34x(vid, pid)) {
        vendor = RESCUE_SERIAL_CH34X;
    } else if (vid == 0x0403) {
        vendor = RESCUE_SERIAL_FTDI;
    }
    if (vendor != RESCUE_SERIAL_NONE) {
        int n = 0;
        for (int i = 0; i < cfg->num_ifaces; i++) {
            const rescue_iface_t *it = &cfg->ifaces[i];
            if (it->cls != CLASS_VENDOR || !has_bulk_pair(it)) {
                continue;
            }
            if (n == port) {
                out->serial = vendor;
                out->serial_ctrl_if = it->number;
                out->serial_data_if = it->number;
                out->ep_in = it->ep_in;
                out->ep_out = it->ep_out;
                out->mps_in = it->mps_in;
                out->mps_out = it->mps_out;
                if (vendor == RESCUE_SERIAL_FTDI) {
                    ftdi_chip(bcd_device, it->number, out);
                }
            }
            n++;
        }
        out->serial_ports = n;
        if (out->serial != RESCUE_SERIAL_NONE) {
            return;
        }
    }

    /* CDC-ACM: the port-th communication interface and the data interface
       that follows it (the usual IAD layout). */
    int n = 0;
    for (int i = 0; i < cfg->num_ifaces; i++) {
        const rescue_iface_t *it = &cfg->ifaces[i];
        if (it->cls != CLASS_CDC || it->subclass != CDC_SUBCLASS_ACM) {
            continue;
        }
        if (n == port) {
            for (int j = i + 1; j < cfg->num_ifaces; j++) {
                const rescue_iface_t *d = &cfg->ifaces[j];
                if (d->cls == CLASS_CDC_DATA && has_bulk_pair(d)) {
                    out->serial = RESCUE_SERIAL_CDC_ACM;
                    out->serial_ctrl_if = it->number;
                    out->serial_data_if = d->number;
                    out->ep_in = d->ep_in;
                    out->ep_out = d->ep_out;
                    out->mps_in = d->mps_in;
                    out->mps_out = d->mps_out;
                    break;
                }
            }
        }
        n++;
    }
    out->serial_ports = n;
}

const char *rescue_serial_kind_name(rescue_serial_kind_t kind)
{
    switch (kind) {
    case RESCUE_SERIAL_CDC_ACM: return "cdc_acm";
    case RESCUE_SERIAL_CP210X:  return "cp210x";
    case RESCUE_SERIAL_CH34X:   return "ch34x";
    case RESCUE_SERIAL_FTDI:    return "ftdi";
    default:                    return "none";
    }
}

/* ===========================================================================
 *  Line control and baud
 * ======================================================================== */

static rescue_ctrl_t ctrl(uint8_t bm, uint8_t req, uint16_t value, uint16_t index)
{
    rescue_ctrl_t c;
    memset(&c, 0, sizeof(c));
    c.bm = bm;
    c.req = req;
    c.value = value;
    c.index = index;
    return c;
}

int rescue_lines_requests(const rescue_caps_t *caps, bool dtr, bool rts, rescue_ctrl_t *out, int max)
{
    if (max < 1) {
        return 0;
    }
    const uint16_t bits = (uint16_t)((dtr ? 0x01 : 0) | (rts ? 0x02 : 0));
    const uint16_t ifn = (uint16_t)caps->serial_ctrl_if;
    switch (caps->serial) {
    case RESCUE_SERIAL_CDC_ACM:
        /* SET_CONTROL_LINE_STATE: D0 DTR, D1 RTS */
        out[0] = ctrl(RT_CLASS_IF_OUT, 0x22, bits, ifn);
        return 1;
    case RESCUE_SERIAL_CP210X:
        /* SET_MHS with both write-enable masks (CONTROL_WRITE_DTR|RTS) */
        out[0] = ctrl(RT_VENDOR_IF_OUT, 0x07, (uint16_t)(0x0300 | bits), ifn);
        return 1;
    case RESCUE_SERIAL_CH34X: {
        /* ch341_set_handshake(): MODEM_CTRL with the active-low bits, ~control as a u16 */
        const unsigned control = (dtr ? 0x20u : 0) | (rts ? 0x40u : 0);
        out[0] = ctrl(RT_VENDOR_DEV_OUT, RESCUE_CH341_REQ_MODEM_CTRL, (uint16_t)~control, 0);
        return 1;
    }
    case RESCUE_SERIAL_FTDI:
        /* MODEM_CTRL with both masks: 0x0101 DTR high, 0x0202 RTS high */
        out[0] = ctrl(RT_VENDOR_DEV_OUT, 0x01, (uint16_t)(0x0300 | bits), caps->ftdi_channel);
        return 1;
    default:
        return 0;
    }
}

bool rescue_cp210x_enable_request(const rescue_caps_t *caps, rescue_ctrl_t *out)
{
    if (caps->serial != RESCUE_SERIAL_CP210X) {
        return false;
    }
    *out = ctrl(RT_VENDOR_IF_OUT, 0x00, 0x0001, (uint16_t)caps->serial_ctrl_if);   /* IFC_ENABLE, UART_ENABLE */
    return true;
}

void rescue_cdc_line_coding(uint32_t baud, uint8_t out[7])
{
    out[0] = (uint8_t)baud;
    out[1] = (uint8_t)(baud >> 8);
    out[2] = (uint8_t)(baud >> 16);
    out[3] = (uint8_t)(baud >> 24);
    out[4] = 0;     /* 1 stop bit */
    out[5] = 0;     /* no parity */
    out[6] = 8;     /* 8 data bits */
}

/* ch341_get_divisor() from drivers/usb/serial/ch341.c */
#define CH341_CLKRATE 48000000u
#define CH341_CLK_DIV(ps, fact) (1u << (12 - 3 * (ps) - (fact)))
#define CH341_MIN_RATE(ps) (CH341_CLKRATE / (CH341_CLK_DIV((ps), 1) * 512))
#define CH341_MIN_BPS ((CH341_CLKRATE + CH341_CLK_DIV(0, 0) * 256 - 1) / (CH341_CLK_DIV(0, 0) * 256))
#define CH341_MAX_BPS (CH341_CLKRATE / (CH341_CLK_DIV(3, 0) * 2))

int rescue_ch341_divisor(uint32_t speed, bool limited_prescaler)
{
    static const uint32_t min_rates[] = {
        CH341_MIN_RATE(0), CH341_MIN_RATE(1), CH341_MIN_RATE(2), CH341_MIN_RATE(3),
    };
    if (speed < CH341_MIN_BPS) {
        speed = CH341_MIN_BPS;
    } else if (speed > CH341_MAX_BPS) {
        speed = CH341_MAX_BPS;
    }
    uint32_t fact = 1;
    int ps;
    for (ps = 3; ps >= 0; ps--) {
        if (speed > min_rates[ps]) {
            break;
        }
    }
    if (ps < 0) {
        return -1;
    }
    uint32_t clk_div = CH341_CLK_DIV(ps, fact);
    uint32_t div = CH341_CLKRATE / (clk_div * speed);
    const bool force_fact0 = ps < 3 && limited_prescaler;
    if (div < 9 || div > 255 || force_fact0) {
        div /= 2;
        clk_div *= 2;
        fact = 0;
    }
    if (div < 2) {
        return -1;
    }
    if (16 * CH341_CLKRATE / (clk_div * div) - 16 * speed >=
            16 * speed - 16 * CH341_CLKRATE / (clk_div * (div + 1))) {
        div++;
    }
    if (fact == 1 && div % 2 == 0) {
        div /= 2;
        fact = 0;
    }
    return (int)((0x100 - div) << 8 | fact << 2 | (uint32_t)ps);
}

/* ftdi_232bm_baud_base_to_divisor() / ftdi_2232h_baud_base_to_divisor()
   from drivers/usb/serial/ftdi_sio.c */
static const uint8_t k_ftdi_divfrac[8] = { 0, 3, 2, 4, 1, 5, 6, 7 };

uint32_t rescue_ftdi_232bm_divisor(uint32_t baud)
{
    const uint64_t base = 48000000u;
    const uint32_t divisor3 = (uint32_t)((base + baud) / (2ull * baud));   /* DIV_ROUND_CLOSEST(base, 2 * baud) */
    uint32_t divisor = divisor3 >> 3;
    divisor |= (uint32_t)k_ftdi_divfrac[divisor3 & 0x7] << 14;
    if (divisor == 1) {
        divisor = 0;
    } else if (divisor == 0x4001) {
        divisor = 1;
    }
    return divisor;
}

uint32_t rescue_ftdi_2232h_divisor(uint32_t baud)
{
    const uint64_t num = 8ull * 120000000u, den = 10ull * baud;
    const uint32_t divisor3 = (uint32_t)((num + den / 2) / den);          /* DIV_ROUND_CLOSEST(8 * base, 10 * baud) */
    uint32_t divisor = divisor3 >> 3;
    divisor |= (uint32_t)k_ftdi_divfrac[divisor3 & 0x7] << 14;
    if (divisor == 1) {
        divisor = 0;
    } else if (divisor == 0x4001) {
        divisor = 1;
    }
    return divisor | 0x00020000;
}

int rescue_baud_requests(const rescue_caps_t *caps, uint32_t baud,
                         uint8_t ch34x_version, bool ch34x_limited,
                         rescue_ctrl_t *out, int max)
{
    if (baud == 0 || max < 4) {
        return -1;
    }
    const uint16_t ifn = (uint16_t)caps->serial_ctrl_if;
    switch (caps->serial) {
    case RESCUE_SERIAL_CDC_ACM:
        out[0] = ctrl(RT_CLASS_IF_OUT, 0x20, 0, ifn);      /* SET_LINE_CODING */
        out[0].len = 7;
        rescue_cdc_line_coding(baud, out[0].data);
        return 1;
    case RESCUE_SERIAL_CP210X:
        rescue_cp210x_enable_request(caps, &out[0]);
        out[1] = ctrl(RT_VENDOR_IF_OUT, 0x03, 0x0800, ifn);  /* SET_LINE_CTL: 8 data bits, no parity, 1 stop */
        out[2] = ctrl(RT_VENDOR_IF_OUT, 0x1E, 0, ifn);       /* SET_BAUDRATE, u32 LE */
        out[2].len = 4;
        out[2].data[0] = (uint8_t)baud;
        out[2].data[1] = (uint8_t)(baud >> 8);
        out[2].data[2] = (uint8_t)(baud >> 16);
        out[2].data[3] = (uint8_t)(baud >> 24);
        return 3;
    case RESCUE_SERIAL_CH34X: {
        int val = rescue_ch341_divisor(baud, ch34x_limited);
        if (val < 0) {
            return -1;
        }
        if (ch34x_version > 0x27) {
            val |= 0x80;    /* do not buffer until a full 32-byte packet */
        }
        out[0] = ctrl(RT_VENDOR_DEV_OUT, RESCUE_CH341_REQ_WRITE_REG, 0x1312, (uint16_t)val); /* DIVISOR<<8 | PRESCALER */
        if (ch34x_version < 0x30) {
            return 1;
        }
        out[1] = ctrl(RT_VENDOR_DEV_OUT, RESCUE_CH341_REQ_WRITE_REG, 0x2518, 0x00C3);      /* LCR2<<8 | LCR: RX|TX|CS8 */
        return 2;
    }
    case RESCUE_SERIAL_FTDI: {
        if (!caps->ftdi_baud_supported) {
            return -1;
        }
        uint32_t div;
        if (caps->ftdi_hispeed) {
            if (baud > 12000000) {
                return -1;
            }
            div = baud >= 1200 ? rescue_ftdi_2232h_divisor(baud) : rescue_ftdi_232bm_divisor(baud);
        } else {
            if (baud > 3000000) {
                return -1;
            }
            div = rescue_ftdi_232bm_divisor(baud);
        }
        const uint16_t ch = caps->ftdi_channel;
        uint16_t index = (uint16_t)(div >> 16);
        if (ch) {
            index = (uint16_t)((index << 8) | ch);
        }
        out[0] = ctrl(RT_VENDOR_DEV_OUT, 0x03, (uint16_t)div, index);   /* SET_BAUD_RATE */
        out[1] = ctrl(RT_VENDOR_DEV_OUT, 0x04, 0x0008, ch);              /* SET_DATA: 8N1 */
        out[2] = ctrl(RT_VENDOR_DEV_OUT, 0x02, 0x0000, ch);              /* SET_FLOW_CTRL: none */
        return 3;
    }
    default:
        return -1;
    }
}

/* ===========================================================================
 *  Reset sequences
 * ======================================================================== */

/* esptool UnixTightReset (atomic DTR+RTS): IO0 low while EN is released. */
static const rescue_line_step_t k_esp_bootloader[] = {
    { false, false, 0 },
    { true,  true,  0 },
    { false, true,  100 },  /* IO0=HIGH, EN=LOW: chip in reset (enter_boot_delay) */
    { true,  false, 50 },   /* IO0=LOW, EN=HIGH: out of reset into download mode (reset_delay) */
    { false, false, 0 },    /* IO0=HIGH, done */
};
/* esptool HardReset over a UART bridge: EN pulse with IO0 high. */
static const rescue_line_step_t k_esp_hard_reset[] = {
    { false, true,  100 },
    { false, false, 0 },
};
/* esptool USBJTAGSerialReset: each pyserial write sends both lines, so the
   distinct states are idle, IO0, (1,1) on the way, reset, release. */
static const rescue_line_step_t k_usb_jtag_bootloader[] = {
    { false, false, 100 },  /* idle */
    { true,  false, 100 },  /* set IO0 */
    { true,  true,  0 },    /* traverse (1,1), not (0,0) */
    { false, true,  100 },  /* reset */
    { false, false, 0 },    /* chip out of reset */
};
/* esptool HardReset(uses_usb=True): hold 200 ms, settle 200 ms. */
static const rescue_line_step_t k_usb_jtag_reset[] = {
    { false, true,  200 },
    { false, false, 200 },
};

#define SEQ(n, d, s, c) { n, d, s, (int)(sizeof(s) / sizeof(s[0])), c }

static const rescue_line_seq_t k_seqs[RESCUE_MODE_COUNT] = {
    [RESCUE_MODE_ESP_BOOTLOADER] = SEQ("esp_bootloader",
        "ESP classic auto-reset circuit (DTR->IO0, RTS->EN): reset into the ROM download mode",
        k_esp_bootloader, -1),
    [RESCUE_MODE_ESP_HARD_RESET] = SEQ("esp_hard_reset",
        "ESP classic auto-reset circuit: pulse EN to restart the app", k_esp_hard_reset, -1),
    [RESCUE_MODE_ESP_USB_JTAG_BOOTLOADER] = SEQ("esp_usb_jtag_bootloader",
        "ESP USB-Serial/JTAG (303a:1001): reset into the ROM download mode", k_usb_jtag_bootloader, 3),
    [RESCUE_MODE_ESP_USB_JTAG_RESET] = SEQ("esp_usb_jtag_reset",
        "ESP USB-Serial/JTAG (303a:1001): hard reset into the app", k_usb_jtag_reset, 0),
};

static const char *const k_mode_names[RESCUE_MODE_COUNT + 1] = {
    [RESCUE_MODE_ESP_BOOTLOADER] = "esp_bootloader",
    [RESCUE_MODE_ESP_HARD_RESET] = "esp_hard_reset",
    [RESCUE_MODE_ESP_USB_JTAG_BOOTLOADER] = "esp_usb_jtag_bootloader",
    [RESCUE_MODE_ESP_USB_JTAG_RESET] = "esp_usb_jtag_reset",
    [RESCUE_MODE_TOUCH_1200] = "touch_1200",
    [RESCUE_MODE_RP_BOOTSEL] = "rp_bootsel",
    [RESCUE_MODE_RP_REBOOT] = "rp_reboot",
    [RESCUE_MODE_PICOBOOT_REBOOT] = "picoboot_reboot",
    [RESCUE_MODE_PICOBOOT_BOOTSEL] = "picoboot_bootsel",
    [RESCUE_MODE_AUTO_BOOTLOADER] = "bootloader",
};

static const char *const k_mode_desc[RESCUE_MODE_COUNT + 1] = {
    [RESCUE_MODE_TOUCH_1200] = "CDC 1200-baud touch (line coding 1200, drop DTR): UF2 / SAM-BA / Arduino bootloader entry",
    [RESCUE_MODE_RP_BOOTSEL] = "RP2040/RP2350 reset interface: reboot into BOOTSEL (USB mass storage + PICOBOOT)",
    [RESCUE_MODE_RP_REBOOT] = "RP2040/RP2350 reset interface: reboot to the application in flash",
    [RESCUE_MODE_PICOBOOT_REBOOT] = "PICOBOOT (device in BOOTSEL): reboot to the application in flash",
    [RESCUE_MODE_PICOBOOT_BOOTSEL] = "PICOBOOT on RP2350: reboot into BOOTSEL again",
    [RESCUE_MODE_AUTO_BOOTLOADER] = "Pick the bootloader entry that fits this device",
};

const char *rescue_mode_name(rescue_mode_t mode)
{
    return (mode <= RESCUE_MODE_AUTO_BOOTLOADER) ? k_mode_names[mode] : "invalid";
}

const char *rescue_mode_description(rescue_mode_t mode)
{
    if (mode < RESCUE_MODE_COUNT && k_seqs[mode].name != NULL) {
        return k_seqs[mode].description;
    }
    return (mode <= RESCUE_MODE_AUTO_BOOTLOADER && k_mode_desc[mode]) ? k_mode_desc[mode] : "";
}

rescue_mode_t rescue_mode_from_name(const char *name)
{
    if (name == NULL) {
        return RESCUE_MODE_INVALID;
    }
    for (int m = 0; m <= RESCUE_MODE_AUTO_BOOTLOADER; m++) {
        if (k_mode_names[m] && strcmp(k_mode_names[m], name) == 0) {
            return (rescue_mode_t)m;
        }
    }
    return RESCUE_MODE_INVALID;
}

const rescue_line_seq_t *rescue_mode_line_seq(rescue_mode_t mode)
{
    if (mode < RESCUE_MODE_COUNT && k_seqs[mode].name != NULL) {
        return &k_seqs[mode];
    }
    return NULL;
}

bool rescue_mode_applicable(const rescue_caps_t *caps, rescue_mode_t mode)
{
    const bool serial = caps->serial != RESCUE_SERIAL_NONE;
    switch (mode) {
    case RESCUE_MODE_ESP_BOOTLOADER:
    case RESCUE_MODE_ESP_HARD_RESET:
        return serial && !caps->esp_usb_jtag;
    case RESCUE_MODE_ESP_USB_JTAG_BOOTLOADER:
    case RESCUE_MODE_ESP_USB_JTAG_RESET:
        return serial && caps->esp_usb_jtag;
    case RESCUE_MODE_TOUCH_1200:
        return caps->serial == RESCUE_SERIAL_CDC_ACM && !caps->esp_usb_jtag;
    case RESCUE_MODE_RP_BOOTSEL:
    case RESCUE_MODE_RP_REBOOT:
        return caps->rp_reset_if >= 0;
    case RESCUE_MODE_PICOBOOT_REBOOT:
        return caps->picoboot_if >= 0 && caps->rp_chip != RESCUE_CHIP_UNKNOWN;
    case RESCUE_MODE_PICOBOOT_BOOTSEL:
        return caps->picoboot_if >= 0 && caps->rp_chip == RESCUE_CHIP_RP2350;
    default:
        return false;
    }
}

rescue_mode_t rescue_auto_bootloader(const rescue_caps_t *caps, uint16_t vid)
{
    if (caps->esp_usb_jtag && caps->serial != RESCUE_SERIAL_NONE) {
        return RESCUE_MODE_ESP_USB_JTAG_BOOTLOADER;
    }
    if (caps->rp_reset_if >= 0) {
        return RESCUE_MODE_RP_BOOTSEL;
    }
    switch (caps->serial) {
    case RESCUE_SERIAL_CP210X:
    case RESCUE_SERIAL_CH34X:
    case RESCUE_SERIAL_FTDI:
        return RESCUE_MODE_ESP_BOOTLOADER;
    case RESCUE_SERIAL_CDC_ACM:
        /* WCH CDC bridges (CH9102, CH343) sit in front of ESP boards; any
           other CDC device is a native-USB board with a 1200-baud bootloader. */
        return vid == 0x1a86 ? RESCUE_MODE_ESP_BOOTLOADER : RESCUE_MODE_TOUCH_1200;
    default:
        return RESCUE_MODE_INVALID;
    }
}

/* ===========================================================================
 *  RP2040 / RP2350
 * ======================================================================== */

void rescue_rp_reset_request(int iface, bool to_bootsel, rescue_ctrl_t *out)
{
    /* RESET_REQUEST_BOOTSEL (wValue 0: no activity LED, no interface
       disabled) or RESET_REQUEST_FLASH, CLASS|INTERFACE as picotool sends it. */
    *out = ctrl(RT_CLASS_IF_OUT, to_bootsel ? 0x01 : 0x02, 0, (uint16_t)iface);
}

void rescue_picoboot_if_reset_request(int iface, rescue_ctrl_t *out)
{
    *out = ctrl(RT_VENDOR_IF_OUT, 0x41, 0, (uint16_t)iface);    /* PICOBOOT_IF_RESET */
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void rescue_picoboot_reboot_cmd(rescue_rp_chip_t chip, bool to_bootsel, uint32_t token,
                                uint32_t delay_ms, uint8_t out[32])
{
    memset(out, 0, 32);
    put_le32(&out[0], 0x431fd10bu);         /* PICOBOOT_MAGIC */
    put_le32(&out[4], token);
    if (chip == RESCUE_CHIP_RP2350) {
        out[8] = 0x0a;                      /* PC_REBOOT2 */
        out[9] = 16;
        put_le32(&out[16], to_bootsel ? 0x2u : 0x0u);   /* REBOOT2_FLAG_REBOOT_TYPE_BOOTSEL / NORMAL */
        put_le32(&out[20], delay_ms);
    } else {
        out[8] = 0x02;                      /* PC_REBOOT (RP2040) */
        out[9] = 12;
        put_le32(&out[16], 0);              /* dPC 0: the regular boot path */
        put_le32(&out[20], 0x20042000u);    /* dSP: SRAM_END, as picotool sends it */
        put_le32(&out[24], delay_ms);
    }
    /* dTransferLength (out[12..15]) stays 0: no data phase */
}

/* ===========================================================================
 *  Console helpers
 * ======================================================================== */

int rescue_named_poke(const char *name, uint8_t *out, size_t cap)
{
    static const struct { const char *name; const char *bytes; } k_pokes[] = {
        { "enter",  "\r\n" },
        { "ctrl_c", "\x03" },
        { "ctrl_d", "\x04" },
    };
    for (size_t i = 0; i < sizeof(k_pokes) / sizeof(k_pokes[0]); i++) {
        if (strcmp(name, k_pokes[i].name) == 0) {
            const size_t n = strlen(k_pokes[i].bytes);
            if (n > cap) {
                return -1;
            }
            memcpy(out, k_pokes[i].bytes, n);
            return (int)n;
        }
    }
    return -1;
}

int rescue_hex_decode(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = 0;
    int hi = -1;
    for (const char *p = hex; *p; p++) {
        if (*p == ' ' || *p == '_') {
            continue;
        }
        if (!isxdigit((unsigned char)*p)) {
            return -1;
        }
        const int v = isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10);
        if (hi < 0) {
            hi = v;
        } else {
            if (n >= cap) {
                return -1;
            }
            out[n++] = (uint8_t)(hi << 4 | v);
            hi = -1;
        }
    }
    return hi < 0 ? (int)n : -1;
}

size_t rescue_ftdi_strip_status(uint8_t *buf, size_t len, uint16_t mps)
{
    if (mps < 3) {
        return 0;
    }
    size_t w = 0;
    for (size_t off = 0; off < len; off += mps) {
        const size_t chunk = (len - off < mps) ? len - off : mps;
        if (chunk > 2) {
            memmove(&buf[w], &buf[off + 2], chunk - 2);
            w += chunk - 2;
        }
    }
    return w;
}

/* Length of a valid UTF-8 sequence at p (max n bytes), 0 if invalid. */
static size_t utf8_seq_len(const uint8_t *p, size_t n)
{
    const uint8_t b = p[0];
    size_t len;
    uint8_t lo = 0x80, hi = 0xBF;
    if (b >= 0xC2 && b <= 0xDF) {
        len = 2;
    } else if (b >= 0xE0 && b <= 0xEF) {
        len = 3;
        if (b == 0xE0) lo = 0xA0;
        if (b == 0xED) hi = 0x9F;           /* no UTF-16 surrogates */
    } else if (b >= 0xF0 && b <= 0xF4) {
        len = 4;
        if (b == 0xF0) lo = 0x90;
        if (b == 0xF4) hi = 0x8F;
    } else {
        return 0;
    }
    if (len > n || p[1] < lo || p[1] > hi) {
        return 0;
    }
    for (size_t i = 2; i < len; i++) {
        if (p[i] < 0x80 || p[i] > 0xBF) {
            return 0;
        }
    }
    return len;
}

size_t rescue_sanitize_text(const uint8_t *in, size_t len, char *out, size_t cap)
{
    size_t o = 0;
    size_t i = 0;
    while (i < len && o + 1 < cap) {
        const uint8_t b = in[i];
        if (b != 0 && b < 0x80) {
            out[o++] = (char)b;
            i++;
            continue;
        }
        const size_t n = (b == 0) ? 0 : utf8_seq_len(&in[i], len - i);
        if (n == 0) {
            out[o++] = '?';
            i++;
        } else if (o + n < cap) {
            memcpy(&out[o], &in[i], n);
            o += n;
            i += n;
        } else {
            break;
        }
    }
    out[o] = '\0';
    return o;
}

static bool contains_ci(const char *hay, const char *needle)
{
    const size_t n = strlen(needle);
    for (const char *h = hay; *h; h++) {
        size_t k = 0;
        while (k < n && h[k] && tolower((unsigned char)h[k]) == tolower((unsigned char)needle[k])) {
            k++;
        }
        if (k == n) {
            return true;
        }
    }
    return false;
}

const char *rescue_classify_transcript(const char *text)
{
    /* Mirrors classify_probe_transcript() in the controller's bench_stages.py. */
    static const char *const k_boot_needles[] = {
        "rst:", "boot:", "ESP-ROM", "configsip:", "entry ", "Boot ", "CIRCUITPY",
        "CircuitPython", "MicroPython", "WipperSnapper", "invalid header",
    };
    bool blank = true;
    for (const char *p = text; *p; p++) {
        if (!isspace((unsigned char)*p)) {
            blank = false;
            break;
        }
    }
    if (blank) {
        return "silent";
    }
    if (contains_ci(text, "safe mode")) {
        return "circuitpython_safe_mode";
    }
    if (strstr(text, ">>>") != NULL) {
        if (contains_ci(text, "micropython") && !contains_ci(text, "circuitpython")) {
            return "micropython_repl";
        }
        return "circuitpython_repl";
    }
    for (size_t i = 0; i < sizeof(k_boot_needles) / sizeof(k_boot_needles[0]); i++) {
        if (strstr(text, k_boot_needles[i]) != NULL) {
            return "boot_banner_detected";
        }
    }
    return "other";
}
