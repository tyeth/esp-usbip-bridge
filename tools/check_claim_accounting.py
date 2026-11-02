#!/usr/bin/env python3
"""Execute the actual claim-accounting functions with a fault-injected host.

Verifies usb_backend_get_claims() and the claim-failure bookkeeping that
feeds GET /api/usb/claims: per-device claimed interfaces/endpoints and
in-flight counts, claim-failure history, and that only
ESP_ERR_NOT_SUPPORTED (hcd_pipe_alloc's "No more HCD channels available")
counts as channel exhaustion. The USB host and descriptor parser are
modeled; no hardware qualification is implied.
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path


def function(source, name):
    match = re.search(
        r'^(?:static )?[A-Za-z_][\w ]*\**\b' + re.escape(name) + r'\([^;]*?\)\n\{',
        source, re.M)
    if not match:
        raise ValueError('Missing function: ' + name)
    start = source.index('{', match.start())
    depth = 0
    for pos in range(start, len(source)):
        depth += (source[pos] == '{') - (source[pos] == '}')
        if depth == 0:
            return source[match.start():pos + 1]
    raise ValueError('Unclosed function: ' + name)


PREAMBLE = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_USBIP_MAX_DEVICES 3
#define USBIP_MAX_INTERFACES 8
#define USBIP_MAX_ENDPOINTS 16
#define USB_BACKEND_MAX_HUBS 8
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_NOT_FOUND 2
#define ESP_ERR_NOT_SUPPORTED 3
#define ESP_ERR_NO_MEM 4
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define portMAX_DELAY 0
#define xSemaphoreTake(...) ((void)0)
#define xSemaphoreGive(...) ((void)0)
typedef int esp_err_t;
typedef int usb_device_handle_t;
typedef struct { uint8_t bEndpointAddress; uint16_t wMaxPacketSize; } usb_ep_desc_t;
typedef struct { int bNumEndpoints, number, alternate, offset; usb_ep_desc_t ep[2]; } usb_intf_desc_t;
typedef struct { int wTotalLength; usb_intf_desc_t intf[9]; } usb_config_desc_t;
typedef struct { uint8_t num_interfaces; char busid[32]; } usbip_backend_device_t;
typedef struct xfer { char busid[32]; struct xfer *next; } xfer_t;
typedef struct { bool in_use,release_pending; bool claimed_interfaces[USBIP_MAX_INTERFACES]; int dev_hdl;
 uint32_t halted_eps; uint32_t claim_failures; esp_err_t last_claim_err; usbip_backend_device_t device; } usb_backend_device_slot_t;
static struct {
 int client_hdl,state_mutex;
 xfer_t *inflight_head, *inflight_tail;
 usb_backend_device_slot_t devices[CONFIG_USBIP_MAX_DEVICES];
 uint32_t exhaustion_events; int64_t last_exhaustion_us; char last_exhaustion_busid[32];
} s_state;
typedef struct {
    char busid[32];
    uint8_t claimed_interfaces[USBIP_MAX_INTERFACES];
    int num_claimed_interfaces;
    int num_claimed_endpoints;
    int in_flight;
    bool release_pending;
    uint32_t claim_failures;
    char last_claim_error[24];
    bool last_claim_was_exhaustion;
} usb_backend_claims_device_t;
typedef struct {
    int connected_devices;
    int external_hubs;
    int claimed_interfaces;
    int claimed_endpoints;
    uint32_t exhaustion_events;
    int64_t last_exhaustion_us;
    char last_exhaustion_busid[32];
    int hcd_channels_total;
    int hcd_channels_allocated;
    int hcd_channel_waiters;
} usb_backend_claims_totals_t;
typedef struct { int addr; } usb_backend_hub_t;
static int fake_hubs = 2;
static size_t usb_backend_get_hubs(usb_backend_hub_t *out, size_t max) {
 (void)out; return (size_t)(fake_hubs < (int)max ? fake_hubs : (int)max);
}
static int64_t esp_timer_get_time(void) { return 424242; }
static int fake_hcd_total = 16, fake_hcd_allocated = 7, fake_hcd_err, fake_hcd_waiters = 2;
static esp_err_t usb_host_channel_stats(int *total, int *allocated) {
 if (fake_hcd_err) return ESP_ERR_INVALID_STATE;
 *total = fake_hcd_total; *allocated = fake_hcd_allocated; return ESP_OK;
}
static esp_err_t usb_host_channel_waiters(int *waiters) {
 if (fake_hcd_err) return ESP_ERR_INVALID_STATE;
 *waiters = fake_hcd_waiters; return ESP_OK;
}
static size_t strlcpy(char *d, const char *s, size_t n) { snprintf(d, n, "%s", s); return strlen(s); }
static const char *esp_err_to_name(esp_err_t e) {
 switch (e) { case 3: return "ESP_ERR_NOT_SUPPORTED"; case 4: return "ESP_ERR_NO_MEM"; default: return "ESP_ERR_OTHER"; }
}
static bool busid_matches_key(const char *key, const char *busid) { return strcmp(key, busid) == 0; }
static usb_config_desc_t configs[CONFIG_USBIP_MAX_DEVICES];
static bool host_claims[CONFIG_USBIP_MAX_DEVICES][USBIP_MAX_INTERFACES];
static int claim_fail = -1, claim_fail_err = ESP_ERR_NOT_SUPPORTED, invalid;
static esp_err_t usb_host_get_active_config_descriptor(int dev,const usb_config_desc_t **cfg) {
 *cfg=&configs[dev]; return ESP_OK;
}
static const usb_intf_desc_t *usb_parse_interface_descriptor(const usb_config_desc_t *cfg,int num,int alt,int *offset) {
 for(int i=0;i<9;i++) if(cfg->intf[i].number==num && cfg->intf[i].alternate==alt) { *offset=i; return &cfg->intf[i]; }
 return NULL;
}
static const usb_ep_desc_t *usb_parse_endpoint_descriptor_by_index(const usb_intf_desc_t *intf,int index,int total,int *offset) {
 (void)total;
 if(*offset!=intf->offset){invalid++;return NULL;}
 *offset+=20+index;return index<intf->bNumEndpoints ? &intf->ep[index] : NULL;
}
static esp_err_t usb_host_interface_claim(int client,int dev,int num,int alt) {
 (void)client;int offset=0;
 const usb_intf_desc_t *intf=usb_parse_interface_descriptor(&configs[dev],num,alt,&offset);
 if(!intf || num>=USBIP_MAX_INTERFACES || host_claims[dev][num]) {invalid++;return ESP_ERR_INVALID_STATE;}
 if(num==claim_fail) return claim_fail_err;
 host_claims[dev][num]=true;return ESP_OK;
}
'''

TESTS = r'''
static int cases,failures;
#define CHECK(x) do { if(!(x)){printf("FAIL %d: %s\n",__LINE__,#x);failures++;} }while(0)
static void reset(void) {
 memset(&s_state,0,sizeof(s_state));memset(configs,0,sizeof(configs));memset(host_claims,0,sizeof(host_claims));
 claim_fail=-1;claim_fail_err=ESP_ERR_NOT_SUPPORTED;invalid=0;fake_hubs=2;
 const char *ids[3]={"a","b","c"};
 for(int d=0;d<3;d++) {
  s_state.devices[d].in_use=(d<2);s_state.devices[d].dev_hdl=d;s_state.devices[d].device.num_interfaces=4;
  strcpy(s_state.devices[d].device.busid,ids[d]);configs[d].wTotalLength=100;
  for(int i=0;i<9;i++) configs[d].intf[i].number=-1;
  for(int i=0;i<4;i++) {
   usb_intf_desc_t *p=&configs[d].intf[i];p->number=i;p->offset=i;p->bNumEndpoints=i?2:1;
   p->ep[0].bEndpointAddress=0x81+i;if(i)p->ep[1].bEndpointAddress=1+i;
  }
 }
}
int main(void) {
 usb_backend_claims_device_t out[4];usb_backend_claims_totals_t totals;

 /* 1: snapshot of claimed interfaces, endpoints, totals */
 reset();cases++;
 CHECK(ensure_endpoint_interface_claimed_locked(0,0x81)==ESP_OK); /* a: intf0 (1 ep) */
 CHECK(ensure_endpoint_interface_claimed_locked(0,0x82)==ESP_OK); /* a: intf1 (2 eps) */
 CHECK(ensure_endpoint_interface_claimed_locked(1,0x81)==ESP_OK); /* b: intf0 (1 ep) */
 CHECK(usb_backend_get_claims(out,4,&totals)==2);
 CHECK(strcmp(out[0].busid,"a")==0 && strcmp(out[1].busid,"b")==0);
 CHECK(out[0].num_claimed_interfaces==2 && out[0].claimed_interfaces[0]==0 && out[0].claimed_interfaces[1]==1);
 CHECK(out[0].num_claimed_endpoints==3);
 CHECK(out[1].num_claimed_interfaces==1 && out[1].num_claimed_endpoints==1);
 CHECK(out[0].claim_failures==0 && out[0].last_claim_error[0]==0 && !out[0].last_claim_was_exhaustion);
 CHECK(totals.connected_devices==2 && totals.external_hubs==2);
 CHECK(totals.claimed_interfaces==3 && totals.claimed_endpoints==4);
 CHECK(totals.exhaustion_events==0 && totals.last_exhaustion_us==0);
 CHECK(totals.hcd_channels_total==16 && totals.hcd_channels_allocated==7);
 CHECK(totals.hcd_channel_waiters==2);
 fake_hcd_err=1;
 CHECK(usb_backend_get_claims(out,4,&totals)==2);
 CHECK(totals.hcd_channels_total==-1 && totals.hcd_channels_allocated==-1);
 CHECK(totals.hcd_channel_waiters==-1);
 fake_hcd_err=0;
 CHECK(invalid==0);

 /* 2: in-flight transfers are counted per busid */
 reset();cases++;
 xfer_t x1={.busid="a"},x2={.busid="a"},x3={.busid="b"};
 x1.next=&x2;x2.next=&x3;x3.next=NULL;s_state.inflight_head=&x1;
 CHECK(usb_backend_get_claims(out,4,&totals)==2);
 CHECK(out[0].in_flight==2 && out[1].in_flight==1);

 /* 3: channel exhaustion (ESP_ERR_NOT_SUPPORTED) is recorded and attributed */
 reset();cases++;claim_fail=2;
 CHECK(ensure_endpoint_interface_claimed_locked(0,0x81)==ESP_OK);
 CHECK(ensure_endpoint_interface_claimed_locked(0,0x83)==ESP_ERR_NOT_SUPPORTED);
 CHECK(ensure_endpoint_interface_claimed_locked(0,0x83)==ESP_ERR_NOT_SUPPORTED);
 CHECK(usb_backend_get_claims(out,4,&totals)==2);
 CHECK(out[0].claim_failures==2);
 CHECK(strcmp(out[0].last_claim_error,"ESP_ERR_NOT_SUPPORTED")==0);
 CHECK(out[0].last_claim_was_exhaustion);
 CHECK(out[0].num_claimed_interfaces==1); /* the earlier claim is kept */
 CHECK(totals.exhaustion_events==2);
 CHECK(strcmp(totals.last_exhaustion_busid,"a")==0);
 CHECK(totals.last_exhaustion_us==424242);
 CHECK(out[1].claim_failures==0 && out[1].last_claim_error[0]==0);

 /* 4: a non-exhaustion claim failure is counted but not exhaustion */
 reset();cases++;claim_fail=1;claim_fail_err=ESP_ERR_NO_MEM;
 CHECK(ensure_endpoint_interface_claimed_locked(1,0x82)==ESP_ERR_NO_MEM);
 CHECK(usb_backend_get_claims(out,4,&totals)==2);
 CHECK(out[1].claim_failures==1);
 CHECK(strcmp(out[1].last_claim_error,"ESP_ERR_NO_MEM")==0);
 CHECK(!out[1].last_claim_was_exhaustion);
 CHECK(totals.exhaustion_events==0 && totals.last_exhaustion_busid[0]==0);

 /* 5: totals are complete even when out is NULL or too small */
 reset();cases++;
 CHECK(ensure_endpoint_interface_claimed_locked(0,0x82)==ESP_OK);
 CHECK(ensure_endpoint_interface_claimed_locked(1,0x82)==ESP_OK);
 CHECK(usb_backend_get_claims(NULL,0,&totals)==0);
 CHECK(totals.connected_devices==2 && totals.claimed_interfaces==2 && totals.claimed_endpoints==4);
 CHECK(usb_backend_get_claims(out,1,&totals)==1);
 CHECK(strcmp(out[0].busid,"a")==0);
 CHECK(totals.connected_devices==2 && totals.claimed_endpoints==4);

 /* 6: release_pending is reported */
 reset();cases++;
 s_state.devices[1].release_pending=true;
 CHECK(usb_backend_get_claims(out,4,&totals)==2);
 CHECK(!out[0].release_pending && out[1].release_pending);
 CHECK(invalid==0);

 printf("%d groups, %d failed assertions\n",cases,failures);return failures?1:0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'main/usb_backend.c')
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    source = args.source.read_text()
    assert 'usb_backend_get_claims' in source
    names = ['find_endpoint_interface_locked', 'ensure_endpoint_interface_claimed_locked',
             'collect_claimed_endpoints_locked', 'count_in_flight_locked',
             'usb_backend_get_claims']
    program = PREAMBLE + '\n'.join(function(source, n) for n in names) + TESTS
    with tempfile.TemporaryDirectory(prefix='usb-claims-acct-') as directory:
        root = Path(directory)
        path = root / 'accounting.c'
        path.write_text(program)
        subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-g', str(path),
                        '-o', str(root / 'accounting')], check=True)
        return subprocess.run([str(root / 'accounting')]).returncode


if __name__ == '__main__':
    raise SystemExit(main())
