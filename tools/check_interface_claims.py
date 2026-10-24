#!/usr/bin/env python3
"""Compile the actual backend claim/release functions with a fault-injected host.

No hardware access. Run with --source <old usb_backend.c> for the negative control.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def function(source, name):
    import re
    match = re.search(r'^static [^\n]+\b' + re.escape(name) + r'\([^\n]*\)\n\{', source, re.M)
    if not match:
        raise ValueError('Missing function: ' + name)
    start = source.index('{', match.start())
    depth = 0
    for position in range(start, len(source)):
        depth += (source[position] == '{') - (source[position] == '}')
        if depth == 0:
            return source[match.start():position + 1]
    raise ValueError('Unclosed function: ' + name)


PREAMBLE = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_USBIP_MAX_DEVICES 2
#define USBIP_MAX_INTERFACES 16
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_NOT_SUPPORTED 3
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define portMAX_DELAY 0
#define xSemaphoreTake(...) ((void)0)
#define xSemaphoreGive(...) ((void)0)
typedef int esp_err_t;
typedef int usb_device_handle_t;
typedef struct { uint8_t num_interfaces; char busid[32]; } usbip_backend_device_t;
typedef struct {
    bool in_use, interfaces_claimed, release_pending;
    bool claimed_interfaces[USBIP_MAX_INTERFACES];
    int dev_hdl;
    uint32_t halted_eps;
    uint32_t claim_failures;
    esp_err_t last_claim_err;
    usbip_backend_device_t device;
} slot_t;
static struct { slot_t devices[CONFIG_USBIP_MAX_DEVICES]; int client_hdl, state_mutex;
    uint32_t exhaustion_events; int64_t last_exhaustion_us; char last_exhaustion_busid[32]; } s_state;
static int64_t esp_timer_get_time(void) { return 424242; }
static size_t strlcpy(char *d, const char *s, size_t n) { snprintf(d, n, "%s", s); return strlen(s); }
static bool host_claims[USBIP_MAX_INTERFACES], in_flight;
static int claim_failure, release_failure, invalid_calls;
static int find_slot_by_busid_locked(const char *busid) { (void)busid; return s_state.devices[0].in_use ? 0 : -1; }
static bool busid_in_flight_locked(const char *busid) { (void)busid; return in_flight; }
static esp_err_t usb_host_interface_claim(int client, int device, uint8_t i, int alt) {
    (void)client; (void)device; (void)alt;
    if (i >= USBIP_MAX_INTERFACES || host_claims[i]) { invalid_calls++; return ESP_ERR_INVALID_STATE; }
    if (i == claim_failure) return ESP_ERR_NO_MEM;
    host_claims[i] = true; return ESP_OK;
}
static esp_err_t usb_host_interface_release(int client, int device, uint8_t i) {
    (void)client; (void)device;
    if (i >= USBIP_MAX_INTERFACES || !host_claims[i]) { invalid_calls++; return ESP_ERR_INVALID_STATE; }
    if (i == release_failure) return ESP_ERR_INVALID_STATE;
    host_claims[i] = false; return ESP_OK;
}
'''

TESTS = r'''
static int cases, failures;
static const char busid[32] = "test-device";
static void reset(void) {
    memset(&s_state, 0, sizeof(s_state)); memset(host_claims, 0, sizeof(host_claims));
    s_state.devices[0].in_use = true; s_state.devices[0].device.num_interfaces = 6;
    strcpy(s_state.devices[0].device.busid, "test-device");
    claim_failure = release_failure = -1; invalid_calls = 0; in_flight = false;
}
static int owned(void) { int n=0; for (int i=0;i<USBIP_MAX_INTERFACES;i++) n+=host_claims[i]; return n; }
#define CHECK(condition) do { if (!(condition)) { printf("FAIL line %d: %s\n",__LINE__,#condition); failures++; } } while (0)
int main(void) {
    for (int fail=0;fail<6;fail++) {
        reset(); cases++; claim_failure=fail;
        for (int retry=0;retry<3;retry++) {
            CHECK(ensure_interfaces_claimed_locked(0)==ESP_ERR_NO_MEM);
            CHECK(owned()==0); CHECK(invalid_calls==0);
        }
        claim_failure=-1;
        CHECK(ensure_interfaces_claimed_locked(0)==ESP_OK); CHECK(owned()==6);
        release_session_device(busid); CHECK(owned()==0); CHECK(invalid_calls==0);
    }
    for (int fail=1;fail<6;fail++) for (int stuck=0;stuck<fail;stuck++) {
        reset(); cases++; claim_failure=fail; release_failure=stuck;
        CHECK(ensure_interfaces_claimed_locked(0)==ESP_ERR_NO_MEM);
        CHECK(owned()==1); CHECK(host_claims[stuck]);
        CHECK(s_state.devices[0].claimed_interfaces[stuck]); CHECK(invalid_calls==0);
        claim_failure=release_failure=-1;
        CHECK(ensure_interfaces_claimed_locked(0)==ESP_OK); CHECK(owned()==6); CHECK(invalid_calls==0);
        release_session_device(busid); CHECK(owned()==0); CHECK(invalid_calls==0);
    }
    reset(); cases++;
    CHECK(ensure_interfaces_claimed_locked(0)==ESP_OK);
    CHECK(ensure_interfaces_claimed_locked(0)==ESP_OK); CHECK(owned()==6); CHECK(invalid_calls==0);
    release_interfaces_locked(0); release_interfaces_locked(0); CHECK(owned()==0); CHECK(invalid_calls==0);
    reset(); cases++; claim_failure=2; release_failure=0;
    CHECK(ensure_interfaces_claimed_locked(0)==ESP_ERR_NO_MEM); CHECK(owned()==1);
    in_flight=true; release_session_device(busid);
    CHECK(s_state.devices[0].release_pending); CHECK(owned()==1);
    in_flight=false; release_failure=-1; release_session_device(busid);
    CHECK(owned()==0); CHECK(invalid_calls==0);
    reset(); cases++;
    CHECK(ensure_interfaces_claimed_locked(-1)==ESP_ERR_INVALID_STATE);
    CHECK(ensure_interfaces_claimed_locked(CONFIG_USBIP_MAX_DEVICES)==ESP_ERR_INVALID_STATE);
    release_interfaces_locked(-1); release_interfaces_locked(CONFIG_USBIP_MAX_DEVICES);
    s_state.devices[0].in_use=false;
    CHECK(ensure_interfaces_claimed_locked(0)==ESP_ERR_INVALID_STATE);
    release_session_device(busid); CHECK(owned()==0); CHECK(invalid_calls==0);
    printf("%d cases, %d failed assertions\n",cases,failures);
    return failures ? 1 : 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1]/'main/usb_backend.c')
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    source = args.source.read_text()
    if 'static esp_err_t ensure_endpoint_interface_claimed_locked(' in source:
        # Preserve the old --source regression path for historical all-interface
        # implementations; current code has a per-interface demand contract.
        return subprocess.run([sys.executable,
                               str(Path(__file__).with_name('check_interface_demand.py')),
                               '--source', str(args.source), '--cc', args.cc]).returncode
    names = ['release_interfaces_locked', 'ensure_interfaces_claimed_locked', 'release_session_device']
    if 'static bool has_claimed_interfaces_locked(' in source:
        names.insert(0, 'has_claimed_interfaces_locked')
    program = PREAMBLE + '\n'.join(function(source, name) for name in names) + TESTS
    with tempfile.TemporaryDirectory(prefix='usb-claims-') as directory:
        root=Path(directory); path=root/'claims.c'; path.write_text(program)
        subprocess.run([args.cc,'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined','-g',str(path),'-o',str(root/'claims')],check=True)
        return subprocess.run([str(root/'claims')]).returncode


if __name__=='__main__':
    raise SystemExit(main())
