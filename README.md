# ESP32-P4 USB/IP Bridge (ESP-IDF)

This project is ESP-IDF firmware for ESP32-P4 / ESP32-S3 / ESP32-S31 USB dev boards that:

- runs USB Host mode,
- enumerates and exports multiple non-hub USB devices (including devices behind a USB hub),
- exposes it over IP using the Linux USB/IP protocol on TCP port `3240`.

## Status

Implemented:

- `OP_REQ_DEVLIST` / `OP_REQ_IMPORT` (`usbip list|attach -r <ip> -b <busid>`),
  multiple devices including behind cascaded hubs (busid = stable Linux-style
  port path)
- `USBIP_CMD_SUBMIT` for control, bulk and interrupt endpoints, with
  concurrent URBs per device and on-demand interface claiming (interfaces are
  released when a client's session ends)
- `USBIP_CMD_UNLINK` with safe cancellation: a stuck transfer is cancelled and
  its host transfer held until the controller retires it
- URB status codes translated to Linux errno values; TCP keepalive reaps a
  client that vanishes without a FIN and releases its claims
- Hub port power control (per-port, saved desired state, restore), enumeration
  diagnostics, and interface-claim / host-channel accounting via the HTTP
  controller API (`GET /api/schema` lists every endpoint)

Not implemented / known limits:

- isochronous endpoints
- the DWC OTG host has 16 channels shared by every open pipe (EP0s, hub
  interrupt pipes, claimed endpoints); `GET /api/usb/claims` reports the live
  authoritative ledger and attributes exhaustion to the claiming busid

## Protocol Basis

USB/IP wire format is implemented from Linux kernel documentation:

- https://docs.kernel.org/usb/usbip_protocol.html
- https://docs.kernel.org/usb/usbip_protocol.html#architecture

## Project Layout

- `main/main.c`: application startup
- `main/network_init.c`: target-specific network bring-up (Ethernet or Wi-Fi STA)
- `main/usb_backend.c`: USB Host backend, device discovery, EP0 control transfers
- `main/usbip_server.c`: TCP server and USB/IP message handling
- `main/usbip_protocol.h`: protocol constants and packet structs
- `main/Kconfig.projbuild`: board/server configuration options

## Cloning

The `esp-harness` submodule declares its own `esp-idf`, which the build never
reads. Initialise selectively rather
than with `--recursive` to avoid downloading a second full ESP-IDF:

```bash
git clone https://github.com/adafruit/esp-usbip-bridge.git
cd esp-usbip-bridge
git submodule update --init
git -C esp-harness submodule update --init components/scpi_parser/upstream
```

## ESP-IDF Setup

This repository pins ESP-IDF to a commit on upstream `espressif/esp-idf` `master`, because no release tag supports the ESP32-S31 yet (it is still a preview target, so `scripts/build-board.sh` passes `--preview` for it). FS-only USB host mode (`USB_DWC_FSLS_ONLY`) is provided by the `adafruit/esp-usb` fork referenced from `main/idf_component.yml`, so ESP-IDF itself needs no patches.

1. Clone/install ESP-IDF locally for this project:

```bash
./scripts/setup-esp-idf.sh
```

2. In every new shell, load the project-local ESP-IDF environment:

```bash
source ./scripts/idf-env.sh
```

## Build and Flash

Five board profiles are provided:

- `p4-function-ev`: ESP32-P4-Function-EV board, USB/IP over Ethernet
- `m5stack-poe-p4`: M5Stack PoE ESP32-P4, USB/IP over Ethernet
- `p4hil`: [P4HIL](https://github.com/tannewt/p4hil) hardware-in-the-loop
  fixture, USB/IP over Ethernet
- `s3-usb-otg`: ESP32-S3-USB-OTG board, USB/IP over Wi-Fi STA
- `s31-function-coreboard-1`: ESP32-S31-Function-CoreBoard-1, USB/IP over
  gigabit (RGMII) Ethernet, USB host on the USB-HS port

Set your serial port once (example):

```bash
export ESPPORT=/dev/ttyUSB0
```

Console output defaults by target:
- ESP32-P4 boards: USB Serial/JTAG console
- `s3-usb-otg`: `UART0` console (to avoid USB host conflicts)
- `s31-function-coreboard-1`: USB Serial/JTAG console

Once running, a board announces itself over mDNS, so you do not need to know its
address in advance:

```bash
avahi-browse -rt _usbip._tcp
```

### ESP32-P4-Function-EV

Build:

```bash
./scripts/build-board.sh p4-function-ev build
```

Flash and monitor:

```bash
ESPPORT=$ESPPORT ./scripts/build-board.sh p4-function-ev flash
ESPPORT=$ESPPORT ./scripts/build-board.sh p4-function-ev monitor
```

### ESP32-S31-Function-CoreBoard-1

The ESP32-S31 has a single USB OTG controller with a high-speed (UTMI) PHY and
no separate full-speed PHY. Like the P4 boards it is run in FS-only mode, so a
hub on the USB-HS port negotiates at full speed and FS/LS devices behind it work
without split transactions.

Build:

```bash
./scripts/build-board.sh s31-function-coreboard-1 build
```

Flash and monitor:

```bash
ESPPORT=$ESPPORT ./scripts/build-board.sh s31-function-coreboard-1 flash
ESPPORT=$ESPPORT ./scripts/build-board.sh s31-function-coreboard-1 monitor
```

The S31 build uses its own partition table (`partitions-s31-function-coreboard-1.csv`):
a 4 MB factory app at `0x10000` (the device rescue tools need more than the
default 1 MB), two 4 MB OTA slots (`ota_0` at `0x410000`, `ota_1` at
`0x810000`) and `otadata` at `0xC10000`, with `nvs` and `phy_init` at the
default offsets. NVS (hostname, API token, port power states, mux topology,
capture settings) is kept across every table change. Once a bridge has this
table it is updated over the network (see
[Firmware updates over the network](#firmware-updates-over-the-network-ota));
moving a bridge to it is one wired flash, described there.

`components/esp-serial-flasher` is a vendored copy of Espressif's host-side
flasher (see its `VENDORED.md` for why the registry package is not used).

### ESP32-S3-USB-OTG

Before building, edit `sdkconfig.defaults.s3-usb-otg`:

- `CONFIG_USBIP_WIFI_SSID`: your AP SSID
- `CONFIG_USBIP_WIFI_PASSWORD`: your AP password
- `CONFIG_USBIP_WIFI_AUTHMODE`:
  - `2` = WPA2-PSK (recommended for most home/office APs)
  - `3` = WPA2/WPA3 transition mode
  - `0` = open (debug only)
- `CONFIG_USBIP_WIFI_DISABLE_MODEM_SLEEP=y` (recommended for USB host stability)

Build:

```bash
./scripts/build-board.sh s3-usb-otg build
```

Flash and monitor:

```bash
ESPPORT=$ESPPORT ./scripts/build-board.sh s3-usb-otg flash
ESPPORT=$ESPPORT ./scripts/build-board.sh s3-usb-otg monitor
```

Clean board build output and board `sdkconfig` (forces regeneration from defaults on next build):

```bash
./scripts/build-board.sh s3-usb-otg clean
./scripts/build-board.sh p4-function-ev clean
```

If switching targets or recovering from config mismatch, erase flash:

```bash
idf.py -B build-s3-usb-otg -DIDF_TARGET=esp32s3 -DSDKCONFIG=sdkconfig.s3-usb-otg erase-flash
```

## Connect from Linux Host

Install/load USB/IP support on Linux host:

```bash
sudo modprobe vhci-hcd
```

Discover bridge by mDNS:

```bash
avahi-browse -rt _usbip._tcp
```

Resolve host (example):

```bash
avahi-resolve-host-name usbip-xxxxxx.local
```

List and attach exported device:

```bash
usbip list -r <bridge-ip-or-hostname>
sudo usbip attach -r <bridge-ip-or-hostname> -b 1-<devaddr>
```

Example bus ID format from this firmware: `1-1`.

## Controller API (HTTP, MCP, SCPI)

Besides USB/IP, the bridge serves a controller API on port 80 for people
(the web page), scripts and agents. `GET /api/schema` lists every endpoint.

**Bus IDs are Linux style port paths** (`1-1.3` = port 3 of the hub on the
root port), so a device keeps its busid when it re-enumerates, and
`usbip list -r` / `usbip attach -b` work like against a Linux host.
Virtual devices are on bus 2.

### USB devices and hub port power

| Method | Path | |
|---|---|---|
| GET | `/api/usb/devices` | VID/PID, manufacturer/product/serial strings, speed, MaxPower, interfaces, hub/port and port power state, `imported` (a USB/IP client has it) and `owner` |
| GET | `/api/usb/devices/{busid}` | one device, 404 when absent (presence check) |
| GET | `/api/usb/hubs` | hubs with their decoded `wHubCharacteristics` (power switching `per-port`/`ganged`/`none`, over-current mode, compound, TT think time, port indicators, PwrOn2PwrGood, controller current) and every port's state, saved state and mismatch |
| GET | `/api/ports`, `/api/ports/{port}` | flat port list (also at `/ports`) |
| POST | `/api/ports/{port}/on`, `/off` | SetPortFeature/ClearPortFeature PORT_POWER, body `{"force":bool}`. `202` with `"pending": true` when the port is busy (e.g. its device is enumerating): the request is applied in the background |
| POST | `/api/ports/{port}/cycle` | off, wait `off_ms` (default 1000), on; runs in the background |
| POST | `/api/ports/off`, `/api/ports/on` | every per-port switched port that does not lead to a hub |
| POST | `/api/ports/restore` | switch every port whose power differs from its saved state back to it |
| GET/POST | `/api/settings` | `{"enforce_per_port_switching": bool, "restore_port_power": bool, "enum_timeout_ms": int}` |
| POST | `/api/settings/enum_timeout` | `{"path": "1-1.3", "timeout_ms": int\|null}`: enumeration timeout override for a hub or port path and everything behind it (no path = global, `null` removes) |
| POST | `/api/usb/debug` | log the USB host's hub, port and enumeration state to the console |

Port power is refused (409) unless `force` is set when the hub does not report
per-port power switching (ganged hubs switch all ports or none, and many cheap
hubs have no switches at all) while `enforce_per_port_switching` is on (the
default), and when powering off a port that leads to another hub.

The last power state set for each port path is saved in NVS. With
`restore_port_power` on (the default), a port that was switched off stays off
when its hub re-enumerates: hub reset, upstream power loss or bridge reboot.
The USB host library keeps such ports unpowered from the start, so the DUT never
sees VBUS. With it off, ports come back powered and show `mismatch` until
`POST /api/ports/restore`. Both settings are toggles on the web page's
Hubs & Power tab.

Requests never wait for a busy port: requests for a port whose device is
enumerating are queued and applied in the background (each port on its own),
and power cycles of several ports run in parallel.

**Enumeration timeout.** Devices are enumerated one at a time, so a device that
never answers would stop every other device from enumerating. If an enumeration
control transfer does not complete within `enum_timeout_ms` (default 2000 ms,
0 = no timeout) the device's port is disabled and enumeration carries on; power
cycle the port to try that device again. USB 2.0 gives a device 500 ms for the
first data packet of a request and 50 ms for a status stage, so 1000 ms is the
shortest sensible global value; slow-booting boards that NAK while starting up
can get a longer per-port or per-hub override (Hubs & Power tab, or
`POST /api/settings/enum_timeout`).

### I2C strand mux

A C port of
[sbc-dut-analog-mux-api-circuitpy](https://github.com/Gundry-Consultancy/sbc-dut-analog-mux-api-circuitpy)
with the same API, so its existing clients (including the HIL controller's
`select_i2c_strand` stage) can point at the bridge: `GET /api/status`,
`GET /api/duts`, `POST /api/select {"dut"}`, `POST /api/isolate`,
`POST /api/groups/{group}/select/{channel}`, `POST /api/groups/{group}/channel`,
`GET|PUT /api/topology`, `GET /api/probe`. ADG729 (dual 4:1) and ADG728 pairs
(8:1 SDA + 8:1 SCL) are supported; selection is break-before-make across all
groups. The topology is stored in NVS. The control bus pins are
`CONFIG_USBIP_MUX_I2C_SDA_GPIO`/`SCL_GPIO` (GPIO2/GPIO3 on the
ESP32-S31-Function-CoreBoard-1, unset elsewhere). The bus needs external pull-ups
(about 4.7 kOhm); with only the internal ones `/api/probe` reports phantom
devices.

### Device rescue (interrogate, serial lines, reset, console peek)

Each bridge can interrogate and rescue a device on its own, with no host tools:
the web page's **Device Rescue** tab (or `http://<bridge>/#rescue=<busid>`, or
the **Rescue** button on a USB Devices row) and these endpoints, which the HIL
controller can call too. The bridge drives the device through its own USB host
(control and bulk transfers on the same backend path as USB/IP).

| Method | Path | |
|---|---|---|
| GET | `/api/usb/devices/{busid}/rescue` | identity, interfaces (numbers, classes, endpoints), `owner`/`imported`, the port's power state (**read-only**), the serial function (`cdc_acm`, `cp210x`, `ch34x`, `ftdi`, with interface numbers and endpoints), RP2 reset / PICOBOOT interfaces, the reset `modes` that apply, `bootloader_mode`, and the baud/DTR/RTS the bridge last set. No bus traffic |
| POST | `/api/usb/devices/{busid}/serial` | `{"baud": int?, "dtr": bool?, "rts": bool?, "port": int?}`: line coding (8N1) and/or DTR and RTS, both lines in one request. A line not given keeps what the bridge last set (deasserted if never). 1200 baud needs `"allow_1200": true` |
| POST | `/api/usb/devices/{busid}/reset` | `{"mode": str, "port": int?, "wait_ms": int?}`: run a reset sequence, then wait up to `wait_ms` (default 3000 when the device is expected to drop, max 10000) for it to re-enumerate and report `dropped` and `reenumerated` (VID:PID, product) |
| POST | `/api/usb/devices/{busid}/console` | `{"serial_baud": 115200, "pokes": ["enter", "ctrl_c", "ctrl_d", {"hex": "0d0a"}], "settle_s": 1, "capture_s": 3, "dtr": bool?, "rts": bool?, "max_bytes": 4096, "port": int?}`: set the baud, optionally DTR/RTS, send the pokes `settle_s` apart while reading, capture `capture_s` more. Returns `text` (UTF-8, invalid bytes as `?`), `data_hex`, `bytes`, `truncated` and the controller's `serial_probe` `classification` (`silent`, `circuitpython_repl`, `micropython_repl`, `circuitpython_safe_mode`, `boot_banner_detected`, `other`) |

Reset modes (`GET .../rescue` lists the ones that apply; the DTR/RTS ones can be
sent to any serial function):

| Mode | Device | What it does |
|---|---|---|
| `esp_bootloader` | ESP behind a UART bridge (CP210x, CH34x, FTDI, CH9102/CH343) with the DTR→IO0 / RTS→EN circuit | esptool's tight reset: (DTR,RTS) 00, 11, 01 for 100 ms, 10 for 50 ms, 00 — download mode |
| `esp_hard_reset` | same | RTS (EN) pulse for 100 ms with IO0 high |
| `esp_usb_jtag_bootloader` | ESP USB-Serial/JTAG `303a:1001` | esptool's USB-JTAG sequence: 00 100 ms, 10 100 ms, 11, 01 100 ms, 00 — download mode |
| `esp_usb_jtag_reset` | `303a:1001` | 01 for 200 ms, 00, 200 ms |
| `touch_1200` | CDC-ACM (RP2040/SAMD UF2, SAM-BA, Arduino native USB) | DTR+RTS on, line coding 1200 8N1, DTR+RTS off |
| `rp_bootsel` / `rp_reboot` | pico-sdk reset interface (FF/00/01) | `RESET_REQUEST_BOOTSEL` / `RESET_REQUEST_FLASH`, as picotool sends them |
| `picoboot_reboot` | RP2040/RP2350 in BOOTSEL (`2e8a:0003` / `000f`) | PICOBOOT interface reset, then `PC_REBOOT` (RP2040) or `PC_REBOOT2` normal (RP2350): back to the app |
| `picoboot_bootsel` | RP2350 in BOOTSEL | `PC_REBOOT2` into BOOTSEL again |
| `bootloader` | any | picks one: USB-Serial/JTAG → `esp_usb_jtag_bootloader`, reset interface → `rp_bootsel`, UART bridge or WCH CDC → `esp_bootloader`, other CDC → `touch_1200` |

The line requests follow the Linux drivers: CDC `SET_LINE_CODING` /
`SET_CONTROL_LINE_STATE`; CP210x `IFC_ENABLE`, `SET_LINE_CTL`, `SET_BAUDRATE`,
`SET_MHS` with both write masks; CH34x version read, quirk probe and
`SERIAL_INIT` once per enumeration, then the divisor/LCR registers and
`MODEM_CTRL` (active low); FTDI `SET_BAUD_RATE` (232BM divisor, or the 120 MHz
one on H-series), `SET_DATA`, `SET_FLOW_CTRL` and `MODEM_CTRL` with both masks
(the two status bytes of every IN packet are stripped). FT232A/SIO baud rates
are not supported. Multi-port bridges (CP2105, FT2232) and composite CDC
devices take `port` (0 = first).

Notes:

- **Never changes port power.** The port's power state is reported only; switch
  it on the Hubs & Power tab or with `/api/ports/{port}/...`.
- **USB/IP excludes rescue and back.** Every POST is refused (`409`) while a
  client has the device imported (`owner.kind == "usbip"`, with its address); an
  import is answered `ST_DEV_BUSY` while a rescue action runs. Detach first.
- **The console does not touch DTR/RTS unless asked** (some transitions reset an
  ESP USB-Serial/JTAG chip). TinyUSB CDC devices send nothing until DTR is on:
  pass `"dtr": true`. It never uses 1200 baud.
- **The HTTP server handles one request at a time**, so a console peek blocks
  the bridge API (not USB/IP) for up to 12 s: `capture_s` ≤ 10 and pokes ×
  `settle_s` + `capture_s` ≤ 12. Resets wait at most 10 s.
- After a peek the claimed data interface is released again, so its host
  channels go back to USB/IP. A read that fails because no host channel was free
  says so (see `/api/usb/claims`).
- Failures after a reset's commit step (the device leaving the bus) are expected
  and reported as `dropped`, not as errors. `requests` lists every control and
  bulk transfer sent with its status.
- The I2C strand-mux section stays as it was; the rescue panel links to it and
  offers **Route strand here** when a mux DUT has the device's friendly name.

### Flashing, capture and mass storage without host tools

The bridge can also program its devices, record their serial output and read
and write their USB drives itself, so a HIL run can flash, reset and watch a
DUT through the bridge alone, with no USB/IP client and no esptool / picotool /
bossac on a host. All of it is on the web page's Device Rescue tab and in the API.

**Staged files.** Firmware and files go into the bridge's PSRAM first:
`PUT /api/rescue/files/{name}` with the raw bytes as the body (streamed;
names `[A-Za-z0-9._-]`), `GET` to download, `DELETE` to remove,
`GET /api/rescue/files` for the list and the space left. Staged files and
capture buffers share one budget (12 MB cap, 1 MB of PSRAM always left free);
everything is lost on reboot.

```bash
curl -X PUT --data-binary @build/bootloader/bootloader.bin http://<bridge>/api/rescue/files/bootloader.bin
```

**Jobs.** Flashing and file copies run as background jobs (one at a time), so
the API stays responsive: the POST answers `202 {"job": id, "poll": "/api/rescue/jobs/<id>"}`,
and `GET /api/rescue/jobs/{id}` (or `latest`) gives `state` (queued, running,
done, failed), `progress`, `stage`, a timestamped `log`, `result` and `error`.
`DELETE /api/rescue/jobs/{id}` cancels. A job holds the device: USB/IP imports
and other rescue actions are refused until it ends. Every job takes
`"capture_after": {capture start args}` to start a serial capture when it lets
go of the device; the ESP flasher then defers its final hard reset until the
capture is reading, so the freshly flashed board's first boot is recorded.

| Method | Path (under `/api/usb/devices/{busid}/`) | |
|---|---|---|
| POST | `flash/esp` | esptool write-flash: `{"images": [{"file", "offset"}], "before": "default_reset"\|"usb_reset"\|"no_reset", "after": "hard_reset"\|"no_reset", "baud": 460800, "erase_all": false, "verify": true, "stub": true}`. Uses esp-serial-flasher with the flasher stub (ROM loader fallback): bootloader entry by the classic DTR/RTS circuit (CP210x, CH34x, FTDI, CH9102) or the USB-Serial/JTAG sequence (`303a:1001`, following its re-enumeration), esptool's watchdog disable over USB-Serial/JTAG, CHANGE_BAUDRATE on UART bridges, an MD5 check of every image against the flash, hard reset |
| POST | `esp/info` | connect and report chip, MAC, flash size and security info |
| POST | `flash/picoboot` | picotool load: `{"file": <UF2 or .bin>, "address": "0x10000000" (.bin), "verify": true, "reboot": true, "erase_all": false, "flash_size": "2MB"}`. Enters BOOTSEL first (reset interface, else the 1200-baud touch), keeps the untouched parts of partially written 4 KB sectors, verifies by read-back, checks the UF2 family against the chip (`force_family`) |
| POST | `picoboot/info` | enter BOOTSEL and report the chip |
| POST | `flash/samba` | bossac: `{"file": <.bin>, "offset": "0x2000" (default: 0x2000 SAMD21, 0x4000 SAMD51), "erase": true, "verify": true, "reset": true}` for SAM-BA bootloaders with the Arduino X/Y/Z extensions (Arduino and Adafruit SAMD bootloaders); 1200-baud touch first when the app is running; CRC16 verify per 4 KB |
| POST | `samba/info` | bootloader version, chip, flash geometry |
| POST | `flash/uf2` | copy a UF2 to the device's UF2 bootloader drive as a file, as a PC does (`{"file", "path": "/FIRMWARE.UF2", "method": "auto"}`). The MSC layer keeps the boot sector, FATs and root directory it writes in a write-through cache, as a PC's page cache does: a UF2 bootloader's FAT is synthetic and ignores writes, and FatFS re-reads FAT sectors. `method`: `auto` falls back to writing the blocks straight to sectors near the end of the volume (WRITE(10)) if the copy fails, `file` never does, `raw` only does that; an ESP32-S2/S3 app whose 1200-baud touch lands in the ROM loader (Arduino) gets TinyUF2 started for it: the bridge checks the partition table for a factory app (TinyUF2's `uf2`), erases otadata, clears the forced download boot and resets, as a double-tap of reset would; enters the bootloader first (RP2 reset interface or 1200-baud touch) when the device has no drive or its drive is an app's (no `INFO_UF2.TXT`: CIRCUITPY, WIPPER), and fails rather than copy onto an app's drive when `"enter_bootloader": false`; the bootloader rebooting after the last block counts as success |

Erasing (old firmware, NVS secrets, file systems): `erase_all` on `flash/esp`
erases the whole chip; on `flash/picoboot` it needs the board's `flash_size`
(it is not guessed: RP2040 cannot report it); SAM-BA's `erase` erases from the
offset to the end, keeping the bootloader; a UF2 copy cannot erase.

**Serial capture.** The bridge records its devices' serial output into PSRAM,
with timestamps, and serves it buffered or live:

| Method | Path | |
|---|---|---|
| POST | `/api/usb/devices/{busid}/capture/start` | `{"baud": 115200 \| [921600, "auto"] \| "auto", "size": 262144, "dtr", "rts", "arm": false}`: capture every serial function of the device (each CDC function of a composite device, each port of a CP2105/FT2232) into its own ring, through resets and re-enumeration on the same port path, including a new VID:PID or port count. `dtr` defaults on for native USB CDC (TinyUSB, Arduino, CircuitPython print nothing until a host asserts it) and leaves UART bridges and the ESP USB-Serial/JTAG alone (their DTR is a boot strap) |
| POST | `/api/usb/devices/{busid}/capture/stop` | `{"disarm": false, "clear": false}`; the buffer stays readable unless cleared |
| GET | `/api/usb/devices/{busid}/capture` | `?port=0&since=<cursor>` or `&from=epoch\|oldest\|tail`, `&max=`, `&format=lines` (prefixes `[uptime s.ms] `). Answers carry `next` (the next cursor), `epoch`, `epoch_cursor`, `serial_baud`, `uptime_ms` |
| POST | `/api/usb/devices/{busid}/capture/send` | `{"port": 0, "text": "..."}` or `{"hex": "03"}` |
| GET | `/api/rescue/capture/{busid}/{port}.log` | the whole buffer as a timestamped log file (`?from=epoch` for the current boot) |
| GET / POST | `/api/capture`, `/api/capture/config` | budget (`total_limit` for all captures, up to 12 MB; `default_size` per port; saved in NVS), armed ports, every capture with its ports and cursors |

- Each start, drop, return and baud change writes a `[bridge] ...` marker line
  and starts a new epoch; read a boot from its `epoch_cursor` (`from=epoch`).
- Timestamps are bridge uptime (no SNTP); every answer has `uptime_ms` to
  convert to wall-clock time.
- Baud: a fixed rate, rates tried in order, or `auto` (UART bridges only;
  native USB CDC ignores the rate). Candidates are scored on the text they
  read (at least 24 bytes) and the first good one is kept; a silent line gets
  the first candidate; output that turns to garbage (ROM vs app rate) is probed
  again. The order after the preferred rates is 115200, 921600, 460800, 230400,
  74880, 57600, 38400, 19200, 9600, 2 M, 1.5 M, 1 M.
- `arm: true` keeps capturing the port whenever a device is on it, across
  bridge reboots and after jobs. A running capture holds the device: USB/IP
  imports are refused; line and reset actions work meanwhile (to watch a boot);
  a console peek or a job stops it (an armed one restarts afterwards).
- Live streams: TCP 3241. Send one line, then read:
  `echo "1-1.2:0 from=epoch ts" | nc <bridge> 3241` (options `since=<cursor>`,
  `from=epoch|oldest`, `ts`, `token=<api token>`; the default is the tail).
  The bridge answers `OK <cursor>` and streams; what the client sends goes to
  the device (with the token when one is set). At most 4 streams.
- `"source": "gpio_uart"` (a bridge UART on GPIO pins, for DUTs without USB
  serial: `{uart, rx_gpio, tx_gpio, baud}`) is reserved in the API and answers
  501 until implemented.
- The older `monitor/start|stop|send` and `GET .../monitor` endpoints map onto
  the capture (port 0).

**Mass storage** (FAT over Bulk-Only Transport/SCSI, one volume at a time):
`GET .../msc` (inquiry, capacity, FAT type, free space, root listing, whether
it is a UF2 bootloader), `GET .../msc/list?path=/lib`, `POST .../msc/delete
{"path"}`, `.../msc/rename {"from", "to"}`, `.../msc/mkdir {"path"}`, and the jobs
`.../msc/write {"file": <staged>, "path"}` (create or replace) and `.../msc/read
{"path", "file": <staged name>}` (then download it from the staged files).

MCP tools: `rescue_start_job` (kind `esp_flash`, `esp_info`, `picoboot_load`,
`picoboot_info`, `samba_flash`, `samba_info`, `uf2_copy`, `msc_write`, `msc_read`),
`rescue_get_job`, `rescue_list_files`, `rescue_monitor`, `rescue_msc`.

Not supported yet: PICOBOOT flashing of RAM-only UF2s and the RP2350 partition
table / GET_INFO commands; SAM-BA without the Arduino extensions (genuine ROM
SAM-BA, SAM3/SAM4); the ESP ROM-USB-OTG CDC (`303a:0002`, ESP32-S2/S3 download
mode over native USB, which needs smaller RAM blocks for the stub upload);
ESP8266; GPIO UART capture.

### Firmware updates over the network (OTA)

S31 bridges with the OTA partition table take a new app image over HTTP:

```bash
curl -T build-s31-function-coreboard-1/esp32p4_usbip_bridge.bin \
     -H "Authorization: Bearer $TOKEN" "http://<bridge>/api/ota?reboot=1"
curl http://<bridge>/api/ota
```

| Method | Path | |
|---|---|---|
| GET | `/api/ota` | running / boot / next slot, each app slot's state (`valid`, `pending_verify`, `invalid`, `aborted`, `null` for factory) and image (`version`, `date`, `time`, `elf_sha256`), `fw_version` (git describe + build time of the running image), `last_invalid` (the last slot rolled back), the new image's `confirm` check and the `last_upload` since boot |
| PUT | `/api/ota` | the raw app `.bin` as the body (`Content-Length` required; POST also accepted). `?reboot=1` restarts into it after answering, `?force=1` accepts another project's app |
| POST | `/api/ota/boot` | `{"partition": "factory" \| "ota_0" \| "ota_1", "reboot": true}`: boot another slot next (its image is verified first) |

The upload is streamed straight into the next OTA slot (`ota_0` and `ota_1`
alternate; `factory` is never written over the network). Before anything is
written the image header is checked: an ESP image (`0xE9`), built for this
chip, an app (has an app description, so a `bootloader.bin` or
`partition-table.bin` is refused), the same project as the running app, and
no larger than the slot. After the last byte `esp_ota_end()` verifies the
whole image (checksum, SHA-256, chip revision) before the slot is made the
boot partition. Errors answer `{"ok": false, "error", "reason"}` with 400
(bad image), 413 (too big), 409 (the running image is still pending
verification), 501 (no OTA slots: the old table). The HTTP server serves
nothing else while it writes (about 1.1 MB), and USB/IP traffic can stall
briefly during flash writes. The web page header has a Firmware update row.

**Rollback.** A new image boots pending verification
(`CONFIG_BOOTLOADER_APP_ROLLBACK_CONFIRM_BY_APP`). It marks itself valid once
`app_main` has started every service, a network interface has an address and
its own HTTP server answers `GET /ping` over loopback (`confirm.state`
`confirmed`, usually a few seconds after boot). If that has not happened
`CONFIG_USBIP_OTA_CONFIRM_TIMEOUT_S` (90) seconds after boot, it marks itself
invalid and reboots into the previous image; a crash or reset before then
does the same through the bootloader. The previous image is the other OTA
slot, or factory after the first update. So a bridge that is power-cycled
with no Ethernet link right after an update goes back to the image it ran
before, by design. `last_invalid` names the slot that was rolled back.

The bootloader needs nothing new for this: with this ESP-IDF,
`CONFIG_BOOTLOADER_APP_ROLLBACK` (the bootloader side) is on by default and
already was in the single-factory builds; `..._CONFIRM_BY_APP` only changes
the app (it drops IDF's automatic confirmation before `app_main`).

**Moving a bridge to the OTA table: one wired flash.** The old table had a
single factory app at `0x10000`; the bootloader at `0x2000` and the table at
`0x8000` change, and `otadata` must start blank (the old table never owned
`0xC10000`, so whatever bytes are there could look like OTA data). Flash all
four from the build directory, e.g. through the bridge's USB-Serial/JTAG:

```bash
esptool --chip esp32s31 -p <port> -b 460800 --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x2000 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin \
  0xC10000 ota_data_initial.bin 0x10000 esp32p4_usbip_bridge.bin
```

(`idf.py flash` writes the same four.) The bridge then boots `factory`, the
image just written, keeps its NVS, and takes every later update with
`PUT /api/ota`. The factory image stays as the fallback.

**Only upload images that have OTA support themselves** (this code and
`CONFIG_BOOTLOADER_APP_ROLLBACK_CONFIRM_BY_APP`). An app built from an older
tree has the same project name, so the upload is accepted. But it confirms
itself at startup and has no `/api/ota`, so it cannot be replaced over the
network and its slot keeps booting. That takes a wired flash that erases
`otadata` (below).

**Wired app-only flashes after that.** Once a bridge boots an OTA slot, writing
only the app at `0x10000` changes `factory`, which it does not boot. Erase
`otadata` in the same esptool call (`erase-region 0xC10000 0x2000`, or write
`ota_data_initial.bin` at `0xC10000`), or `POST /api/ota/boot
{"partition": "factory"}` first.

### Auth

`POST /api/auth/token {"token": "..."}` sets a token (empty clears it). Once set,
every non-GET request (and `/api/select/*`) needs `Authorization: Bearer <token>`.
The web page has a token field.

### MCP

`POST /mcp` is a stateless Model Context Protocol endpoint (Streamable HTTP,
JSON responses) with tools `list_usb_devices`, `get_usb_device`, `list_hubs`,
`set_port_power`, `power_cycle_port`, `set_device_name`, `mux_status`,
`mux_select`, `mux_isolate`, `mux_set_channel`, `mux_get_topology`,
`mux_set_topology`, `get_port_power_settings`, `set_port_power_settings`,
`set_enum_timeout`, `restore_port_power`, `get_bridge_info`, `get_ota_status` and the device
rescue tools `rescue_device_info`, `rescue_serial_lines`, `rescue_reset_device`
and `rescue_console_peek`. For example with Claude Code:

```bash
claude mcp add --transport http usbip-bridge http://usbip-xxxxxx.local/mcp
```

Add `--header "Authorization: Bearer <token>"` when a token is set.

### SCPI

The virtual test harness device (USB/IP busid `2-1`, CDC-ACM) also accepts
`HUB:LIST?`, `HUB:PORT:POWer "1-1.3",ON|OFF[,FORCE]`, `HUB:PORT:POWer? "1-1.3"`,
`HUB:PORT:CYCLe "1-1.3"[,<off_ms>[,FORCE]]`, `USB:DEVices?`, `MUX:SELect "dut"`,
`MUX:SELect:CHANnel "group",<n>`, `MUX:ISOLate`, `MUX:ACTive?` and
`MUX:STATus?` (JSON answers for list queries).

## Service Discovery (mDNS / DNS-SD)

USB/IP protocol itself does not define mDNS discovery. This firmware advertises a custom DNS-SD service:

- Service type: `_usbip._tcp`
- Port: `3240`
- TXT keys: `transport`, `state`, `busid`, `vid`, `pid`, `target`
- `count` indicates the number of exported devices (`busid=multi` when `count > 1`)

Linux discovery examples:

```bash
avahi-browse -rt _usbip._tcp
avahi-resolve-host-name usbip-xxxxxx.local
```

## Notes

- The USB/IP server supports real workloads (CDC consoles, mass storage, composite devices); see Status above for the current limits.
- ESP-IDF version constraint is set in `main/idf_component.yml` to `>=6.0.0-beta2`.

## Port event log

`GET /api/ports/events` lists the last 64 port state changes, newest first. Each
event has `when_ms` (bridge uptime), `port`, `what` (`power_on`, `power_off`,
`connected`, `disconnected`, `enabled`, `disabled`, `overcurrent`,
`overcurrent_cleared`), `from`/`to`, `attribution`, `via` (when `restore`) and
`mismatch`. Attribution:

- `api`: a power change the API commanded on that port within 15 s (a
  saved-state restore is tagged `via: "restore"`, so it is never mistaken for a
  manual change);
- `device`: a connection or enable change with no commanded cause (a replug or drop);
- `external`: a power change the API did not command (hand-toggled switch,
  upstream glitch), or an over-current change.

Counters since boot (`external_power_changes`, `device_replugs`) are returned
with the events. Each port's `GET /api/ports` entry carries `last_change`
(`when_ms`, `what`, `attribution`).

