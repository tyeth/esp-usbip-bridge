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
       rKv('Serial number', esc(r.serial)) +
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
  el.innerHTML = h;
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
