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
    "hostname": "usbip-d19bee",
    "board_id": null
  },
  "network": {
    "transports": ["ethernet", "wifi"],
    "interfaces": [
      {"name": "eth",  "up": true,  "ip": "192.168.1.133", "mac": "1e:29:04:d1:9b:ef"},
      {"name": "wifi", "up": false, "ip": null, "ssid": null, "rssi": null, "configured": false}
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
    "mux":            {"version": 1, "available": true},
    "device_info":    {"version": 1},
    "serial_lines":   {"version": 1},
    "reset":          {"version": 1, "modes": ["esp_hard_reset", "esp_bootloader", "usb_jtag_reset", "touch_1200", "picoboot_bootsel"]},
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
  firmware accepts; keep them generated from the same tables the handlers use.
- Feature names are stable identifiers; never rename one (add a new one).

## mDNS / DNS-SD

- Hostname `<hostname>.local` as today (`usbip-<last 3 MAC bytes>` unless set).
- Keep `_usbip._tcp` on 3240 with its existing, frequently updated TXT.
- **Add `_hil-bridge._tcp` on port 80** with a small, static TXT (republished
  only when a value changes):

  | key | value |
  |---|---|
  | `api` | `1.0` |
  | `fw` | firmware version (`ac4fbed`) |
  | `target` | `esp32s31` |
  | `board` | profile (`s31-function-coreboard-1`) |
  | `mac` | base MAC, lower case with colons |
  | `caps` | `/api/capabilities` |
  | `auth` | `0` or `1` |

  Instance name: the hostname.  Advertise on every interface that is up
  (Ethernet and Wi-Fi).
- Clients browse `_hil-bridge._tcp`, then confirm with `GET /api/capabilities`
  (TXT is a hint, the endpoint is the truth).

## Wi-Fi (S31 and P4 Ethernet boards)

Ethernet stays the primary transport.  A board may also join a Wi-Fi network as
a station; both interfaces can be up at once and every service (HTTP, USB/IP,
serial streams, mDNS) answers on both.  A board with no Ethernet link and Wi-Fi
credentials is fully usable over Wi-Fi.

Credentials live in NVS namespace `wifi`, keys `ssid` and `pass` (the existing
S3 keys).  They can be set the way a user would, three ways:

1. **Serial console** (the board's UART/USB console, 115200): line commands,
   never blocking boot —
   `wifi status`, `wifi set <ssid> <password>` (quotes allowed for spaces),
   `wifi clear`, `reboot`.  Answers are one line starting `wifi:`.
2. **HTTP**: `GET /api/network` (the `network` object above),
   `PUT /api/network/wifi {"ssid": "...", "password": "..."}` (connects
   without a reboot, stores on success or with `"store": true` regardless),
   `DELETE /api/network/wifi`.  The password is never returned.  Auth rules as
   for the other write endpoints.
3. **Web UI**: a Network section on the bridge page with the same three actions.

No partition table change: there is no user-FS route for settings on S31 (the
OTA table has no spare data partition; adding one needs a wired flash).

## CI deploy

`uat.yml` never deploys on push.  The `deploy` job runs only on
`workflow_dispatch` with an input `bridges` (space-separated host names or
addresses, required, no default) on the `[self-hosted, bridge-lan]` runner.
