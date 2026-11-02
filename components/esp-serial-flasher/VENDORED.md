# esp-serial-flasher (vendored)

Source: https://github.com/espressif/esp-serial-flasher, component registry
release **2.1.0** (`espressif/esp-serial-flasher`), Apache-2.0 (see LICENSE).
The bundled flasher stubs come from espressif/esp-flasher-stub (Apache-2.0 OR MIT).

Copied unchanged except that `idf_component.yml`, examples, tests and docs are
left out. The manifest is dropped on purpose: 2.1.0 declares
`usb_host_cdc_acm` / `usb_host_cp210x_vcp` / `usb_host_ch34x_vcp` for USB OTG
targets (esp32s31 included), and those require the registry `espressif/usb`,
which the component manager then substitutes for this project's pinned esp-usb
fork (hub port power API). The bridge does not use the library's ports; it
provides its own (`main/esp_flash_port.c`, USB serial through the bridge's USB
host backend), and the library's built-in ports are all disabled in
`sdkconfig.defaults.s31-function-coreboard-1`.

To update: replace these directories from the new release and keep the
manifest out.
