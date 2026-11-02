/*
 * USB/IP Bridge — browser-side script for the status page.
 *
 * This file is embedded verbatim into the served HTML (see
 * EMBED_TXTFILES in main/CMakeLists.txt and stream_html_page() in
 * http_server.c).  Keep it self-contained: it must run in a plain
 * <script> tag with no module loader and no build step.
 *
 * Validate syntax with:  node --check main/web_app.js
 */
'use strict';

/* Refresh all GPIO pin values, wire labels, directions and pull states. */
function rp() {
  var x = new XMLHttpRequest();
  x.open('GET', '/api/pins', true);
  x.onload = function () {
    if (x.status != 200) return;
    var r = JSON.parse(x.responseText);
    r.pins.forEach(function (p) {
      var e = document.getElementById('v' + p.name);
      if (e) {
        e.textContent = p.value ? 'HIGH' : 'LOW';
        e.className = p.value ? 'hi' : 'lo';
      }
      var w = document.getElementById('w' + p.name);
      if (w) {
        w.value = p.wire;
        w.onchange = function () { sw(this); };
      }
      var d = document.getElementById('d' + p.name);
      if (d) { d.value = p.dir; }
    });
    if (r.pulls) {
      r.pulls.forEach(function (p) {
        var s = document.getElementById('pl' + p.header);
        if (s) { s.value = p.state; }
      });
    }
  };
  x.send();
}

/* Set a header-pin pull resistor via its IO-expander line. */
function spul(sel) {
  var exp = sel.getAttribute('data-exp');
  var v = sel.value;
  var x = new XMLHttpRequest();
  x.open('POST', '/api/pins/' + exp, true);
  x.setRequestHeader('Content-Type', 'application/json');
  x.onload = function () { if (x.status == 200) { rp(); } };
  var b;
  if (v == 'up') { b = '{"direction":"out","value":1}'; }
  else if (v == 'down') { b = '{"direction":"out","value":0}'; }
  else { b = '{"direction":"in"}'; }
  x.send(b);
}

/* Switch between the GPIO-pins and USB-devices tabs. */
function st(n) {
  ['p', 'u', 'h', 'm', 'r'].forEach(function (x) {
    document.getElementById('t' + x).className = '';
    document.getElementById('s' + x).className = 'sec';
  });
  document.getElementById('t' + n).className = 'on';
  document.getElementById('s' + n).className = 'sec on';
}

/* Change a pin's direction (IN / OUT). */
function sd(p) {
  var d = document.getElementById('d' + p).value;
  var x = new XMLHttpRequest();
  x.open('POST', '/api/pins/' + p, true);
  x.setRequestHeader('Content-Type', 'application/json');
  x.onload = function () { if (x.status == 200) { rp(); } };
  x.send(JSON.stringify({ direction: d }));
}

/* Toggle a pin's output level. */
function tg(p) {
  var x = new XMLHttpRequest();
  x.onload = function () { if (x.status == 200) { rp(); } };
  x.open('POST', '/api/pins/' + p + '/toggle', true);
  x.send();
}

/* Read a single pin and update its cell. */
function rr(p) {
  var x = new XMLHttpRequest();
  x.open('GET', '/api/pins/' + p, true);
  x.onload = function () {
    if (x.status != 200) return;
    var r = JSON.parse(x.responseText);
    var e = document.getElementById('v' + p);
    if (e) {
      e.textContent = r.value ? 'HIGH' : 'LOW';
      e.className = r.value ? 'hi' : 'lo';
    }
  };
  x.send();
}

/* Save a DUT wire name typed into a pin row.  Updates only the saved
   pin's value display from the response instead of calling rp(), which
   would overwrite every wire input field and could clear a name the user
   is still typing in another row. */
function sw(elem) {
  var p = elem.id.replace(/^w/, '');
  var x = new XMLHttpRequest();
  x.open('POST', '/api/pins/' + p, true);
  x.setRequestHeader('Content-Type', 'application/json');
  x.onload = function () {
    if (x.status == 200) {
      var r = JSON.parse(x.responseText);
      var e = document.getElementById('v' + p);
      if (e) {
        e.textContent = r.value ? 'HIGH' : 'LOW';
        e.className = r.value ? 'hi' : 'lo';
      }
    }
  };
  x.send(JSON.stringify({ wire: elem.value }));
}

/* Hostname editing inline UI. */
function he() {
  var h = document.getElementById('hostname');
  var inp = document.getElementById('hn-input');
  inp.value = h.textContent;
  h.style.display = 'none';
  inp.style.display = 'inline';
  document.getElementById('hn-edit').style.display = 'none';
  document.getElementById('hn-save').style.display = 'inline';
  document.getElementById('hn-cancel').style.display = 'inline';
  inp.focus();
  inp.select();
}

function hs() {
  var inp = document.getElementById('hn-input');
  var x = new XMLHttpRequest();
  x.open('POST', '/api/hostname', true);
  x.setRequestHeader('Content-Type', 'application/json');
  x.onload = function () {
    if (x.status == 200) {
      document.getElementById('hostname').textContent = inp.value;
      hc();
    }
  };
  x.send(JSON.stringify({ hostname: inp.value }));
}

function hc() {
  document.getElementById('hostname').style.display = 'inline';
  document.getElementById('hn-input').style.display = 'none';
  document.getElementById('hn-edit').style.display = 'inline';
  document.getElementById('hn-save').style.display = 'none';
  document.getElementById('hn-cancel').style.display = 'none';
}

/* Board ID editing inline UI. */
function be() {
  var h = document.getElementById('board_id');
  var inp = document.getElementById('bi-input');
  inp.value = h.textContent;
  h.style.display = 'none';
  inp.style.display = 'inline';
  document.getElementById('bi-edit').style.display = 'none';
  document.getElementById('bi-save').style.display = 'inline';
  document.getElementById('bi-cancel').style.display = 'inline';
  inp.focus();
  inp.select();
}

function bs() {
  var inp = document.getElementById('bi-input');
  var x = new XMLHttpRequest();
  x.open('POST', '/api/board_id', true);
  x.setRequestHeader('Content-Type', 'application/json');
  x.onload = function () {
    if (x.status == 200) {
      document.getElementById('board_id').textContent = inp.value;
      bc();
    }
  };
  x.send(JSON.stringify({ board_id: inp.value }));
}

function bc() {
  document.getElementById('board_id').style.display = 'inline';
  document.getElementById('bi-input').style.display = 'none';
  document.getElementById('bi-edit').style.display = 'inline';
  document.getElementById('bi-save').style.display = 'none';
  document.getElementById('bi-cancel').style.display = 'none';
}

/* Config export: GET /api/config and download as <hostname>-config.json. */
function cfgExport() {
  var x = new XMLHttpRequest();
  x.open('GET', '/api/config', true);
  x.onload = function () {
    if (x.status != 200) return;
    var b = new Blob([x.responseText], { type: 'application/json' });
    var a = document.createElement('a');
    a.href = URL.createObjectURL(b);
    var hn = 'usbip';
    try { hn = JSON.parse(x.responseText).hostname || 'usbip'; } catch (e) {}
    a.download = hn + '-config.json';
    document.body.appendChild(a);
    a.click();
    setTimeout(function () { URL.revokeObjectURL(a.href); a.remove(); }, 0);
  };
  x.send();
}

/* Config import: create a temporary file input, click it, and POST
   the selected file to /api/config. */
function cfgImport() {
  var inp = document.createElement('input');
  inp.type = 'file';
  inp.accept = 'application/json,.json';
  inp.style.display = 'none';
  inp.onchange = function () {
    if (!inp.files || !inp.files[0]) return;
    var f = inp.files[0];
    var fr = new FileReader();
    fr.onload = function () {
      var x = new XMLHttpRequest();
      x.open('POST', '/api/config', true);
      x.setRequestHeader('Content-Type', 'application/json');
      x.onload = function () {
        if (x.status != 200) {
          alert('Import failed: HTTP ' + x.status);
          return;
        }
        var r;
        try { r = JSON.parse(x.responseText); } catch (e) {}
        if (r && r.error) {
          alert('Import failed: ' + r.error);
          return;
        }
        var msg = 'Config imported.';
        if (r) {
          msg = 'Config imported (' + r.applied + ' wires applied, ' +
                r.skipped + ' skipped).';
        }
        alert(msg + ' Reloading...');
        location.reload();
      };
      x.send(fr.result);
    };
    fr.readAsText(f);
  };
  document.body.appendChild(inp);
  inp.click();
  setTimeout(function () { inp.remove(); }, 100);
}

/* Hold-to-confirm clear config: user must hold the button for 3 seconds. */
var cfgClearTimer = null;
var cfgClearStartTime = 0;
var cfgClearHeld = false;
var CLEAR_HOLD_MS = 3000;

function cfgClearStart(e) {
  if (cfgClearTimer) return;
  /* Prevent default to avoid text selection etc. */
  e.preventDefault();
  cfgClearHeld = true;
  cfgClearStartTime = Date.now();
  var btn = document.getElementById('cfgClearBtn');
  if (btn) {
    btn.style.transition = 'none';
    btn.style.backgroundSize = '0% 100%';
    /* Force reflow so the 0-size takes effect before the transition starts. */
    btn.offsetHeight;
    btn.style.transition = 'background-size ' + CLEAR_HOLD_MS + 'ms linear';
    btn.style.backgroundSize = '100% 100%';
  }
  cfgClearTimer = setTimeout(function () {
    if (!cfgClearHeld) return;
    cfgClearHeld = false;
    cfgClearTimer = null;
    var x = new XMLHttpRequest();
    x.open('POST', '/api/config/clear', true);
    x.setRequestHeader('Content-Type', 'application/json');
    x.onload = function () {
      if (x.status == 200) {
        location.reload();
      } else {
        alert('Clear failed: HTTP ' + x.status);
        var btn2 = document.getElementById('cfgClearBtn');
        if (btn2) { btn2.style.transition = 'none'; btn2.style.backgroundSize = '0% 100%'; }
      }
    };
    x.onerror = function () {
      alert('Clear failed: network error');
      var btn2 = document.getElementById('cfgClearBtn');
      if (btn2) { btn2.style.transition = 'none'; btn2.style.backgroundSize = '0% 100%'; }
    };
    x.send();
  }, CLEAR_HOLD_MS);
}

function cfgClearCancel() {
  if (!cfgClearHeld) return;
  cfgClearHeld = false;
  if (cfgClearTimer) {
    clearTimeout(cfgClearTimer);
    cfgClearTimer = null;
  }
  var btn = document.getElementById('cfgClearBtn');
  if (btn) {
    btn.style.transition = 'none';
    btn.style.backgroundSize = '0% 100%';
  }
}

/* ---------------------------------------------------------------------
 *  API token: sent as a Bearer token on every non-GET request
 * ------------------------------------------------------------------- */
function tokGet() {
  try { return localStorage.getItem('usbipToken') || ''; } catch (e) { return ''; }
}

(function () {
  var open = XMLHttpRequest.prototype.open;
  var send = XMLHttpRequest.prototype.send;
  XMLHttpRequest.prototype.open = function (m) {
    this._m = m;
    return open.apply(this, arguments);
  };
  XMLHttpRequest.prototype.send = function () {
    var t = tokGet();
    if (t && this._m && this._m.toUpperCase() != 'GET') {
      this.setRequestHeader('Authorization', 'Bearer ' + t);
    }
    return send.apply(this, arguments);
  };
})();

function tokSave() {
  try { localStorage.setItem('usbipToken', document.getElementById('tok').value); } catch (e) {}
  authStatus();
}

function tokSet() {
  var v = document.getElementById('tok').value;
  if (!confirm(v ? 'Require this token for changes on the bridge?' : 'Clear the bridge token (no auth)?')) return;
  api('POST', '/api/auth/token', { token: v }, function (ok, r) {
    if (!ok) { alert('Failed: ' + (r && r.error)); return; }
    tokSave();
  });
}

function authStatus() {
  api('GET', '/api/auth', null, function (ok, r) {
    var e = document.getElementById('authst');
    if (ok && e) e.textContent = r.auth_required ? '(bridge requires a token)' : '(no token required)';
  });
}

/* -------------------------------------------------------------------
 *  Firmware update (OTA): PUT the app .bin to /api/ota
 * ------------------------------------------------------------------- */
function otaText(r) {
  if (!r.supported) return 'not available: this bridge has no OTA slots (needs the one wired migration flash)';
  var run = null;
  (r.partitions || []).forEach(function (p) { if (p.running) run = p; });
  var t = 'running ' + r.running + (run && run.state ? ' (' + run.state + ')' : '') + ', ' + r.fw_version +
          '; next update goes to ' + r.next_update;
  if (r.confirm && r.confirm.state == 'waiting') t += '; new image waiting to confirm itself (network ' +
      (r.confirm.network_up ? 'up' : 'down') + ', HTTP ' + (r.confirm.http_up ? 'up' : 'down') + ')';
  if (r.confirm && r.confirm.state == 'confirmed') t += '; new image confirmed after ' +
      Math.round(r.confirm.confirmed_after_ms / 1000) + ' s';
  if (r.last_invalid) t += '; ' + r.last_invalid + ' was rolled back';
  return t;
}

function otaStatus(cb) {
  api('GET', '/api/ota', null, function (ok, r) {
    var e = document.getElementById('otast');
    if (e) e.textContent = ok && r ? otaText(r) : 'unknown';
    if (cb) cb(ok, r);
  });
}

/* After a reboot: wait for the bridge to answer, then until the new image
   has confirmed itself (or rolled back). */
function otaWait(st, tries) {
  setTimeout(function () {
    otaStatus(function (ok, r) {
      if (ok && r && !(r.confirm && r.confirm.state == 'waiting')) {
        st.textContent = 'back up on ' + r.running + (r.last_invalid ? ' (' + r.last_invalid + ' rolled back)' : '');
        return;
      }
      st.textContent = ok ? 'up, confirming the new image ...' : 'rebooting ...';
      if (tries > 0) otaWait(st, tries - 1);
    });
  }, 3000);
}

function otaUpload() {
  var f = document.getElementById('otafile').files[0];
  if (!f) { alert('Pick an app .bin (build/<project>.bin, not the bootloader or the partition table)'); return; }
  var reboot = document.getElementById('otareboot').checked;
  if (!confirm('Write ' + f.name + ' (' + f.size + ' bytes) to the next OTA slot' +
               (reboot ? ' and reboot into it?' : '?'))) return;
  var st = document.getElementById('otaprog');
  var x = new XMLHttpRequest();
  x.open('PUT', '/api/ota' + (reboot ? '?reboot=1' : ''), true);
  x.upload.onprogress = function (e) { if (e.lengthComputable) st.textContent = 'writing ' + Math.round(100 * e.loaded / e.total) + '%'; };
  x.onload = function () {
    var r = null;
    try { r = JSON.parse(x.responseText); } catch (e) {}
    if (x.status != 200 || !r) { st.textContent = (r && r.error) || ('HTTP ' + x.status); return; }
    st.textContent = 'wrote ' + r.partition + ' (' + r.version + ', ' + r.date + ' ' + r.time + ', ' + r.ms + ' ms)' +
                     (r.rebooting ? '; rebooting ...' : '; boots on the next restart');
    if (r.rebooting) otaWait(st, 60);
    else otaStatus();
  };
  x.onerror = function () { st.textContent = 'network error'; };
  x.send(f);
}

/* -------------------------------------------------------------------
 *  Network: interfaces and the Wi-Fi station (GET /api/network,
 *  PUT / DELETE /api/network/wifi).  The password is never read back.
 * ------------------------------------------------------------------- */
function netIfText(i) {
  var t = i.name + ' ' + (i.ip ? i.ip : (i.up ? 'up, no address' : 'down'));
  if (i.name == 'wifi') {
    if (!i.configured) t += ' (not configured)';
    else {
      t += ' "' + i.ssid + '"';
      if (i.rssi !== null && i.rssi !== undefined) t += ' ' + i.rssi + ' dBm';
      if (i.state && i.state != 'connected') t += ' ' + i.state + (i.last_reason ? ' (' + i.last_reason + ')' : '');
      if (i.stored === false) t += ', not stored yet';
    }
  }
  return t;
}

function netShow(n) {
  var e = document.getElementById('netst');
  if (!n || !n.interfaces) { if (e) e.textContent = 'unknown'; return; }
  if (e) e.textContent = n.interfaces.map(netIfText).join('; ');
  var wifi = null;
  n.interfaces.forEach(function (i) { if (i.name == 'wifi') wifi = i; });
  /* The form only where the station can be configured (wifi_config) */
  var f = document.getElementById('netwifi');
  if (f) f.style.display = wifi && wifi.state !== undefined ? '' : 'none';
  var s = document.getElementById('netssid');
  if (s && wifi && wifi.ssid && !s.value && document.activeElement != s) s.value = wifi.ssid;
}

function netLoad() {
  api('GET', '/api/network', null, function (ok, r) { netShow(ok ? r : null); });
}

function netSet() {
  var ssid = document.getElementById('netssid').value;
  var pass = document.getElementById('netpass').value;
  if (!ssid) { alert('Enter the SSID'); return; }
  if (pass && (pass.length < 8 || pass.length > 63)) { alert('A WPA password is 8-63 characters (empty for an open network)'); return; }
  var st = document.getElementById('netprog');
  st.textContent = 'connecting ...';
  api('PUT', '/api/network/wifi', { ssid: ssid, password: pass, store: document.getElementById('netstore').checked },
      function (ok, r, code) {
        document.getElementById('netpass').value = '';
        if (!ok) { st.textContent = (r && r.error) || ('HTTP ' + code); return; }
        st.textContent = r.connected ? 'connected' + (r.stored ? ', stored' : '')
                                     : 'still connecting (stored once it connects' + (r.stored ? '; stored already' : '') + ')';
        netShow(r.network);
        if (!r.connected) setTimeout(netLoad, 5000);
      });
}

function netClear() {
  if (!confirm('Forget the Wi-Fi credentials and disconnect Wi-Fi?')) return;
  api('DELETE', '/api/network/wifi', null, function (ok, r) {
    document.getElementById('netprog').textContent = ok ? 'forgotten' : ((r && r.error) || 'failed');
    document.getElementById('netssid').value = '';
    if (ok) netShow(r.network);
  });
}

/* JSON request helper: cb(ok, body) */
function api(method, path, body, cb) {
  var x = new XMLHttpRequest();
  x.open(method, path, true);
  x.setRequestHeader('Content-Type', 'application/json');
  x.onload = function () {
    var r = null;
    try { r = JSON.parse(x.responseText); } catch (e) {}
    if (cb) cb(x.status >= 200 && x.status < 300, r, x.status);
  };
  x.onerror = function () { if (cb) cb(false, { error: 'network error' }, 0); };
  x.send(body === null || body === undefined ? null : JSON.stringify(body));
}

function esc(s) {
  return String(s === null || s === undefined ? '' : s).replace(/[&<>"']/g, function (c) {
    return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
  });
}

/* ---------------------------------------------------------------------
 *  USB devices and hub port power
 * ------------------------------------------------------------------- */
function forceOn() {
  var f = document.getElementById('force');
  return !!(f && f.checked);
}

function pw(port, action) {
  var body = { force: forceOn() };
  if (action == 'cycle') body.off_ms = 1000;
  api('POST', '/api/ports/' + encodeURIComponent(port) + '/' + action, body, function (ok, r) {
    if (!ok) alert(port + ' ' + action + ': ' + (r && r.error));
    setTimeout(refreshUsb, action == 'cycle' ? 2500 : 600);
  });
}

function allPorts(action) {
  if (!confirm('Switch ' + action + ' every per-port switched hub port?')) return;
  api('POST', '/api/ports/' + action, null, function (ok, r) {
    if (!ok) alert(r && r.error);
    setTimeout(refreshUsb, 1000);
  });
}

function powerButtons(port) {
  var p = esc(port);
  return '<button onclick="pw(\'' + p + '\',\'on\')">On</button>' +
         '<button onclick="pw(\'' + p + '\',\'off\')">Off</button>' +
         '<button onclick="pw(\'' + p + '\',\'cycle\')">Cycle</button>';
}

function loadDevs() {
  api('GET', '/api/usb/devices', null, function (ok, r) {
    var tb = document.getElementById('ud');
    if (!tb || !ok) return;
    var h = '';
    r.devices.forEach(function (d) {
      var pwr = d.virtual ? '<span class="muted">virtual</span>'
              : d.hub ? (esc(d.port_power_status || '?') + ' ' + powerButtons(d.busid))
              : '<span class="muted">root port</span>';
      var rb = d.virtual ? '' : ' <button onclick="rescueOpen(\'' + esc(d.busid) + '\')">Rescue</button>' +
               (d.imported ? ' <span class="warn" title="imported over USB/IP">imported</span>' : '');
      h += '<tr><td><strong>' + esc(d.busid) + '</strong>' + rb + '</td>' +
           '<td>' + esc(d.name) + '</td>' +
           '<td>' + esc(d.vid) + ':' + esc(d.pid) + '</td>' +
           '<td>' + esc(d.manufacturer) + ' ' + esc(d.product) + '</td>' +
           '<td>' + esc(d.serial) + '</td>' +
           '<td>' + esc(d.speed) + '</td>' +
           '<td>' + esc(d.max_power_ma) + '</td>' +
           '<td>' + pwr + '</td></tr>';
    });
    tb.innerHTML = h || '<tr><td colspan="8" class="muted">No USB devices</td></tr>';
  });
}

function showSettings(s) {
  if (!s) return;
  var e = document.getElementById('setEnf'), r = document.getElementById('setRst');
  if (e && document.activeElement !== e) e.checked = !!s.enforce_per_port_switching;
  if (r && document.activeElement !== r) r.checked = !!s.restore_port_power;
  var t = document.getElementById('setTo');
  if (t && document.activeElement !== t) t.value = s.enum_timeout_ms;
}

/* path null = global default; empty value removes a hub/port override */
function saveTimeout(path, value) {
  var body = { timeout_ms: value === '' ? null : parseInt(value, 10) };
  if (path) body.path = path;
  api('POST', '/api/settings/enum_timeout', body, function (ok, r) {
    if (!ok) alert('Enumeration timeout: ' + (r && r.error));
    else showSettings(r);
    setTimeout(refreshUsb, 200);
  });
}

function timeoutInput(path, ms, override) {
  return '<input class="txt" type="number" min="0" max="60000" style="width:70px"' +
         ' title="Enumeration timeout (ms); empty = inherit"' +
         (override ? ' value="' + esc(ms) + '"' : ' placeholder="' + esc(ms) + '"') +
         ' onchange="saveTimeout(\'' + esc(path) + '\', this.value)">';
}

function saveSettings() {
  api('POST', '/api/settings', {
    enforce_per_port_switching: document.getElementById('setEnf').checked,
    restore_port_power: document.getElementById('setRst').checked
  }, function (ok, r) {
    if (!ok) alert('Settings: ' + (r && r.error));
    showSettings(r);
  });
}

function restorePorts() {
  api('POST', '/api/ports/restore', null, function (ok, r) {
    document.getElementById('rstst').textContent = ok ? (r.ports + ' port(s) restored') : (r && r.error);
    setTimeout(refreshUsb, 1000);
  });
}

function hubChars(c) {
  if (!c) return '';
  return 'wHubCharacteristics ' + esc(c.raw) + ': power switching <strong' +
         (c.power_switching == 'per-port' ? '' : ' class="warn"') + '>' + esc(c.power_switching) + '</strong>, ' +
         'over-current ' + esc(c.over_current_protection) + ', ' + (c.compound ? 'compound, ' : '') +
         'TT think ' + esc(c.tt_think_time_fs_bits) + ' FS bits, ' +
         (c.port_indicators ? 'port indicators, ' : '') +
         'PwrOn2PwrGood ' + esc(c.pwr_on_to_pwr_good_ms) + ' ms, controller ' + esc(c.hub_contr_current_ma) + ' mA';
}

function loadHubs() {
  api('GET', '/api/usb/hubs', null, function (ok, r) {
    var el = document.getElementById('hubs');
    if (!el || !ok) return;
    showSettings(r.settings);
    if (!r.hubs.length) { el.innerHTML = 'No hubs attached'; return; }
    var h = '';
    r.hubs.forEach(function (hub) {
      h += '<div class="hub"><strong>' + esc(hub.path) + '</strong> ' + esc(hub.vid) + ':' + esc(hub.pid) +
           ' ' + esc(hub.product) + ' &mdash; ' + esc(hub.num_ports) + ' ports, enumeration timeout ' +
           timeoutInput(hub.path, hub.enum_timeout_ms, hub.enum_timeout_override) + ' ms<br>' +
           hubChars(hub.characteristics) + '</div>';
      h += '<table><thead><tr><th>Port</th><th>Power</th><th>Saved</th><th>Link</th><th>Speed</th><th>Attached</th><th>Enum timeout</th><th>Action</th></tr></thead><tbody>';
      hub.ports.forEach(function (p) {
        var dev = p.device ? (esc(p.device.type) + ' ' + esc(p.device.vid) + ':' + esc(p.device.pid)) : '';
        h += '<tr><td>' + esc(p.path) + '</td>' +
             '<td class="' + (p.power == 'on' ? 'hi' : 'lo') + '">' + esc(p.power) + (p.user_off ? ' (switched off)' : '') +
             (p.over_current ? ' <span class="lo">OVER-CURRENT</span>' : '') + '</td>' +
             '<td>' + esc(p.desired) + (p.mismatch ? ' <span class="warn">mismatch</span>' : '') + '</td>' +
             '<td>' + (p.connected ? 'connected' : '<span class="muted">-</span>') + '</td>' +
             '<td>' + esc(p.speed) + '</td><td>' + dev + '</td>' +
             '<td>' + timeoutInput(p.path, p.enum_timeout_ms, p.enum_timeout_override) + '</td>' +
             '<td>' + powerButtons(p.path) + '</td></tr>';
      });
      h += '</tbody></table>';
    });
    /* don't clobber an input being edited */
    if (el.contains(document.activeElement)) return;
    el.innerHTML = h;
  });
}

function refreshUsb() {
  loadDevs();
  loadHubs();
}

/* ---------------------------------------------------------------------
 *  I2C strand mux
 * ------------------------------------------------------------------- */
function muxErr(ok, r) {
  if (!ok) alert((r && r.error) || 'failed');
  loadMux();
}

function muxSel(dut) { api('POST', '/api/select', { dut: dut }, muxErr); }
function muxIso() { api('POST', '/api/isolate', null, muxErr); }
function muxCh(g, ch, closed) {
  api('POST', '/api/groups/' + encodeURIComponent(g) + '/channel', { channel: ch, closed: closed }, muxErr);
}
function muxGrpSel(g, ch) {
  api('POST', '/api/groups/' + encodeURIComponent(g) + '/select/' + ch, null, muxErr);
}

function loadMux() {
  api('GET', '/api/status', null, function (ok, r) {
    if (!ok) return;
    document.getElementById('mact').textContent = r.active || 'none (isolated)';
    document.getElementById('mman').textContent = r.manual ? '(manual)' : '';
    var h = '';
    r.duts.forEach(function (d) {
      h += '<button class="' + (d.active ? 'act' : '') + '" onclick="muxSel(\'' + esc(d.name) + '\')">' +
           esc(d.name) + ' <span class="muted">' + esc(d.group) + ':' + d.channel + '</span></button> ';
    });
    document.getElementById('mduts').innerHTML = h ? '<p class="sub">' + h + '</p>' : '<p class="sub muted">No DUTs in the topology</p>';
    var g = '';
    r.groups.forEach(function (grp) {
      g += '<div class="hub"><strong>' + esc(grp.name) + '</strong> ' + esc(grp.addresses.join(', ')) + '</div><p class="sub">';
      grp.channels.forEach(function (c) {
        g += '<label><input type="checkbox"' + (c.closed ? ' checked' : '') +
             ' onchange="muxCh(\'' + esc(grp.name) + '\',' + c.channel + ',this.checked)"> ch' + c.channel +
             (c.dut ? ' <span class="muted">' + esc(c.dut) + '</span>' : '') + '</label> ' +
             '<button onclick="muxGrpSel(\'' + esc(grp.name) + '\',' + c.channel + ')">only</button> &nbsp; ';
      });
      g += '</p>';
    });
    document.getElementById('mgroups').innerHTML = g;
  });
}

function muxProbe() {
  api('GET', '/api/probe', null, function (ok, r) {
    document.getElementById('mprobe').textContent = ok
      ? 'found: ' + (r.scan.join(', ') || 'nothing') : (r && r.error);
  });
}

function topoLoad() {
  api('GET', '/api/topology', null, function (ok, r) {
    if (ok) document.getElementById('mtopo').value = JSON.stringify(r, null, 2);
  });
}

function topoSave() {
  var t;
  try { t = JSON.parse(document.getElementById('mtopo').value); } catch (e) { alert('Invalid JSON: ' + e); return; }
  api('PUT', '/api/topology', t, function (ok, r) {
    document.getElementById('mtst').textContent = ok ? (r.saved ? 'saved' : ('applied, not saved: ' + r.note)) : (r && r.error);
    loadMux();
  });
}

/* ---------------------------------------------------------------------
 *  Device rescue: identity, serial lines, resets and a console peek run by
 *  the bridge itself (/api/usb/devices/{busid}/rescue|serial|reset|console).
 *  Nothing here polls: each request is a button press.  Port power is
 *  shown read-only.
 * ------------------------------------------------------------------- */
var rBusid = '';
var rInfo = null;
var rDuts = [];

function rescueOpen(busid) {
  st('r');
  rescueSel(busid);
}

function rescueDevList(sel) {
  api('GET', '/api/usb/devices', null, function (ok, r) {
    var e = document.getElementById('rdev');
    if (!e || !ok) return;
    var h = '<option value="">&mdash; pick a device &mdash;</option>';
    r.devices.forEach(function (d) {
      if (d.virtual) return;
      h += '<option value="' + esc(d.busid) + '"' + (d.busid == sel ? ' selected' : '') + '>' +
           esc(d.busid) + ' ' + esc(d.vid) + ':' + esc(d.pid) + ' ' + esc(d.product || d.manufacturer || '') +
           (d.name ? ' (' + esc(d.name) + ')' : '') + (d.imported ? ' [imported]' : '') + '</option>';
    });
    e.innerHTML = h;
  });
}

function rescueSel(busid) {
  rBusid = busid || '';
  try { history.replaceState(null, '', rBusid ? '#rescue=' + encodeURIComponent(rBusid) : '#'); } catch (e) {}
  rescueDevList(rBusid);
  rescueLoad();
}

function rescueLoad() {
  var el = document.getElementById('rinfo');
  if (!rBusid) { el.innerHTML = 'Pick a device'; return; }
  api('GET', '/api/usb/devices/' + encodeURIComponent(rBusid) + '/rescue', null, function (ok, r) {
    if (!ok) {
      rInfo = null;
      el.innerHTML = '<span class="lo">' + esc((r && r.error) || 'failed') + '</span>';
      return;
    }
    rInfo = r;
    /* mux DUT names, to offer routing the I2C strand to this device */
    api('GET', '/api/status', null, function (ok2, m) {
      rDuts = ok2 && m && m.duts ? m.duts : [];
      rescueRender();
    });
  });
}

function rKv(k, v) { return '<tr><td>' + esc(k) + '</td><td>' + v + '</td></tr>'; }
function rTri(v) { return v === null || v === undefined ? '?' : (v ? 'on' : 'off'); }
function rTriSel(id) {
  return '<select id="' + id + '"><option value="">leave</option><option value="1">on</option><option value="0">off</option></select>';
}

function rPortText(p) {
  if (!p) return '';
  if (p.power === null) return '<span class="muted">' + esc(p.note) + '</span>';
  return '<span class="' + (p.power == 'on' ? 'hi' : 'lo') + '">' + esc(p.power) + '</span>' +
         (p.over_current ? ' <span class="lo">OVER-CURRENT</span>' : '') +
         ' <span class="muted">(saved ' + esc(p.saved) + '; read-only here: port power is switched on the Hubs &amp; Power tab)</span>';
}

function rSerialText(s) {
  if (!s || s.kind == 'none') return '<span class="muted">none</span>';
  return esc(s.kind) + (s.esp_usb_jtag ? ' (ESP USB-Serial/JTAG)' : '') +
         ' &mdash; control if ' + s.control_interface + ', data if ' + s.data_interface +
         ' (bulk in ' + esc(s.bulk_in) + ', out ' + esc(s.bulk_out) + ')' + (s.ports > 1 ? ', ' + s.ports + ' ports' : '') +
         (s.baud_supported === false ? ' <span class="warn">baud not supported for this FTDI chip</span>' : '');
}

function rRpText(p) {
  if (!p || (p.reset_interface < 0 && p.picoboot_interface < 0)) return '<span class="muted">none</span>';
  return (p.reset_interface >= 0 ? 'reset interface ' + p.reset_interface + ' ' : '') +
         (p.picoboot_interface >= 0 ? 'PICOBOOT interface ' + p.picoboot_interface + ' (' + esc(p.chip) + ', in BOOTSEL)' : '');
}

function rescueRender() {
  var r = rInfo, el = document.getElementById('rinfo');
  if (!r || !el) return;
  var dis = r.owner ? ' disabled' : '';
  var h = '';
  if (r.owner) {
    h += '<p class="sub warn">' + (r.owner.kind == 'usbip'
      ? 'Imported over USB/IP by ' + esc(r.owner.peer) + ' &mdash; detach it (usbip detach) to use the actions below.'
      : 'Busy: ' + esc(r.owner.peer)) + '</p>';
  }
  h += '<table class="kv"><tbody>' +
       rKv('Device', esc(r.vid) + ':' + esc(r.pid) + ' ' + esc(r.manufacturer) + ' ' + esc(r.product)) +
       rKv('Serial number', r.serial_number ? esc(r.serial_number) : '<span class="muted">none</span>') +
       rKv('Name', esc(r.name)) +
       rKv('Speed / address', esc(r.speed) + ' / ' + esc(r.address)) +
       rKv('bcdDevice / class', esc(r.bcd_device) + ' / ' + esc(r.device_class)) +
       rKv('Port power', rPortText(r.port)) +
       rKv('Serial function', rSerialText(r.serial)) +
       rKv('RP2 interfaces', rRpText(r.rp)) +
       '</tbody></table>';
  h += '<table><thead><tr><th>If</th><th>Class</th><th>Subclass</th><th>Protocol</th><th>Endpoints</th></tr></thead><tbody>';
  r.interfaces.forEach(function (i) {
    var eps = [];
    if (i.bulk_in) eps.push('bulk in ' + i.bulk_in);
    if (i.bulk_out) eps.push('bulk out ' + i.bulk_out);
    if (i.intr_in) eps.push('intr in ' + i.intr_in);
    h += '<tr><td>' + i.number + '</td><td>' + esc(i['class']) + ' ' + esc(i.class_name) + '</td><td>' + esc(i.subclass) +
         '</td><td>' + esc(i.protocol) + '</td><td>' + esc(eps.join(', ')) + '</td></tr>';
  });
  h += '</tbody></table>';

  var dut = null;
  rDuts.forEach(function (d) { if (r.name && d.name == r.name) dut = d; });
  h += '<h2>I2C strand</h2><p class="sub">' + (dut
      ? 'Mux DUT <strong>' + esc(dut.name) + '</strong> (' + esc(dut.group) + ':' + dut.channel + ')' +
        (dut.active ? ' &mdash; connected' : '') +
        ' <button onclick="muxSel(\'' + esc(dut.name) + '\');setTimeout(rescueLoad,500)">Route strand here</button>'
      : '<span class="muted">No mux DUT is named after this device.</span>') +
       ' <a href="#" onclick="st(\'m\');return false" style="color:#e94560">Open the I2C strand mux</a></p>';

  if (r.serial && r.serial.kind != 'none') {
    var ls = r.serial.last_set || {};
    h += '<h2>Serial lines</h2><p class="sub">' +
         (r.serial.ports > 1 ? 'Port <input id="rport" class="txt" type="number" min="0" max="' + (r.serial.ports - 1) +
                               '" value="0" style="width:45px"> ' : '') +
         'Baud <input id="rbaud" class="txt" type="number" value="' + esc(ls.baud || 115200) + '" style="width:90px">' +
         ' <button onclick="rSetSerial(true,false)"' + dis + '>Set baud</button> &nbsp; ' +
         '<label><input type="checkbox" id="rdtr"' + (ls.dtr ? ' checked' : '') + '> DTR</label> ' +
         '<label><input type="checkbox" id="rrts"' + (ls.rts ? ' checked' : '') + '> RTS</label>' +
         ' <button onclick="rSetSerial(false,true)"' + dis + '>Apply DTR/RTS</button>' +
         ' <span class="muted">last set by the bridge: baud ' + esc(ls.baud === null || ls.baud === undefined ? '?' : ls.baud) +
         ', DTR ' + rTri(ls.dtr) + ', RTS ' + rTri(ls.rts) + '</span> <span id="rserst" class="muted"></span></p>';
  }

  if (r.modes.length) {
    h += '<h2>Reset / bootloader</h2><p class="sub">';
    if (r.bootloader_mode) {
      h += '<button class="act" onclick="rReset(\'bootloader\')"' + dis + '>Enter bootloader</button> <span class="muted">(' +
           esc(r.bootloader_mode) + ')</span><br>';
    }
    r.modes.forEach(function (m) {
      h += '<button onclick="rReset(\'' + esc(m.name) + '\')" title="' + esc(m.description) + '"' + dis + '>' + esc(m.name) + '</button> ';
    });
    h += '</p><pre id="rresout" class="con" style="display:none"></pre>';
  }

  if (r.console) {
    h += '<h2>Console peek</h2><p class="sub">Baud <input id="rcbaud" class="txt" type="number" value="115200" style="width:90px">' +
         ' Pokes <input id="rpokes" class="txt" value="enter" size="18" title="comma separated: enter, ctrl_c, ctrl_d, hex:0d0a">' +
         ' settle <input id="rsettle" class="txt" type="number" step="0.1" min="0" value="1" style="width:50px"> s' +
         ' capture <input id="rcap" class="txt" type="number" step="0.5" min="0" max="' + r.limits.capture_s_max +
         '" value="3" style="width:50px"> s' +
         ' DTR ' + rTriSel('rcdtr') + ' RTS ' + rTriSel('rcrts') +
         ' <button onclick="rConsole()"' + dis + '>Peek</button> <span id="rconst" class="muted"></span><br>' +
         '<span class="muted">DTR/RTS are left alone unless set here (some ESP USB-Serial/JTAG transitions reset the chip);' +
         ' TinyUSB CDC sends nothing until DTR is on. The bridge API is blocked while it captures.</span></p>' +
         '<pre id="rconout" class="con" style="display:none"></pre>';
  }

  if (r.console) {
    h += '<h2>Serial capture</h2><p class="sub">Baud <input id="rmbaud" class="txt" value="auto" size="14"' +
         ' title="a rate, rates tried in order (921600,115200), or auto; auto/lists only matter on UART bridges">' +
         ' buffer <input id="rmsize" class="txt" type="number" value="256" style="width:60px"> KB/port' +
         ' DTR ' + rTriSel('rmdtr') + ' RTS ' + rTriSel('rmrts') +
         ' <label><input type="checkbox" id="rmarm"> arm (always capture this port)</label>' +
         ' <button onclick="rMonStart()"' + dis + '>Start</button> <button onclick="rMonStop(false)">Stop</button>' +
         ' <button onclick="rMonStop(true)">Stop &amp; disarm</button><br>' +
         'Port <select id="rmport" onchange="rMonSwitch()"><option>0</option></select>' +
         ' <label><input type="checkbox" id="rmts" checked onchange="rMonSwitch()"> timestamps</label>' +
         ' <a id="rmdl" href="#" style="color:#e94560">download log</a>' +
         ' <span id="rmst" class="muted">' + (r.monitor ? 'running' : 'stopped') + '</span><br>' +
         '<span class="muted">Captured into the bridge\'s PSRAM in the background (every serial port of the device, through resets' +
         ' and re-enumeration, with [uptime s] timestamps); also live on TCP 3241: <code>echo "' + esc(r.busid) +
         ':0 from=epoch ts" | nc &lt;bridge&gt; 3241</code>. Reset buttons work while it runs; USB/IP imports are refused.</span></p>' +
         '<pre id="rmout" class="con" style="height:260px"></pre>' +
         '<p class="sub"><input id="rmin" class="txt" size="40" onkeydown="if(event.key==\'Enter\')rMonSend()">' +
         ' <label><input type="checkbox" id="rmnl" checked> + CR LF</label> <button onclick="rMonSend()">Send</button>' +
         ' <button onclick="rMonSendRaw(\'03\')">Ctrl-C</button> <button onclick="rMonSendRaw(\'04\')">Ctrl-D</button>' +
         ' <button onclick="document.getElementById(\'rmout\').textContent=\'\'">Clear</button></p>';
  }

  h += '<h2>Flash</h2><p class="sub">Stage the firmware below first (kept in the bridge\'s PSRAM until reboot).' +
       ' Each flash runs as a background job; the bridge API stays responsive.</p>' +
       '<div id="rjob" class="sub" style="display:none"><progress id="rjobp" max="100" value="0" style="width:200px"></progress>' +
       ' <span id="rjobs"></span> <button onclick="rJobCancel()">Cancel</button><pre id="rjobl" class="con"></pre></div>';
  if (r.serial && r.serial.kind != 'none') {
    h += '<p class="sub"><strong>ESP (esptool write-flash)</strong> <span class="muted">through ' + esc(r.serial.kind) +
         (r.serial.esp_usb_jtag ? ' USB-Serial/JTAG' : '') + '</span><br><span id="respimgs"></span>' +
         '<button onclick="rEspAddImg()">+ image</button> before <select id="respbefore"><option>default_reset</option>' +
         '<option>usb_reset</option><option>no_reset</option></select> after <select id="respafter"><option>hard_reset</option>' +
         '<option>no_reset</option></select> baud <input id="respbaud" class="txt" type="number" value="460800" style="width:80px">' +
         ' <label title="wipe the whole flash first: old firmware, NVS secrets, file systems"><input type="checkbox" id="resperase">' +
         ' erase all</label> <label><input type="checkbox" id="respverify" checked> verify</label>' +
         ' <label title="start a serial capture before the final reset, so the first boot is recorded">' +
         '<input type="checkbox" id="respcap" checked> capture first boot</label>' +
         ' <button class="act" onclick="rEspFlash()"' + dis + '>Flash ESP</button> <button onclick="rJob(\'esp/info\',{})"' + dis + '>Chip info</button></p>';
    h += '<p class="sub"><strong>SAM-BA (bossac)</strong> <span class="muted">SAMD21/SAMD51 .bin; offset blank = after the bootloader</span><br>' +
         rFileSel('rsbfile') + ' offset <input id="rsboff" class="txt" size="8" placeholder="auto">' +
         ' <label><input type="checkbox" id="rsbverify" checked> verify</label>' +
         ' <button class="act" onclick="rJob(\'flash/samba\',{file:rVal(\'rsbfile\'),offset:rVal(\'rsboff\')||undefined,' +
         'verify:document.getElementById(\'rsbverify\').checked})"' + dis + '>Flash SAM-BA</button>' +
         ' <button onclick="rJob(\'samba/info\',{})"' + dis + '>Bootloader info</button></p>';
  }
  if (r.rp && (r.rp.reset_interface >= 0 || r.rp.picoboot_interface >= 0 || r.vid == '2e8a' || r.console)) {
    h += '<p class="sub"><strong>RP2040/RP2350 (picotool load)</strong> <span class="muted">UF2, or .bin at an address; enters BOOTSEL first</span><br>' +
         rFileSel('rpbfile') + ' address (.bin) <input id="rpbaddr" class="txt" size="10" placeholder="0x10000000">' +
         ' <label><input type="checkbox" id="rpbverify" checked> verify</label>' +
         ' <label><input type="checkbox" id="rpberase"> erase all, flash size</label>' +
         ' <input id="rpbsize" class="txt" size="6" placeholder="2MB">' +
         ' <button class="act" onclick="rJob(\'flash/picoboot\',{file:rVal(\'rpbfile\'),address:rVal(\'rpbaddr\')||undefined,' +
         'verify:document.getElementById(\'rpbverify\').checked,erase_all:document.getElementById(\'rpberase\').checked,' +
         'flash_size:rVal(\'rpbsize\')||undefined,capture_after:{baud:\'auto\'}})"' + dis + '>Load</button>' +
         ' <button onclick="rJob(\'picoboot/info\',{})"' + dis + '>BOOTSEL info</button></p>';
  }
  h += '<p class="sub"><strong>UF2 drive</strong> <span class="muted">copy to the UF2 bootloader drive (enters the bootloader first if needed)</span><br>' +
       rFileSel('ruf2file') + ' <button class="act" onclick="rJob(\'flash/uf2\',{file:rVal(\'ruf2file\')})"' + dis + '>Copy UF2</button></p>';

  h += '<h2>Staged files</h2><p class="sub"><input type="file" id="rupfile"> as <input id="rupname" class="txt" size="20" placeholder="name">' +
       ' <button onclick="rUpload()">Upload</button> <span id="rupst" class="muted"></span></p><div id="rfiles" class="sub"></div>' +
       '<p class="sub" id="rcapcfg"></p>';

  if ((r.serial && r.serial.kind != 'none') || (r.rp && (r.rp.picoboot_interface >= 0 || r.rp.reset_interface >= 0))) {
    h += '<h2>Device flash</h2><p class="sub"><span class="muted">Partitions and the FAT, LittleFS or SPIFFS in them, ' +
         'through the bootloader (ESP loader, RP2 PICOBOOT, SAMD SAM-BA read-only). The device stays in its bootloader ' +
         'while the session is open.</span><br>platform <select id="rdfplat"><option>auto</option><option>esp</option>' +
         '<option>rp2</option><option>samd</option></select> keep open <input id="rdfkeep" class="txt" type="number" value="120" ' +
         'style="width:60px"> s <button class="act" onclick="rDfRun([])"' + dis + '>Map flash</button>' +
         ' <button onclick="rDfClose()"' + dis + '>Close session</button> <span id="rdfst" class="muted"></span></p>' +
         '<div id="rdfmap" class="sub"></div><div id="rdffs" class="sub"></div>';
  }
  if (r.mass_storage) {
    h += '<h2>Mass storage</h2><p class="sub">Path <input id="rmscpath" class="txt" size="30" value="/">' +
         ' <button onclick="rMscList()"' + dis + '>List</button> <button onclick="rMscUp()">Up</button>' +
         ' <button onclick="rMscMkdir()"' + dis + '>New folder</button> <span id="rmscst" class="muted"></span><br>' +
         'Upload ' + rFileSel('rmscfile') + ' <button onclick="rMscPut()"' + dis + '>to this folder</button>' +
         ' <span class="muted">(or pick a local file in Staged files first)</span></p><div id="rmsc"></div>';
  }
  el.innerHTML = h;
  rEspImgs = rEspImgs.length ? rEspImgs : [{ file: '', offset: '0x0' }];
  rEspRenderImgs();
  rFiles();
  rCapCfg();
  if (r.monitor && rMonBusid != rBusid) {
    rMonBusid = rBusid;
    rMonNext = -1;
    rMonPortN = 0;
    rMonPoll();
  }
}

/* --- staged files ----------------------------------------------------- */
var rFileNames = [];

function rVal(id) { var e = document.getElementById(id); return e ? e.value.trim() : ''; }

function rFileSel(id) {
  var h = '<select id="' + id + '" class="rfsel">';
  rFileNames.forEach(function (n) { h += '<option>' + esc(n) + '</option>'; });
  return h + '</select>';
}

function rFiles() {
  api('GET', '/api/rescue/files', null, function (ok, r) {
    if (!ok) return;
    rFileNames = r.files.map(function (f) { return f.name; });
    var h = '<table><thead><tr><th>Name</th><th>Size</th><th></th></tr></thead><tbody>';
    r.files.forEach(function (f) {
      h += '<tr><td>' + esc(f.name) + '</td><td>' + f.size + '</td><td><a href="/api/rescue/files/' + encodeURIComponent(f.name) +
           '" style="color:#e94560">download</a> <button onclick="rDelFile(\'' + esc(f.name) + '\')">Delete</button></td></tr>';
    });
    h += '</tbody></table><span class="muted">' + r.used + ' bytes used, ' + r.space + ' free (PSRAM, lost on reboot)</span>';
    var el = document.getElementById('rfiles');
    if (el) el.innerHTML = h;
    Array.prototype.forEach.call(document.querySelectorAll('select.rfsel'), function (s) {
      var cur = s.value;
      s.innerHTML = rFileNames.map(function (n) { return '<option>' + esc(n) + '</option>'; }).join('');
      if (rFileNames.indexOf(cur) >= 0) s.value = cur;
    });
    rEspRenderImgs();
  });
}

function rUpload(after) {
  var f = document.getElementById('rupfile').files[0];
  if (!f) { alert('Pick a file'); return; }
  var name = (rVal('rupname') || f.name).replace(/[^A-Za-z0-9._-]/g, '_').replace(/^\.+/, '');
  var st = document.getElementById('rupst');
  var x = new XMLHttpRequest();
  x.open('PUT', '/api/rescue/files/' + encodeURIComponent(name), true);
  x.upload.onprogress = function (e) { if (e.lengthComputable) st.textContent = Math.round(100 * e.loaded / e.total) + '%'; };
  x.onload = function () {
    var r = null;
    try { r = JSON.parse(x.responseText); } catch (e) {}
    st.textContent = x.status == 200 ? ('staged ' + name + ' (' + f.size + ' bytes)') : ((r && r.error) || ('HTTP ' + x.status));
    rFiles();
    if (x.status == 200 && after) after(name);
  };
  x.onerror = function () { st.textContent = 'network error'; };
  x.send(f);
}

function rDelFile(n) {
  if (!confirm('Delete staged ' + n + '?')) return;
  api('DELETE', '/api/rescue/files/' + encodeURIComponent(n), null, function (ok, r) {
    if (!ok) alert(r && r.error);
    rFiles();
  });
}

/* --- jobs --------------------------------------------------------------- */
var rJobId = 0, rJobTimer = null;

function rJob(action, body) {
  if (body.file === '') { alert('Stage a file first'); return; }
  body.port = rPort();
  api('POST', rUrl(action), body, function (ok, r) {
    if (!ok) { alert((r && r.error) || 'failed'); return; }
    rJobId = r.job;
    document.getElementById('rjob').style.display = 'block';
    rJobPoll();
  });
}

function rJobPoll() {
  clearTimeout(rJobTimer);
  api('GET', '/api/rescue/jobs/' + rJobId, null, function (ok, j) {
    if (!ok || !j) return;
    var p = document.getElementById('rjobp'), s = document.getElementById('rjobs'), l = document.getElementById('rjobl');
    if (!p) return;
    p.value = j.progress;
    s.textContent = j.kind + ': ' + j.state + (j.stage ? ' - ' + j.stage : '') + (j.error ? ' - ' + j.error : '') +
                    ' (' + j.elapsed_s + ' s)';
    s.className = j.state == 'failed' ? 'lo' : j.state == 'done' ? 'hi' : '';
    l.textContent = (j.log || '') + (j.result ? '\nresult: ' + JSON.stringify(j.result) : '');
    l.scrollTop = l.scrollHeight;
    if (j.state == 'running' || j.state == 'queued') {
      rJobTimer = setTimeout(rJobPoll, 700);
    } else {
      rFiles();
      setTimeout(function () { rescueDevList(rBusid); }, 500);
    }
  });
}

function rJobCancel() {
  api('DELETE', '/api/rescue/jobs/' + rJobId, null, function (ok, r) { if (!ok) alert(r && r.error); });
}

/* --- ESP images --------------------------------------------------------- */
var rEspImgs = [];

function rEspRenderImgs() {
  var el = document.getElementById('respimgs');
  if (!el) return;
  var h = '';
  rEspImgs.forEach(function (im, i) {
    h += 'file <select onchange="rEspImgs[' + i + '].file=this.value">' + rFileNames.map(function (n) {
      return '<option' + (n == im.file ? ' selected' : '') + '>' + esc(n) + '</option>';
    }).join('') + '</select> at <input class="txt" size="8" value="' + esc(im.offset) +
         '" onchange="rEspImgs[' + i + '].offset=this.value"> <button onclick="rEspImgs.splice(' + i +
         ',1);rEspRenderImgs()">x</button><br>';
    if (!im.file && rFileNames.length) im.file = rFileNames[0];
  });
  el.innerHTML = h;
}

function rEspAddImg() { rEspImgs.push({ file: rFileNames[0] || '', offset: '0x10000' }); rEspRenderImgs(); }

function rEspFlash() {
  if (!rEspImgs.length || rEspImgs.some(function (i) { return !i.file; })) { alert('Stage the images and pick them'); return; }
  rJob('flash/esp', {
    images: rEspImgs.map(function (i) { return { file: i.file, offset: i.offset }; }),
    before: rVal('respbefore'), after: rVal('respafter'), baud: parseInt(rVal('respbaud'), 10),
    erase_all: document.getElementById('resperase').checked, verify: document.getElementById('respverify').checked,
    capture_after: document.getElementById('respcap').checked ? { baud: 'auto' } : undefined
  });
}

/* --- serial capture ------------------------------------------------------- */
var rMonNext = -1, rMonTimer = null, rMonBusid = '', rMonPortN = 0;

function rMonBauds() {
  var v = rVal('rmbaud') || 'auto';
  if (v == 'auto') return 'auto';
  var list = v.split(',').map(function (x) { x = x.trim(); return x == 'auto' ? 'auto' : parseInt(x, 10); });
  return list.length == 1 ? list[0] : list;
}

function rMonPort() { var e = document.getElementById('rmport'); return e ? (parseInt(e.value, 10) || 0) : 0; }

function rMonStart() {
  var body = { baud: rMonBauds(), size: (parseInt(rVal('rmsize'), 10) || 256) * 1024,
               arm: document.getElementById('rmarm').checked };
  var d = rTriVal('rmdtr'), t = rTriVal('rmrts');
  if (d !== undefined) body.dtr = d;
  if (t !== undefined) body.rts = t;
  api('POST', rUrl('capture/start'), body, function (ok, r) {
    if (!ok) { alert((r && r.error) || 'failed'); return; }
    rMonNext = -1;                      /* from this epoch */
    rMonBusid = rBusid;
    document.getElementById('rmout').textContent = '';
    setTimeout(rMonPoll, 300);
  });
}

function rMonStop(disarm) {
  clearTimeout(rMonTimer);
  api('POST', rUrl('capture/stop'), { disarm: !!disarm }, function (ok, r) {
    var s = document.getElementById('rmst');
    if (s) s.textContent = ok ? ('stopped' + (r.armed ? ' (still armed)' : '')) : ((r && r.error) || 'failed');
  });
}

function rMonSwitch() {
  rMonNext = -1;
  rMonBusid = rBusid;
  document.getElementById('rmout').textContent = '';
  rMonPoll();
}

function rMonPoll() {
  clearTimeout(rMonTimer);
  if (rMonBusid != rBusid || !document.getElementById('rmout')) return;
  var port = rMonPort(), ts = document.getElementById('rmts').checked;
  var from = rMonNext < 0 ? 'from=epoch' : 'since=' + rMonNext;
  document.getElementById('rmdl').href = '/api/rescue/capture/' + encodeURIComponent(rBusid) + '/' + port + '.log';
  api('GET', rUrl('capture') + '?port=' + port + '&' + from + '&max=8192' + (ts ? '&format=lines' : ''), null, function (ok, r) {
    var o = document.getElementById('rmout'), s = document.getElementById('rmst');
    if (!o) return;
    if (!ok) { s.textContent = (r && r.error) || 'no capture'; return; }
    if (r.text) {
      var atEnd = o.scrollTop + o.clientHeight >= o.scrollHeight - 4;
      o.textContent = (o.textContent + r.text).slice(-131072);
      if (atEnd) o.scrollTop = o.scrollHeight;
    }
    rMonNext = r.next;
    if (r.ports != rMonPortN) {         /* the device came back with a different number of ports */
      rMonPortN = r.ports;
      var sel = document.getElementById('rmport'), cur = sel.value, opts = '';
      for (var i = 0; i < Math.max(1, r.ports); i++) opts += '<option>' + i + '</option>';
      sel.innerHTML = opts;
      sel.value = cur;
    }
    s.textContent = r.state + ', ' + r.device + ', epoch ' + r.epoch + ', ' + r.serial_baud + ' baud' +
                    (r.auto_baud ? ' (auto)' : '') + (r.dropped ? ', output dropped (buffer wrapped)' : '') +
                    (r.reconnects ? ', ' + r.reconnects + ' reconnect(s)' : '');
    if (r.running || r.text) rMonTimer = setTimeout(rMonPoll, r.text ? 150 : 500);
  });
}

function rMonSend() {
  var t = document.getElementById('rmin').value + (document.getElementById('rmnl').checked ? '\r\n' : '');
  api('POST', rUrl('capture/send'), { port: rMonPort(), text: t }, function (ok, r) {
    if (!ok) alert(r && r.error);
    else document.getElementById('rmin').value = '';
  });
}

function rMonSendRaw(hex) { api('POST', rUrl('capture/send'), { port: rMonPort(), hex: hex }, null); }

function rCapCfg() {
  api('GET', '/api/capture', null, function (ok, r) {
    var e = document.getElementById('rcapcfg');
    if (!ok || !e) return;
    e.innerHTML = 'Capture budget: total <input id="rcaptot" class="txt" type="number" style="width:70px" value="' +
      Math.round(r.total_limit / 1024) + '"> KB, default <input id="rcapdef" class="txt" type="number" style="width:60px" value="' +
      Math.round(r.default_size / 1024) + '"> KB/port <button onclick="rCapCfgSave()">Save</button> <span class="muted">' +
      Math.round(r.reserved / 1024) + ' KB in use by ' + r.captures.length + ' capture(s); armed: ' +
      (r.armed.map(function (a) { return esc(a.busid); }).join(', ') || 'none') + '</span>';
  });
}

function rCapCfgSave() {
  api('POST', '/api/capture/config', { total_limit: parseInt(rVal('rcaptot'), 10) * 1024,
                                       default_size: parseInt(rVal('rcapdef'), 10) * 1024 }, function (ok, r) {
    if (!ok) alert(r && r.error);
    rCapCfg();
  });
}

/* --- mass storage ---------------------------------------------------------- */
function rMscPath(name) {
  var p = rVal('rmscpath') || '/';
  return (p.charAt(p.length - 1) == '/' ? p : p + '/') + name;
}

function rMscList() {
  var st = document.getElementById('rmscst');
  st.textContent = 'reading...';
  api('GET', rUrl('msc/list') + '?path=' + encodeURIComponent(rVal('rmscpath') || '/'), null, function (ok, r) {
    st.textContent = ok ? '' : ((r && r.error) || 'failed');
    if (!ok) return;
    var h = '<table><thead><tr><th>Name</th><th>Size</th><th>Modified</th><th></th></tr></thead><tbody>';
    r.files.forEach(function (f) {
      var p = esc(rMscPath(f.name));
      h += '<tr><td>' + (f.dir ? '<a href="#" style="color:#e94560" onclick="document.getElementById(\'rmscpath\').value=\'' + p +
           '\';rMscList();return false">' + esc(f.name) + '/</a>' : esc(f.name)) + '</td><td>' + (f.dir ? '' : f.size) +
           '</td><td>' + esc(f.modified) + '</td><td>' +
           (f.dir ? '' : '<button onclick="rMscGet(\'' + p + '\')">Download</button> ') +
           '<button onclick="rMscRename(\'' + p + '\')">Rename</button> <button onclick="rMscDel(\'' + p + '\')">Delete</button></td></tr>';
    });
    document.getElementById('rmsc').innerHTML = h + '</tbody></table>';
  });
}

function rMscUp() {
  var p = rVal('rmscpath').replace(/\/+$/, '');
  document.getElementById('rmscpath').value = p.substring(0, p.lastIndexOf('/')) || '/';
  rMscList();
}

function rMscDone(ok, r) {
  if (!ok) alert((r && r.error) || 'failed');
  rMscList();
}

function rMscDel(p) {
  if (confirm('Delete ' + p + ' on the device?')) api('POST', rUrl('msc/delete'), { path: p }, rMscDone);
}

function rMscRename(p) {
  var to = prompt('Rename / move ' + p + ' to', p);
  if (to && to != p) api('POST', rUrl('msc/rename'), { from: p, to: to }, rMscDone);
}

function rMscMkdir() {
  var n = prompt('New folder name');
  if (n) api('POST', rUrl('msc/mkdir'), { path: rMscPath(n) }, rMscDone);
}

function rMscPut() {
  var f = rVal('rmscfile');
  if (!f) { alert('Stage a file first'); return; }
  rJob('msc/write', { file: f, path: rMscPath(f) });
}

function rMscGet(p) {
  var name = p.substring(p.lastIndexOf('/') + 1).replace(/[^A-Za-z0-9._-]/g, '_').replace(/^\.+/, '') || 'file';
  api('POST', rUrl('msc/read'), { path: p, file: name }, function (ok, r) {
    if (!ok) { alert((r && r.error) || 'failed'); return; }
    rJobId = r.job;
    document.getElementById('rjob').style.display = 'block';
    var wait = function () {
      api('GET', '/api/rescue/jobs/' + rJobId, null, function (ok2, j) {
        if (ok2 && j.state == 'done') { rFiles(); window.location = '/api/rescue/files/' + encodeURIComponent(name); }
        else if (ok2 && (j.state == 'running' || j.state == 'queued')) setTimeout(wait, 500);
        else alert('Download failed: ' + (j && j.error));
      });
    };
    rJobPoll();
    wait();
  });
}

/* --- device flash: partitions and file systems through the bootloader ------ */
var rDf = { job: 0, next: 0, part: '' };

function rHex(n) { return '0x' + Number(n).toString(16); }

/* Run ops in the device's flash session (starting one if none is open) and
   call cb with their results once the job has them. */
function rDfRun(ops, cb) {
  var st = document.getElementById('rdfst');
  if (st) st.textContent = 'working...';
  var body = { ops: ops, platform: rVal('rdfplat') || 'auto', keep_open_s: parseInt(rVal('rdfkeep'), 10) || 0, port: rPort() };
  api('POST', rUrl('dutflash'), body, function (ok, r) {
    if (!ok) { if (st) st.textContent = (r && r.error) || 'failed'; return; }
    if (!r.added_to_open_session || r.job != rDf.job) { rDf.job = r.job; rDf.next = 0; }
    var first = rDf.next;
    rDf.next += ops.length;
    rJobId = r.job;
    document.getElementById('rjob').style.display = 'block';
    rJobPoll();
    var wait = function () {
      api('GET', '/api/rescue/jobs/' + r.job, null, function (ok2, j) {
        if (!ok2 || !j) { if (st) st.textContent = 'lost the session job'; return; }
        var res = j.result || {};
        if (res.regions) rDfMap(res);
        var done = (res.ops || []).filter(function (o) { return o.index >= first && o.index < first + ops.length; });
        if (ops.length ? done.length == ops.length : !!res.regions) {
          var errs = done.filter(function (o) { return !o.ok; }).map(function (o) { return o.op + ': ' + o.error; });
          if (st) st.textContent = errs.length ? errs.join('; ') : (j.state == 'running' ? 'session open' : j.state);
          if (cb) cb(done);
        } else if (j.state == 'running' || j.state == 'queued') {
          setTimeout(wait, 600);
        } else if (st) {
          st.textContent = 'session ended: ' + (j.error || j.state);
        }
      });
    };
    wait();
  });
}

function rDfClose() {
  api('POST', rUrl('dutflash/close'), {}, function (ok, r) {
    var st = document.getElementById('rdfst');
    if (st) st.textContent = ok ? 'closing (the device is reset)' : ((r && r.error) || 'failed');
  });
}

function rDfMap(res) {
  var el = document.getElementById('rdfmap');
  if (!el) return;
  var d = res.device || {};
  var h = '<strong>' + esc(d.chip || '?') + '</strong> ' + esc(d.platform || '') + ', ' + (d.flash_size >> 10) + ' KB flash' +
          (d.writable ? '' : ' <span class="muted">(read-only)</span>');
  (res.notes || []).forEach(function (n) { h += '<br><span class="muted">' + esc(n) + '</span>'; });
  h += '<table><thead><tr><th>Name</th><th>Type</th><th>Offset</th><th>Size</th><th>File system</th><th></th></tr></thead><tbody>';
  res.regions.forEach(function (g) {
    var fs = g.fs || '';
    var browsable = fs == 'littlefs' || fs == 'fat' || fs == 'spiffs';
    var n = esc(g.name);
    h += '<tr><td>' + n + (g.protected ? ' <span class="muted" title="code or system data">(locked)</span>' : '') + '</td><td>' +
         esc(g.type) + '</td><td>' + rHex(g.offset) + '</td><td>' + (g.size >> 10) + ' KB</td><td>' + esc(fs) +
         (g.fs_version ? ' ' + esc(g.fs_version) : '') + (g.label ? ' ' + esc(g.label) : '') + '</td><td>' +
         '<button onclick="rDfRead(\'' + n + '\')">Read</button>' +
         (browsable ? ' <button onclick="rDfBrowse(\'' + n + '\')">Browse</button>' : '') +
         (d.writable && !g.protected ? ' <button onclick="rDfFormat(\'' + n + '\',\'' + esc(fs) + '\')">Format</button>' : '') +
         '</td></tr>';
  });
  el.innerHTML = h + '</tbody></table>';
}

function rDfDownload(o) {
  rFiles();
  if (o && o.ok && o.download) window.location = o.download;
}

function rDfRead(part) {
  rDfRun([{ op: 'read_region', part: part }], function (d) { rDfDownload(d[0]); });
}

function rDfFormat(part, was) {
  var fs = prompt('Format ' + part + (was ? ' (now: ' + was + ')' : '') + ' as littlefs, fat or spiffs?', 'littlefs');
  if (!fs) return;
  if (was && was != 'blank' && !confirm('Erase everything in ' + part + '?')) return;
  rDfRun([{ op: 'format', part: part, fs: fs, confirm: true }]);
}

function rDfBrowse(part) {
  rDf.part = part;
  document.getElementById('rdffs').innerHTML = '<strong>' + esc(part) + '</strong> path <input id="rdfpath" class="txt" size="30" value="/">' +
    ' <button onclick="rDfList()">List</button> <button onclick="rDfUp()">Up</button> <button onclick="rDfMkdir()">New folder</button>' +
    '<br>Upload ' + rFileSel('rdffile') + ' <button onclick="rDfPut()">to this folder</button> <span id="rdfinfo" class="muted"></span>' +
    '<div id="rdflist"></div>';
  rDfRun([{ op: 'fs_info', part: part }], function (d) {
    var o = d[0];
    if (o && o.ok) {
      document.getElementById('rdfinfo').textContent = o.fs + ', ' + Math.round(o.free / 1024) + ' KB free of ' +
        Math.round(o.total / 1024) + ' KB';
      rDfFiles(o);
    }
  });
}

function rDfPath(name) {
  var p = rVal('rdfpath') || '/';
  return (p.charAt(p.length - 1) == '/' ? p : p + '/') + name;
}

function rDfFiles(o) {
  var h = '<table><thead><tr><th>Name</th><th>Size</th><th></th></tr></thead><tbody>';
  (o.files || []).forEach(function (f) {
    var p = esc(rDfPath(f.name));
    h += '<tr><td>' + (f.dir ? '<a href="#" style="color:#e94560" onclick="document.getElementById(\'rdfpath\').value=\'' + p +
         '\';rDfList();return false">' + esc(f.name) + '/</a>' : esc(f.name)) + '</td><td>' + (f.dir ? '' : f.size) + '</td><td>' +
         (f.dir ? '' : '<button onclick="rDfGet(\'' + p + '\')">Download</button> ') +
         '<button onclick="rDfRename(\'' + p + '\')">Rename</button> <button onclick="rDfDel(\'' + p + '\')">Delete</button></td></tr>';
  });
  document.getElementById('rdflist').innerHTML = h + '</tbody></table>';
}

function rDfList() {
  rDfRun([{ op: 'list', part: rDf.part, path: rVal('rdfpath') || '/' }], function (d) { if (d[0] && d[0].ok) rDfFiles(d[0]); });
}

function rDfUp() {
  var p = rVal('rdfpath').replace(/\/+$/, '');
  document.getElementById('rdfpath').value = p.substring(0, p.lastIndexOf('/')) || '/';
  rDfList();
}

/* A change, then the listing again (both in the same session). */
function rDfChange(op) {
  rDfRun([op, { op: 'list', part: rDf.part, path: rVal('rdfpath') || '/' }], function (d) { if (d[1] && d[1].ok) rDfFiles(d[1]); });
}

function rDfDel(p) {
  if (confirm('Delete ' + p + ' on the device?')) rDfChange({ op: 'delete', part: rDf.part, path: p });
}

function rDfRename(p) {
  var to = prompt('Rename / move ' + p + ' to', p);
  if (to && to != p) rDfChange({ op: 'rename', part: rDf.part, from: p, to: to });
}

function rDfMkdir() {
  var n = prompt('New folder name');
  if (n) rDfChange({ op: 'mkdir', part: rDf.part, path: rDfPath(n) });
}

function rDfPut() {
  var f = rVal('rdffile');
  if (!f) { alert('Stage a file first'); return; }
  rDfChange({ op: 'write', part: rDf.part, path: rDfPath(f), file: f });
}

function rDfGet(p) {
  var name = p.substring(p.lastIndexOf('/') + 1).replace(/[^A-Za-z0-9._-]/g, '_').replace(/^\.+/, '') || 'file';
  rDfRun([{ op: 'read', part: rDf.part, path: p, file: name }], function (d) { rDfDownload(d[0]); });
}

function rUrl(action) { return '/api/usb/devices/' + encodeURIComponent(rBusid) + '/' + action; }
function rPort() { var e = document.getElementById('rport'); return e ? (parseInt(e.value, 10) || 0) : 0; }

function rSetSerial(baud, lines) {
  var body = { port: rPort() };
  if (baud) body.baud = parseInt(document.getElementById('rbaud').value, 10);
  if (lines) {
    body.dtr = document.getElementById('rdtr').checked;
    body.rts = document.getElementById('rrts').checked;
  }
  var s = document.getElementById('rserst');
  s.textContent = '...';
  api('POST', rUrl('serial'), body, function (ok, r) {
    s.textContent = ok ? 'ok' : ((r && r.error) || 'failed');
    if (ok) setTimeout(rescueLoad, 300);
  });
}

function rReset(mode) {
  if (!confirm('Run ' + mode + ' on ' + rBusid + '? The device may reset and re-enumerate.')) return;
  var o = document.getElementById('rresout');
  o.style.display = 'block';
  o.textContent = 'running ' + mode + ' ...';
  api('POST', rUrl('reset'), { mode: mode, port: rPort() }, function (ok, r) {
    var t = (ok ? 'ok' : 'FAILED: ' + ((r && r.error) || '')) + '\n';
    if (r) {
      if (r.ran) t += 'ran: ' + r.ran + '\n';
      if (r.dropped !== undefined) {
        t += 'dropped off the bus: ' + r.dropped + (r.waited_ms !== undefined ? ' (waited ' + r.waited_ms + ' ms)' : '') + '\n';
      }
      if (r.reenumerated) {
        t += 'came back as: ' + r.reenumerated.vid + ':' + r.reenumerated.pid + ' ' + r.reenumerated.product + '\n';
      }
      if (r.requests) t += r.requests.join('\n');
    }
    o.textContent = t;
    rescueDevList(rBusid);
    loadDevs();
  });
}

function rPokes() {
  var out = [];
  document.getElementById('rpokes').value.split(',').forEach(function (p) {
    p = p.trim();
    if (!p) return;
    out.push(p.indexOf('hex:') === 0 ? { hex: p.slice(4) } : p);
  });
  return out;
}

function rTriVal(id) {
  var v = document.getElementById(id).value;
  return v === '' ? undefined : v == '1';
}

function rConsole() {
  var body = {
    port: rPort(),
    serial_baud: parseInt(document.getElementById('rcbaud').value, 10),
    pokes: rPokes(),
    settle_s: parseFloat(document.getElementById('rsettle').value),
    capture_s: parseFloat(document.getElementById('rcap').value)
  };
  var d = rTriVal('rcdtr'), t = rTriVal('rcrts');
  if (d !== undefined) body.dtr = d;
  if (t !== undefined) body.rts = t;
  var s = document.getElementById('rconst'), o = document.getElementById('rconout');
  s.textContent = 'capturing ...';
  api('POST', rUrl('console'), body, function (ok, r) {
    s.textContent = ok ? (r.bytes + ' bytes, ' + r.classification + (r.truncated ? ' (truncated)' : '') +
                          (r.warning ? ' - ' + r.warning : ''))
                       : ((r && r.error) || 'failed');
    o.style.display = 'block';
    o.textContent = r && r.text !== undefined ? (r.text || '(nothing received)') : '';
  });
}

/* Refresh pin values, wire names, directions and pull states on initial
   load.  Wire names are also pre-filled server-side in the HTML, but this
   keeps the live state (values/pulls) current too. */
rp();
document.getElementById('tok').value = tokGet();
authStatus();
otaStatus();
netLoad();
refreshUsb();
loadMux();
topoLoad();
/* Deep link: http://<bridge>/#rescue=<busid> opens the rescue panel */
(function () {
  var m = /^#rescue=(.+)$/.exec(location.hash);
  if (m) rescueOpen(decodeURIComponent(m[1]));
  else rescueDevList('');
})();
setInterval(function () {
  var u = document.getElementById('su'), hb = document.getElementById('sh');
  if ((u && u.className.indexOf('on') >= 0) || (hb && hb.className.indexOf('on') >= 0)) refreshUsb();
}, 3000);
