# Bridge discovery, capabilities and versioning

The contract between a bridge and anything that drives it (the HIL controller,
CI, scripts).  Clients find bridges with mDNS, read `GET /api/capabilities`, and
decide what to use from **feature versions**, never from the firmware commit.

## Versioning

- `api_version` is `"MAJOR.MINOR"`.  MINOR goes up when something is added
  (a field, an endpoint, a feature); MAJOR goes up only when a published field,
  endpoint or behaviour changes or goes away.  This document starts at `"1.0"`.
- Every feature has its own integer `version`, starting at 1 and going up when
  that feature gains or changes behaviour.  A client asks "is `flash_esp` at
  least 1?", not "is the firmware newer than ac4fbed?".
- Clients ignore fields and features they don't know.  A bridge without
  `/api/capabilities` (404) is a pre-1.0 bridge: treat it as having only the
  `/api/info` and `/api/devices` endpoints.
- `GET /api/info` gains `"api_version"` (same value).

## `GET /api/capabilities`

No auth needed (same rule as `/api/info`).  Response:

```json
{
  "schema": 1,
  "api_version": "1.0",
  "fw": {
    "project": "esp32p4_usbip_bridge",
    "version": "ac4fbed",
    "build_time": "2026-11-02 17:11:31",
    "idf": "v6.1-dev-8295-gce2100de51a",
    "elf_sha256": "62edb3b63e932c29",
    "running_partition": "factory"
  },
  "board": {
    "target": "esp32s31",
    "profile": "s31-function-coreboard-1",
    "mac": "1c:29:04:d1:9b:ee",
    "hostname": "usbip-s31-d19bee",
    "board_id": null
  },
  "network": {
    "transports": ["ethernet", "wifi"],
    "interfaces": [
      {"name": "eth",  "up": true,  "ip": "192.168.1.133", "mac": "1e:29:04:d1:9b:ef"},
      {"name": "wifi", "up": false, "ip": null, "mac": "1c:29:04:d1:9b:ee", "ssid": null, "rssi": null,
       "configured": false, "stored": false, "state": "unconfigured", "attempts": 0, "last_reason": null}
    ]
  },
  "ports": {"http": 80, "usbip": 3240, "serial_stream": 3241},
  "auth": {"required": false},
  "limits": {
    "max_devices": 32,
    "max_interfaces_per_device": 8,
    "max_endpoints_per_device": 16,
    "max_inflight_transfers": 128,
    "rescue_staging_bytes": 12582912
  },
  "features": {
    "usbip":          {"version": 1},
    "claims":         {"version": 1},
    "hub_power":      {"version": 1},
    "port_events":    {"version": 1},
    "mux":            {"version": 1, "available": true, "service": "_i2c-strand._tcp",
                       "chips": "adg729", "groups": 1, "duts": 4},
    "device_info":    {"version": 1},
    "serial_lines":   {"version": 1},
    "reset":          {"version": 1, "modes": ["esp_bootloader", "esp_hard_reset", "esp_usb_jtag_bootloader",
                                               "esp_usb_jtag_reset", "touch_1200", "rp_bootsel", "rp_reboot",
                                               "picoboot_reboot", "picoboot_bootsel", "bootloader"]},
    "console":        {"version": 1},
    "capture":        {"version": 1},
    "serial_stream":  {"version": 1},
    "flash_esp":      {"version": 1},
    "flash_picoboot": {"version": 1},
    "flash_samba":    {"version": 1},
    "flash_uf2":      {"version": 1},
    "msc":            {"version": 1},
    "dut_fs":         {"version": 1, "filesystems": ["fat", "littlefs", "spiffs"]},
    "ota":            {"version": 1},
    "wifi_config":    {"version": 1},
    "debug_ctrl":     {"version": 1}
  }
}
```

The example values are .133's.  Rules:

- A feature appears only when the build has it (a P4 build without OTA has no
  `ota` key).  `available: false` means compiled in but not usable right now
  (e.g. `mux` with no ADG729 on the bus); clients must check it where present.
- The `reset.modes` and `dut_fs.filesystems` lists are the real lists the
  firmware accepts; keep them generated from the same tables the handlers use
  (`rescue_mode_name()` over every mode including `bootloader`, the automatic
  pick; `dut_fs_kinds[]`).  The `flash_*`, `msc` and `dut_fs` keys come from
  the rescue job table (`k_job_kinds[]` in `controller_api.c`).
- `ota` is listed when the partition table has OTA slots; `wifi_config` when
  the build has the Wi-Fi station next to Ethernet (`CONFIG_USBIP_WIFI_STA`).
- `fw.version` is the git describe part of the firmware version and
  `fw.build_time` the rest (`/api/info` `version` carries both); `board.profile`
  is the `scripts/build-board.sh` profile, `null` for none.
- Feature names are stable identifiers; never rename one (add a new one).

## mDNS / DNS-SD

- Hostname `<hostname>.local`, also the DHCP option 12 name on Ethernet and
  Wi-Fi: `usbip-<chip>-<last 3 MAC bytes>` unless one is set, where `<chip>`
  is the IDF target without `esp32` plus, on the ESP32-P4, its major silicon
  revision (v1 and v3 differ): `usbip-s31-d19bee`, `usbip-s3-0a1b2c`,
  `usbip-p4v1-0a1b2c`, `usbip-p4v3-0a1b2c`.  A stored hostname wins; a stored
  copy of the old default (`usbip-<last 3 MAC bytes>`) is cleared on boot
  (logged once) and the new default applies.
- Keep `_usbip._tcp` on 3240 with its existing, frequently updated TXT, with
  the DNS-SD subtype `_esp-usbip-bridge`.
- **`_esp-usbip._tcp` on port 80** (the bridge's HTTP API; `_esp-usbip-bridge`
  is 16 characters, over the 15-character service-name limit), subtype
  `_esp-usbip-bridge`, with a small, static TXT (republished only when a value
  changes):

  | key | value |
  |---|---|
  | `product` | `esp-usbip-bridge` |
  | `api` | `1.0` |
  | `fw` | firmware version (`ac4fbed`) |
  | `target` | `esp32s31` |
  | `board` | profile (`s31-function-coreboard-1`) |
  | `mac` | base MAC, lower case with colons |
  | `caps` | `/api/capabilities` |
  | `auth` | `0` or `1` |

  Instance name: the hostname.  Advertise on every interface that is up
  (Ethernet and Wi-Fi).  Every board profile advertises it.
- **`_i2c-strand._tcp` on port 80**: the I2C strand mux API (`/api/select`,
  `/api/select/<dut>`, `/api/isolate`, `/api/groups/<g>/channel`,
  `/api/groups/<g>/select/<c>`, `/api/topology`, `/api/duts`, `/api/probe`,
  `/api/status`), advertised while the mux control bus is up
  (`features.mux.available`), even with no groups configured.  The standalone
  CircuitPython mux box advertises the same service with
  `product=sbc-dut-analog-mux`.  TXT, republished only when a value changes
  (e.g. after `PUT /api/topology`):

  | key | value |
  |---|---|
  | `product` | `esp-usbip-bridge` |
  | `api` | `1` (the strand-mux API version, independent of the bridge's) |
  | `mux` | `adg729`, `adg728`, `mixed` or `none` (from the configured groups) |
  | `groups`, `duts` | counts in the topology |
  | `path` | `/api` |
  | `mac` | base MAC |
  | `caps` | `/api/capabilities` |
- Clients browse `_esp-usbip._tcp` (or `_esp-usbip-bridge._sub._usbip._tcp`),
  then confirm with `GET /api/capabilities` (TXT is a hint, the endpoint is
  the truth).  `_hil-bridge._tcp` (the pre-release name) is no longer
  advertised.

## Wi-Fi (S31, Waveshare ESP32-P4-WIFI6)

Only builds with the `esp_wifi` API: a radio (the S31), or a Wi-Fi
co-processor through `esp_wifi_remote` and `esp_hosted` (the Waveshare
ESP32-P4-WIFI6's ESP32-C6, `CONFIG_USBIP_WIFI_REMOTE`).  The Ethernet P4 builds
have neither, so no Wi-Fi and no `wifi_config` feature (their `/api/network`
lists `eth` only, and `PUT`/`DELETE /api/network/wifi` answer 501).  The S3
build's Wi-Fi is its only transport, configured at boot as before; it lists
`wifi` without `wifi_config`.  The Waveshare ESP32-P4-WIFI6 has no Ethernet:
its station is the only transport (`transports: ["wifi"]`, with
`wifi_config`), configured the same three ways below; everything about
Ethernet in this section does not apply to it.

Ethernet stays the primary transport (and the default route).  A board may
also join a Wi-Fi network as a station; both interfaces can be up at once and
every service (HTTP, USB/IP, serial streams, mDNS) answers on both, each with
a DHCP address under the same hostname.  A board with no Ethernet link and
Wi-Fi credentials is fully usable over Wi-Fi.  Boot never waits for Wi-Fi; a
lost or failed association is retried with a 1 s backoff doubling to 30 s.

The `wifi` interface object adds `stored` (the credentials in use are the ones
in NVS), `state` (`unconfigured`, `connecting`, `connected`, `retrying`),
`attempts` (failures since the last connect), `last_reason` (the last
disconnect reason, e.g. `auth_fail`, `no_ap_found`, or `null`) and, while
retrying, `retry_in_ms`.

Credentials live in NVS namespace `wifi`, keys `ssid` and `pass` (the existing
S3 keys).  They can be set the way a user would, three ways:

1. **Serial console** (the board's console UART, 115200: the CP2102N on the
   S31, the CH343 on the Waveshare ESP32-P4-WIFI6): line commands, never blocking boot —
   `wifi status`, `wifi set <ssid> [<password>]` (quotes allowed for spaces;
   no password joins an open network; stores at once and connects without a
   reboot), `wifi clear`, `reboot`, `help`.  Answers are one line starting
   `wifi:` (`reboot:`, `console:` for the others); the password echoes as `*`.
2. **HTTP**: `GET /api/network` (the `network` object above),
   `PUT /api/network/wifi {"ssid": "...", "password": "...", "store": false,
   "wait_ms": 10000}` (connects without a reboot, stores on success or with
   `"store": true` regardless; waits up to `wait_ms`, default and max 10000,
   for an address and answers 200 `{"ok", "connected": true, "stored",
   "network"}`, or 202 with `"connected": false` while it keeps trying; an
   empty password joins an open network, otherwise 8-63 characters),
   `DELETE /api/network/wifi`.  The password is never returned.  Auth rules as
   for the other write endpoints.
3. **Web UI**: a Network section on the bridge page with the same three actions.

No partition table change: there is no user-FS route for settings on S31 (the
OTA table has no spare data partition; adding one needs a wired flash).

## CI deploy

`uat.yml` never deploys on push.  The `deploy` job runs only on
`workflow_dispatch` with an input `bridges` (space-separated host names or
addresses, required, no default) on the `[self-hosted, bridge-lan]` runner.
