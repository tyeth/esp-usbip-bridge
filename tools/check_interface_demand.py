#!/usr/bin/env python3
"""Execute actual backend interface-demand functions with a fault-injected host.

The USB host and descriptor parser are modeled. No hardware qualification is
implied. Capacity costs use the observed CDC/MSC/HID descriptors (1/2/2/2).
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, name):
    match = re.search(r'^static [^\n]+\b' + re.escape(name) + r'\([^)]*\)\n\{', source, re.M)
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
#include <stdio.h>
#include <string.h>
#define CONFIG_USBIP_MAX_DEVICES 2
#define USBIP_MAX_INTERFACES 8
#define USBIP_MAX_ENDPOINTS 16
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_NOT_FOUND 2
#define ESP_ERR_NOT_SUPPORTED 3
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define portMAX_DELAY 0
#define xSemaphoreTake(...) ((void)0)
#define xSemaphoreGive(...) ((void)0)
#define USB_BM_REQUEST_TYPE_DIR_OUT 0
#define USB_BM_REQUEST_TYPE_TYPE_STANDARD 0
#define USB_BM_REQUEST_TYPE_RECIP_ENDPOINT 2
#define USB_BM_REQUEST_TYPE_RECIP_INTERFACE 1
#define USB_BM_REQUEST_TYPE_RECIP_DEVICE 0
#define USB_B_REQUEST_CLEAR_FEATURE 1
#define USB_B_REQUEST_SET_INTERFACE 11
#define USB_B_REQUEST_SET_CONFIGURATION 9
typedef int esp_err_t;
typedef int usb_device_handle_t;
typedef struct { uint8_t bEndpointAddress; uint16_t wMaxPacketSize; } usb_ep_desc_t;
typedef struct { int bNumEndpoints, number, alternate, offset; usb_ep_desc_t ep[2]; } usb_intf_desc_t;
typedef struct { int wTotalLength; usb_intf_desc_t intf[17]; } usb_config_desc_t;
typedef struct { uint8_t bmRequestType,bRequest; uint16_t wValue,wIndex; } usb_setup_packet_t;
typedef struct { usb_setup_packet_t setup; char busid[32]; } xfer_t;
typedef struct { uint8_t num_interfaces; char busid[32]; } usbip_backend_device_t;
typedef struct { bool in_use,release_pending; bool claimed_interfaces[USBIP_MAX_INTERFACES]; int dev_hdl;
 uint32_t halted_eps; uint32_t claim_failures; esp_err_t last_claim_err; usbip_backend_device_t device; } usb_backend_device_slot_t;
static struct { usb_backend_device_slot_t devices[2]; int client_hdl,state_mutex;
 uint32_t exhaustion_events; int64_t last_exhaustion_us; char last_exhaustion_busid[32]; } s_state;
static int64_t esp_timer_get_time(void) { return 424242; }
static size_t strlcpy(char *d, const char *s, size_t n) { snprintf(d, n, "%s", s); return strlen(s); }
static usb_config_desc_t configs[2];
static bool claims[2][USBIP_MAX_INTERFACES], config_error, config_null, in_flight;
static int channels,invalid,claim_calls,claim_fail,release_fail,resets;
static uint8_t reset_eps[16];
static int find_slot_by_busid_locked(const char *busid) { return busid[0]=='b' ? 1 : 0; }
static bool busid_in_flight_locked(const char *busid) { (void)busid; return in_flight; }
static int usb_host_get_active_config_descriptor(int dev,const usb_config_desc_t **cfg) {
 *cfg=config_null?NULL:&configs[dev]; return config_error?ESP_ERR_INVALID_STATE:ESP_OK;
}
static const usb_intf_desc_t *usb_parse_interface_descriptor(const usb_config_desc_t *cfg,int num,int alt,int *offset) {
 for(int i=0;i<17;i++) if(cfg->intf[i].number==num && cfg->intf[i].alternate==alt) {
  *offset=i;return &cfg->intf[i];
 }
 return NULL;
}
static const usb_ep_desc_t *usb_parse_endpoint_descriptor_by_index(const usb_intf_desc_t *intf,int index,int total,int *offset) {
 (void)total;
 if(*offset!=intf->offset){invalid++;return NULL;}
 *offset+=20+index;return index<intf->bNumEndpoints ? &intf->ep[index] : NULL;
}
static int usb_host_interface_claim(int client,int dev,int num,int alt) {
 (void)client;int offset=0;claim_calls++;
 const usb_intf_desc_t *intf=usb_parse_interface_descriptor(&configs[dev],num,alt,&offset);
 if(!intf || num>=USBIP_MAX_INTERFACES || claims[dev][num]) {invalid++;return ESP_ERR_INVALID_STATE;}
 if(num==claim_fail || channels+intf->bNumEndpoints>16) return ESP_ERR_NOT_SUPPORTED;
 claims[dev][num]=true;channels+=intf->bNumEndpoints;return ESP_OK;
}
static int usb_host_interface_release(int client,int dev,int num) {
 (void)client;int offset=0;
 const usb_intf_desc_t *intf=usb_parse_interface_descriptor(&configs[dev],num,0,&offset);
 if(!intf || !claims[dev][num]) {invalid++;return ESP_ERR_INVALID_STATE;}
 if(num==release_fail) return ESP_ERR_INVALID_STATE;
 claims[dev][num]=false;channels-=intf->bNumEndpoints;return ESP_OK;
}
static void reset_host_endpoint(int dev,uint8_t ep) {(void)dev;reset_eps[resets++]=ep;}
'''

TESTS = r'''
static int cases,failures;
static const char busid_a[32]="a";
#define CHECK(x) do { if(!(x)){printf("FAIL %d: %s\n",__LINE__,#x);failures++;} }while(0)
static void reset(void) {
 memset(&s_state,0,sizeof(s_state));memset(configs,0,sizeof(configs));memset(claims,0,sizeof(claims));
 channels=8;invalid=claim_calls=resets=0;claim_fail=release_fail=-1;config_error=config_null=in_flight=false;
 for(int d=0;d<2;d++) {
  s_state.devices[d].in_use=true;s_state.devices[d].dev_hdl=d;s_state.devices[d].device.num_interfaces=4;
  strcpy(s_state.devices[d].device.busid,d?"b":"a");configs[d].wTotalLength=100;
  for(int i=0;i<17;i++) configs[d].intf[i].number=-1;
  for(int i=0;i<4;i++) {
   usb_intf_desc_t *p=&configs[d].intf[i];p->number=i;p->offset=i;p->bNumEndpoints=i?2:1;
   p->ep[0].bEndpointAddress=0x81+i;if(i)p->ep[1].bEndpointAddress=1+i;
  }
 }
}
static void request(int recipient,int req,int index,int value) {
 xfer_t x={.setup={recipient,req,value,index},.busid="a"};apply_standard_request_to_host(&x);
}
int main(void) {
 for(int first=0;first<2;first++) {
  reset();cases++;
  CHECK(ensure_endpoint_interface_claimed_locked(first,0x81)==ESP_OK);
  CHECK(ensure_endpoint_interface_claimed_locked(first,0x82)==ESP_OK);
  CHECK(ensure_endpoint_interface_claimed_locked(1-first,0x81)==ESP_OK);
  CHECK(ensure_endpoint_interface_claimed_locked(1-first,0x82)==ESP_OK);
  CHECK(channels==14);CHECK(claim_calls==4);
  CHECK(ensure_endpoint_interface_claimed_locked(first,2)==ESP_OK);CHECK(claim_calls==4);
  CHECK(!claims[0][2]&&!claims[0][3]&&!claims[1][2]&&!claims[1][3]);
  release_interfaces_locked(first);CHECK(channels==11);
  CHECK(has_claimed_interfaces_locked(1-first));release_interfaces_locked(1-first);CHECK(channels==8);CHECK(invalid==0);
 }
 for(int fail=0;fail<4;fail++) {
  reset();cases++;claim_fail=fail;
  for(int retry=0;retry<3;retry++){CHECK(ensure_endpoint_interface_claimed_locked(0,0x81+fail)==ESP_ERR_NOT_SUPPORTED);CHECK(channels==8);}
  CHECK(s_state.devices[0].claim_failures==3);CHECK(s_state.exhaustion_events==3);
  CHECK(s_state.devices[0].last_claim_err==ESP_ERR_NOT_SUPPORTED);
  CHECK(strcmp(s_state.last_exhaustion_busid,"a")==0);CHECK(s_state.last_exhaustion_us!=0);
  CHECK(!has_claimed_interfaces_locked(0));claim_fail=-1;
  CHECK(ensure_endpoint_interface_claimed_locked(0,0x81+fail)==ESP_OK);
  release_interfaces_locked(0);CHECK(channels==8);CHECK(invalid==0);
 }
 for(int fail=0;fail<4;fail++) {
  reset();cases++;
  for(int i=0;i<4;i++) CHECK(ensure_endpoint_interface_claimed_locked(0,0x81+i)==ESP_OK);
  CHECK(channels==15);release_fail=fail;release_interfaces_locked(0);
  CHECK(has_claimed_interfaces_locked(0));CHECK(claims[0][fail]);
  int calls=claim_calls;CHECK(ensure_endpoint_interface_claimed_locked(0,0x81+fail)==ESP_OK);CHECK(claim_calls==calls);
  release_fail=-1;release_interfaces_locked(0);release_interfaces_locked(0);CHECK(channels==8);CHECK(invalid==0);
 }
 reset();cases++;
 CHECK(ensure_endpoint_interface_claimed_locked(0,0x82)==ESP_OK);
 claim_fail=2;CHECK(ensure_endpoint_interface_claimed_locked(0,0x83)==ESP_ERR_NOT_SUPPORTED);
 CHECK(claims[0][1] && channels==10);CHECK(!claims[0][2]);
 in_flight=true;release_session_device(busid_a);CHECK(s_state.devices[0].release_pending);CHECK(channels==10);
 in_flight=false;release_session_device(busid_a);CHECK(channels==8);CHECK(!has_claimed_interfaces_locked(0));
 reset();cases++;
 configs[0].intf[1].number=USBIP_MAX_INTERFACES-1;s_state.devices[0].device.num_interfaces=4;
 CHECK(find_endpoint_interface_locked(0,0x82)==USBIP_MAX_INTERFACES-1);
 configs[0].intf[1].ep[0].wMaxPacketSize=512;
 CHECK(get_endpoint_mps_locked(0,0x82)==512);
 configs[0].intf[1].ep[1].wMaxPacketSize=256;
 CHECK(get_endpoint_mps_locked(0,2)==256);CHECK(invalid==0);
 CHECK(ensure_endpoint_interface_claimed_locked(0,0x82)==ESP_OK);CHECK(claims[0][USBIP_MAX_INTERFACES-1]);
 uint8_t eps[16];CHECK(collect_claimed_endpoints_locked(0,-1,eps,16)==2);
 release_interfaces_locked(0);CHECK(!claims[0][USBIP_MAX_INTERFACES-1]);CHECK(channels==8);
 reset();cases++;configs[0].intf[1].alternate=1;
 CHECK(ensure_endpoint_interface_claimed_locked(0,0x82)==ESP_ERR_NOT_FOUND);CHECK(claim_calls==0);
 reset();cases++;configs[0].intf[1].number=USBIP_MAX_INTERFACES;
 CHECK(ensure_endpoint_interface_claimed_locked(0,0x82)==ESP_ERR_NOT_FOUND);CHECK(claim_calls==0);
 reset();cases++;
 CHECK(ensure_endpoint_interface_claimed_locked(-1,0x81)==ESP_ERR_INVALID_STATE);
 CHECK(ensure_endpoint_interface_claimed_locked(2,0x81)==ESP_ERR_INVALID_STATE);
 s_state.devices[0].in_use=false;CHECK(ensure_endpoint_interface_claimed_locked(0,0x81)==ESP_ERR_INVALID_STATE);
 CHECK(!has_claimed_interfaces_locked(-1));CHECK(!has_claimed_interfaces_locked(2));CHECK(!has_claimed_interfaces_locked(0));
 release_interfaces_locked(-1);release_interfaces_locked(2);release_interfaces_locked(0);CHECK(invalid==0);
 reset();cases++;
 CHECK(find_endpoint_interface_locked(0,0)==-1);CHECK(find_endpoint_interface_locked(0,0x80)==-1);
 CHECK(ensure_endpoint_interface_claimed_locked(0,0x8f)==ESP_ERR_NOT_FOUND);CHECK(claim_calls==0);
 config_error=true;CHECK(ensure_endpoint_interface_claimed_locked(0,0x82)==ESP_ERR_NOT_FOUND);
 config_error=false;config_null=true;CHECK(ensure_endpoint_interface_claimed_locked(0,0x82)==ESP_ERR_NOT_FOUND);CHECK(claim_calls==0);
 reset();cases++;
 request(0,9,0,1);request(1,11,1,0);request(2,1,0x82,0);CHECK(resets==0 && claim_calls==0);
 CHECK(ensure_endpoint_interface_claimed_locked(0,0x82)==ESP_OK);
 s_state.devices[0].halted_eps=ep_halt_bit(0x82)|ep_halt_bit(2);
 request(2,1,0x83,0);CHECK(resets==0);
 request(2,1,0x82,0);CHECK(resets==1 && reset_eps[0]==0x82);
 CHECK(!(s_state.devices[0].halted_eps&ep_halt_bit(0x82)));
 request(1,11,2,0);CHECK(resets==1);
 request(1,11,1,0);CHECK(resets==3);CHECK(s_state.devices[0].halted_eps==0);
 request(0,9,0,1);CHECK(resets==5);CHECK(claim_calls==1);
 CHECK(collect_claimed_endpoints_locked(0,-1,eps,1)==1);
 CHECK(collect_claimed_endpoints_locked(0,-1,eps,0)==0);
 CHECK(collect_claimed_endpoints_locked(0,2,eps,16)==0);
 CHECK(invalid==0);
 printf("%d groups, %d failed assertions\n",cases,failures);return failures?1:0;
}
'''


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1]/'main/usb_backend.c')
    parser.add_argument('--cc',default='cc')
    args=parser.parse_args()
    source=args.source.read_text()
    assert 'ensure_endpoint_interface_claimed_locked(dev_slot, pipe->endpoint_addr)' in source
    names=['has_claimed_interfaces_locked','release_interfaces_locked','find_endpoint_interface_locked',
           'ensure_endpoint_interface_claimed_locked','release_session_device','ep_halt_bit',
           'collect_claimed_endpoints_locked','apply_standard_request_to_host','get_endpoint_mps_locked']
    program=PREAMBLE+'\n'.join(function(source,n) for n in names)+TESTS
    with tempfile.TemporaryDirectory(prefix='usb-demand-') as directory:
        root=Path(directory);path=root/'demand.c';path.write_text(program)
        subprocess.run([args.cc,'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',str(path),'-o',str(root/'demand')],check=True)
        return subprocess.run([str(root/'demand')]).returncode


if __name__=='__main__':
    raise SystemExit(main())
