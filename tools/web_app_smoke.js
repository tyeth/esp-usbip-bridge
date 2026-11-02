// Run: node tools/web_app_smoke.js main/web_app.js
// Smoke test: load web_app.js with a stub DOM/XHR, feed canned API answers,
// render the rescue panel for a few device kinds, and report JS errors.
const fs = require('fs');
const vm = require('vm');
const src = fs.readFileSync(process.argv[2], 'utf8');

const els = {};
function el(id) {
  if (!els[id]) {
    els[id] = { id, innerHTML: '', textContent: '', value: '', checked: false, className: '', style: {},
      files: [], scrollTop: 0, scrollHeight: 0, clientHeight: 0, contains: () => false, focus() {}, select() {} };
  }
  return els[id];
}
const answers = {};
class XHR {
  open(m, url) { this.m = m; this.url = url; this.upload = {}; }
  setRequestHeader() {}
  send(body) {
    const key = this.m + ' ' + this.url.split('?')[0];
    const a = answers[key];
    this.status = a ? a[0] : 404;
    this.responseText = JSON.stringify(a ? a[1] : { error: 'stub: ' + key });
    if (this.onload) this.onload();
  }
}
const sandbox = {
  document: { getElementById: el, querySelectorAll: () => [], activeElement: null, body: { appendChild() {} },
              createElement: () => ({ style: {}, click() {}, remove() {} }) },
  XMLHttpRequest: XHR, localStorage: { getItem: () => '', setItem() {} },
  location: { hash: '' }, history: { replaceState() {} }, setTimeout: () => 0, clearTimeout() {}, setInterval() {},
  alert: (m) => console.log('ALERT', m), confirm: () => true, prompt: () => null, console, JSON, Math, String, Array,
  URL: { createObjectURL() {}, revokeObjectURL() {} }, Blob: function () {}, encodeURIComponent, decodeURIComponent,
  parseInt, parseFloat, window: {},
};
vm.createContext(sandbox);
vm.runInContext(src, sandbox);

answers['GET /api/rescue/files'] = [200, { files: [{ name: 'app.bin', size: 1000, age_s: 1, in_use: false }], used: 1000, space: 9000000 }];
answers['GET /api/capture'] = [200, { total_limit: 2097152, default_size: 262144, reserved: 0, captures: [], armed: [] }];
answers['GET /api/status'] = [200, { active: null, duts: [{ name: 'feather-s3', group: 'g', channel: 1, active: false }], groups: [] }];
answers['GET /api/usb/devices'] = [200, { devices: [] }];

const base = { ok: true, busid: '1-1.2', vid: '303a', pid: '1001', bcd_device: '0101', device_class: 'ef',
  manufacturer: 'Espressif', product: 'USB JTAG/serial debug unit', serial: 'AA', name: 'feather-s3', speed: 'full',
  address: 3, configuration: 1, interfaces: [{ number: 0, class: '02', subclass: '02', protocol: '00', class_name: 'cdc',
  num_endpoints: 1, intr_in: '82' }], owner: null, imported: false, monitor: false, job_running: false,
  port: { path: '1-1.2', power: 'on', connected: true, over_current: false, saved: 'on', read_only: true },
  serial: { kind: 'cdc_acm', ports: 1, control_interface: 0, data_interface: 1, bulk_in: '81', bulk_out: '01',
  esp_usb_jtag: true, last_set: { baud: null, dtr: null, rts: null } }, rp: { reset_interface: -1, picoboot_interface: -1, chip: 'unknown' },
  modes: [{ name: 'esp_usb_jtag_bootloader', description: 'x' }], bootloader_mode: 'esp_usb_jtag_bootloader',
  console: true, flashers: ['esp', 'samba'], mass_storage: false, limits: { capture_s_max: 10, session_s_max: 12, max_bytes: 16384 } };

const cases = {
  esp: base,
  imported: Object.assign({}, base, { owner: { kind: 'usbip', peer: '10.0.0.2' }, imported: true, monitor: true }),
  bootsel: Object.assign({}, base, { vid: '2e8a', pid: '0003', serial: { kind: 'none', ports: 0 }, console: false,
    rp: { reset_interface: -1, picoboot_interface: 1, chip: 'rp2040' }, mass_storage: true, flashers: ['picoboot', 'uf2'],
    modes: [{ name: 'picoboot_reboot', description: 'y' }], bootloader_mode: null }),
};
let failed = 0;
for (const [name, info] of Object.entries(cases)) {
  answers['GET /api/usb/devices/1-1.2/rescue'] = [200, info];
  answers['GET /api/usb/devices/1-1.2/monitor'] = [200, { ok: true, state: 'running', running: false, next: 10, text: 'hello', dropped: false, reconnects: 0 }];
  try {
    vm.runInContext("rescueSel('1-1.2')", sandbox);
    const h = el('rinfo').innerHTML;
    const want = { esp: ['Flash ESP', 'Serial capture', 'Staged files', 'esp_usb_jtag_bootloader'],
                   imported: ['Imported over USB/IP', 'disabled'], bootsel: ['Load', 'Mass storage', 'Copy UF2'] }[name];
    for (const w of want) {
      if (!h.includes(w)) { console.log(`FAIL ${name}: missing "${w}"`); failed++; }
    }
    if (name === 'bootsel' && h.includes('Flash ESP')) { console.log('FAIL bootsel shows the ESP flasher'); failed++; }
    if (!el('rfiles').innerHTML.includes('app.bin')) { console.log(`FAIL ${name}: staged files not listed`); failed++; }
  } catch (e) {
    console.log(`FAIL ${name}: ${e.stack}`);
    failed++;
  }
}
// job progress rendering
answers['POST /api/usb/devices/1-1.2/flash/esp'] = [202, { ok: true, job: 7 }];
answers['GET /api/rescue/jobs/7'] = [200, { id: 7, kind: 'esp_flash', state: 'done', progress: 100, stage: '', elapsed_s: 3.2,
  error: null, result: { chip: 'ESP32-S3' }, log: '0.00 esp_flash on 1-1.2\n' }];
try {
  answers['GET /api/usb/devices/1-1.2/rescue'] = [200, base];
  vm.runInContext("rescueSel('1-1.2'); rEspImgs=[{file:'app.bin',offset:'0x0'}]; rEspFlash();", sandbox);
  if (!el('rjobl').textContent.includes('ESP32-S3')) { console.log('FAIL job log not rendered'); failed++; }
} catch (e) { console.log('FAIL job: ' + e.stack); failed++; }
// device flash: map, browse and list through a session job
answers['POST /api/usb/devices/1-1.2/dutflash'] = [202, { ok: true, job: 9, added_to_open_session: false, queued_ops: 0 }];
answers['GET /api/rescue/jobs/9'] = [200, { id: 9, kind: 'dut_flash', state: 'running', progress: 50, stage: 'ready', elapsed_s: 4,
  error: null, log: '', result: { device: { platform: 'esp', chip: 'ESP32-C6', flash_size: 4194304, writable: true },
  notes: ['no flasher stub'], regions: [
    { index: 0, name: 'flash', type: 'flash', offset: 0, size: 4194304, protected: true },
    { index: 1, name: 'app0', type: 'app:ota_0', offset: 65536, size: 1966080, protected: true },
    { index: 2, name: 'spiffs', type: 'data:spiffs', offset: 3997696, size: 131072, protected: false, fs: 'littlefs',
      fs_version: '2.0', block_size: 4096, block_count: 32 }],
  ops: [{ index: 0, op: 'fs_info', ok: true, status: 200, part: 'spiffs', fs: 'littlefs', total: 131072, free: 122880,
          path: '/', files: [{ name: 'secrets.json', dir: false, size: 30 }, { name: 'lib', dir: true, size: 0 }] }] } }];
try {
  answers['GET /api/usb/devices/1-1.2/rescue'] = [200, base];
  vm.runInContext("rescueSel('1-1.2'); rDfRun([]);", sandbox);
  if (!el('rinfo').innerHTML.includes('Device flash')) { console.log('FAIL device flash panel missing'); failed++; }
  const m = el('rdfmap').innerHTML;
  for (const w of ['ESP32-C6', 'app:ota_0', 'littlefs 2.0', 'Browse', 'Format', 'no flasher stub']) {
    if (!m.includes(w)) { console.log(`FAIL device flash map: missing "${w}"`); failed++; }
  }
  vm.runInContext("rDfBrowse('spiffs');", sandbox);
  const l = el('rdflist').innerHTML;
  if (!l.includes('secrets.json') || !l.includes('lib/')) { console.log('FAIL device flash listing'); failed++; }
  if (!el('rdfinfo').textContent.includes('120 KB free')) { console.log('FAIL device flash usage: ' + el('rdfinfo').textContent); failed++; }
} catch (e) { console.log('FAIL device flash: ' + e.stack); failed++; }
console.log(failed ? `${failed} failure(s)` : 'ui smoke: all passed');
process.exit(failed ? 1 : 0);
