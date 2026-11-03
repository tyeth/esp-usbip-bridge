set(req_components esp_event esp_netif esp_timer lwip nvs_flash usb mdns esp_driver_i2c
                   fatfs esp-serial-flasher app_update esp_app_format esp_partition littlefs spiffs
                   harness_io harness_scpi harness_dut harness_io_expander)

set(srcs
    "main.c"
    "device_naming.c"
    "discovery_service.c"
    "network_init.c"
    "usb_backend.c"
    "log_server.c"
    "usbip_server.c"
    "http_server.c"
    "led_strip_encoder.c"
    "neopixel.c"
    "hub_control.c"
    "port_events.c"
    "analog_mux.c"
    "controller_api.c"
    "busid_owner.c"
    "rescue_logic.c"
    "device_rescue.c"
    "serial_link.c"
    "blob_store.c"
    "rescue_job.c"
    "capture.c"
    "capture_stream.c"
    "esp_flasher.c"
    "picoboot_flasher.c"
    "samba_flasher.c"
    "msc_host.c"
    "ota_logic.c"
    "ota_update.c"
    "dut_part_logic.c"
    "dut_fs.c"
    "dut_flash.c"
)

if(CONFIG_USBIP_VIRTUAL_LOGIC_ANALYZER OR CONFIG_USBIP_VIRTUAL_HARNESS)
    list(APPEND srcs
        "virtual_device.c"
        "virtual_cdc.c"
    )
endif()

if(CONFIG_USBIP_VIRTUAL_LOGIC_ANALYZER)
    list(APPEND req_components sump sump_io logic_capture perfetto_trace)
    list(APPEND srcs
        "virtual_perfetto_logic.c"
    )
endif()

if(CONFIG_USBIP_VIRTUAL_HARNESS)
    list(APPEND req_components harness_io_expander harness_gpio harness_bus)
    list(APPEND srcs
        "virtual_harness.c"
        "scpi_controller.c"
    )
endif()

if(IDF_TARGET STREQUAL "esp32p4")
    # esp_wifi and esp_driver_uart: the Wi-Fi station and serial console of a
    # Wi-Fi-only board (waveshare-p4-wifi6).  The P4 has no radio, so esp_wifi
    # is the API only, backed by esp_hosted (main/idf_component.yml) on that
    # board; the other P4 builds compile nothing that calls it.
    list(APPEND req_components esp_eth esp_wifi esp_driver_uart)
elseif(IDF_TARGET STREQUAL "esp32s31")
    # Ethernet, and the optional Wi-Fi station next to it
    list(APPEND req_components esp_eth esp_wifi esp_driver_uart)
elseif(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND req_components esp_wifi esp_driver_gpio)
endif()

# Optional Wi-Fi station next to Ethernet (S31), or alone (a P4 with an
# ESP-Hosted co-processor), with its serial console
# commands (wifi set/status/clear, reboot).  The requirements above go by
# target: sdkconfig values are not known yet when IDF expands them.
if(CONFIG_USBIP_WIFI_STA)
    list(APPEND srcs "wifi_sta.c" "serial_console.c")
endif()

# Always compiled; neopixel_init() becomes a no-op when gpio < 0.
list(APPEND req_components esp_driver_rmt)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${req_components}
    EMBED_TXTFILES "web_app.js"
)

# dut_fs.c drives the SPIFFS core of ESP-IDF's spiffs component directly
# (spiffs.h, and spiffs_api.h for the lock its SPIFFS_LOCK takes), which the
# component keeps in private include directories.
idf_component_get_property(spiffs_dir spiffs COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${spiffs_dir}" "${spiffs_dir}/spiffs/src")

# Syntax-check the embedded browser JS at build time when node is available.
# This is best-effort: hosts without node simply skip the check so CI and
# local builds stay portable.  Run `node --check main/web_app.js` manually
# to validate when node is absent.
find_program(NODE_JS node)
if(NODE_JS)
    add_custom_target(main_jscheck
        COMMAND "${NODE_JS}" --check "${CMAKE_CURRENT_SOURCE_DIR}/web_app.js"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web_app.js"
        COMMENT "Syntax-checking web_app.js"
        VERBATIM)
    add_dependencies(${COMPONENT_LIB} main_jscheck)
endif()
