/* Xiaomi ESP Flasher – web GUI (no external dependencies).
   The "Configure" tab is the pvvx TelinkMiFlasher configuration section (pvvx_config.js); this file provides
   the transport for it: the ESP32 holds the BLE link (connect/disconnect), raw commands go to
   POST /api/device/<mac>/cmd and 0x1F1F notifications come back over SSE (or polling). */
(function () {
  'use strict';
  const $q = (s, r) => (r || document).querySelector(s);
  const $$ = (s, r) => Array.from((r || document).querySelectorAll(s));
  const esc = (s) => String(s == null ? '' : s).replace(/[&<>"]/g, (c) => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'}[c]));
  const fmtTime = (epoch) => epoch ? new Date(epoch * 1000).toLocaleString() : '—';
  const clock = () => new Date().toLocaleTimeString();
  const GH_RAW = 'https://raw.githubusercontent.com/pvvx/ATC_MiThermometer/master/';

  // ---------------------------------------------------------------- state
  const S = { devices: [], firmware: [], status: {}, current: null, currentFull: null, logSeq: 0, ota: null, view: 'devices', tab: 'overview', manifest: null };
  const PV = { mac: null, connected: false, seq: 0, poll: null, connecting: false };

  // ---------------------------------------------------------------- api
  async function api(path, opts) {
    const r = await fetch('/api' + path, Object.assign({ cache: 'no-store' }, opts || {}));
    let j = null;
    try { j = await r.json(); } catch (e) { /* no body */ }
    if (!r.ok) throw new Error((j && (j.error + (j.msg ? ': ' + j.msg : ''))) || ('HTTP ' + r.status));
    return j;
  }
  const post = (path, body) => api(path, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body || {}) });

  // ---------------------------------------------------------------- log + toast
  const logEl = $q('#log');
  function addLog(t, msg) {
    const ts = t ? new Date(t * 1000).toLocaleTimeString() : clock();
    logEl.textContent += `[${ts}] ${msg}\n`;
    if (logEl.textContent.length > 60000) logEl.textContent = logEl.textContent.slice(-40000);
    logEl.scrollTop = logEl.scrollHeight;
  }
  window.xfLog = (m) => addLog(0, String(m).replace(/<[^>]+>/g, ''));   // pvvx addLog() -> our log panel
  window.xfMenuUpgrade = () => {};                                       // firmware buttons live on our Firmware tab
  let toastTimer;
  function toast(msg, kind) {
    const t = $q('#toast');
    t.textContent = msg;
    t.className = 'toast ' + (kind || '');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => t.classList.add('hidden'), 4000);
  }
  $q('#log-clear').onclick = () => (logEl.textContent = '');
  $q('#log-toggle').onclick = (e) => { logEl.classList.toggle('hidden'); e.target.textContent = logEl.classList.contains('hidden') ? 'Show' : 'Hide'; };

  // ---------------------------------------------------------------- theme
  const applyTheme = () => document.documentElement.setAttribute('data-theme', localStorage.getItem('theme') || (matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light'));
  $q('#theme-toggle').onclick = () => { localStorage.setItem('theme', document.documentElement.getAttribute('data-theme') === 'dark' ? 'light' : 'dark'); applyTheme(); };
  applyTheme();

  // ---------------------------------------------------------------- modal
  function modal(html, buttons) {
    return new Promise((resolve) => {
      const box = $q('#modal-box');
      box.innerHTML = html + '<div class="btnrow modal-btns"></div>';
      const row = $q('.modal-btns', box);
      buttons.forEach((b) => {
        const btn = document.createElement('button');
        btn.textContent = b.label; btn.className = b.cls || '';
        btn.onclick = () => { $q('#modal').classList.add('hidden'); resolve(b.value); };
        row.appendChild(btn);
      });
      $q('#modal').classList.remove('hidden');
    });
  }

  // ---------------------------------------------------------------- SSE / polling
  let es = null, pollTimer = null;
  function connectEvents() {
    if (!window.EventSource) return startPolling();
    es = new EventSource('/api/events');
    es.onopen = () => { $q('#conn-state').textContent = 'live'; stopPolling(); };
    es.onerror = () => { $q('#conn-state').textContent = 'reconnecting…'; startPolling(); };
    es.onmessage = (m) => { try { handleEvent(JSON.parse(m.data)); } catch (e) { /* ignore */ } };
  }
  function startPolling() { if (!pollTimer) pollTimer = setInterval(pollOnce, 2000); }
  function stopPolling() { if (pollTimer) { clearInterval(pollTimer); pollTimer = null; } }
  async function pollOnce() {
    try {
      const l = await api('/log?since=' + S.logSeq);
      l.lines.forEach((x) => { S.logSeq = x.seq; addLog(x.t, x.msg); });
      const o = await api('/ota/status');
      handleEvent(Object.assign({ type: 'ota_progress', session: o.session, device: o.device }, o));
      if (S.view === 'devices') refreshDevices();
      if (S.view === 'device' && S.current) refreshDevice(true);
    } catch (e) { $q('#conn-state').textContent = 'offline'; }
  }
  function handleEvent(ev) {
    switch (ev.type) {
      case 'log': if (ev.seq > S.logSeq) { S.logSeq = ev.seq; addLog(ev.t, ev.msg); } break;
      case 'state':
        setBadge(ev.state, ev.job, ev.mac);
        if (ev.job === 'connect' && ev.mac && PV.mac === ev.mac.toUpperCase()) {
          if (ev.state === 'READY') pvOnConnected();
          if (ev.state === 'SUCCESS' || ev.state === 'ERROR') pvOnDisconnected();
        }
        if (ev.state === 'SUCCESS' || ev.state === 'ERROR') { refreshDevices(); if (S.current) refreshDevice(true); }
        break;
      case 'notify': if (PV.connected && ev.mac && ev.mac.toUpperCase() === PV.mac) pvNotify(ev.seq, ev.hex); break;
      case 'ota_progress': S.ota = ev; renderProgress(); if (ev.session) setBadge(ev.session, 'flash', ev.device); break;
      case 'device': if (S.view === 'devices') refreshDevices(); if (S.current && S.current.toUpperCase() === (ev.mac || '').toUpperCase()) refreshDevice(true); break;
      case 'result': toast(ev.msg, ev.ok ? 'ok' : 'err'); if (!ev.ok) addLog(0, 'ERROR ' + (ev.error || '') + ' ' + ev.msg); refreshDevices(); if (S.current) refreshDevice(true); break;
      default: break;
    }
  }
  function setBadge(state, job, mac) {
    const b = $q('#session-badge');
    b.textContent = state + (job && job !== 'none' && state !== 'IDLE' ? ' · ' + job + (mac ? ' ' + mac : '') : '');
    b.className = 'badge ' + (state === 'ERROR' ? 'err' : state === 'SUCCESS' || state === 'IDLE' ? 'ok' : 'busy');
  }

  // ---------------------------------------------------------------- progress rendering
  function renderProgress() {
    const p = S.ota; const box = $q('#progress-box');
    if (!p || !box) return;
    const active = p.state === 'starting' || p.state === 'writing' || p.state === 'checking' || p.state === 'finishing';
    const stage = { CONNECTING: 'Connecting...', CONNECTED: 'Connected', IDENTIFYING: 'Identifying...', ACTIVATING: 'Activating...', READY: 'Ready', ERASING: 'Erasing...', WRITING: `Writing ${Math.floor(p.progress || 0)}%`, VERIFYING: 'Verifying...', REBOOTING: 'Rebooting...', RECONNECTING: 'Reconnecting...', SUCCESS: 'Done', ERROR: 'Error' }[p.session] || p.session;
    box.classList.remove('hidden');
    $q('.pbar i', box).style.width = (p.progress || 0) + '%';
    $q('.pstage', box).textContent = stage;
    const secs = ((p.elapsed_ms || 0) / 1000).toFixed(1);
    $q('.pinfo', box).textContent = `${p.bytes_sent || 0} / ${p.total_bytes || 0} bytes · ${Math.round(p.speed || 0)} B/s · ${secs} s · retries ${p.retries || 0}` + (p.last_error ? ` · last error ${p.last_error}` : '') + (active ? '' : ` · ${p.state}`);
  }

  // ---------------------------------------------------------------- views
  function show(tpl) { const v = $q('#view'); v.innerHTML = ''; v.appendChild($q('#tpl-' + tpl).content.cloneNode(true)); $$('nav a').forEach((a) => a.classList.toggle('active', a.dataset.nav === (tpl === 'device' ? 'devices' : tpl))); }

  async function refreshDevices() {
    try { S.devices = await api('/devices'); } catch (e) { return; }
    if (S.view !== 'devices') return;
    const tb = $q('#dev-rows'); if (!tb) return;
    $q('#dev-count').textContent = S.devices.length + ' device(s)';
    tb.innerHTML = S.devices.map((d) => `<tr data-mac="${esc(d.mac)}" class="${d.status === 'offline' ? 'dim' : ''}">
      <td><b>${esc(d.display_name || d.name || '?')}</b>${d.alias ? `<div class="muted small">${esc(d.name)}</div>` : ''}</td>
      <td class="mono">${esc(d.mac)}</td><td>${esc(d.model || '?')}</td><td>${esc(d.hardware || (d.identified ? d.hw_name : '?'))}</td>
      <td>${d.identified ? esc(d.firmware_kind + ' ' + d.firmware) : '<span class="muted">not identified</span>'}</td>
      <td>${d.temperature != null ? d.temperature.toFixed(2) + ' °C' : '—'}</td><td>${d.humidity != null ? d.humidity.toFixed(2) + ' %' : '—'}</td>
      <td>${d.battery != null ? d.battery + ' %' : d.battery_mv ? (d.battery_mv / 1000).toFixed(2) + ' V' : '—'}</td>
      <td>${d.rssi} dBm</td>
      <td>${d.update_available ? `<span class="tag warn">YES → ${esc(d.latest_version)}</span>` : d.identified ? (d.compatible ? '<span class="tag ok">up-to-date</span>' : `<span class="tag err" title="${esc(d.compat_message)}">${esc(d.compat_code)}</span>`) : '—'}</td>
      <td>${esc(d.status)}${d.session ? ' <span class="tag busy">' + esc(d.session) + '</span>' : ''}</td></tr>`).join('');
    $$('tr', tb).forEach((tr) => (tr.onclick = () => (location.hash = '#/device/' + tr.dataset.mac)));
  }

  function viewDevices() {
    S.view = 'devices'; S.current = null; show('devices');
    $q('#btn-scan').onclick = async () => { await post('/scan'); toast('Scan requested'); };
    $q('#btn-refresh').onclick = refreshDevices;
    $q('#btn-update-all').onclick = async () => {
      const list = S.devices.filter((d) => d.update_available && d.compatible && d.status !== 'offline');
      if (!list.length) return toast('No device with an available and compatible update', 'err');
      const ok = await modal(`<h3>Update all</h3><p>The following devices will be updated <b>sequentially</b>, each one verified before the next starts:</p><ul>${list.map((d) => `<li>${esc(d.display_name)} (${esc(d.mac)}): ${esc(d.firmware)} → ${esc(d.latest_version)}</li>`).join('')}</ul><p class="warnbox">Do not move the devices out of BLE range during the update.</p>`, [{ label: 'Cancel', value: false }, { label: 'Start updates', value: true, cls: 'danger' }]);
      if (ok) { await post('/queue/all'); toast('Update queue started'); }
    };
    $q('#btn-defaults-all').onclick = async () => {
      const list = S.devices.filter((d) => d.identified && d.firmware_kind === 'pvvx custom' && d.status !== 'offline');
      if (!list.length) return toast('No reachable pvvx device', 'err');
      const ok = await modal(`<h3>Send default config to all</h3><p>Sends <code>56</code> (CMD_ID_CFG_DEF – factory configuration of the pvvx firmware) to each device <b>sequentially</b> and reads the configuration back:</p><ul>${list.map((d) => `<li>${esc(d.display_name)} (${esc(d.mac)}), firmware ${esc(d.firmware)}</li>`).join('')}</ul><p class="warnbox">All custom settings (advertising type/interval, display, offsets in the main block, comfort mode …) return to the firmware defaults. Device names are not changed.</p>`, [{ label: 'Cancel', value: false }, { label: 'Send defaults to all', value: true, cls: 'danger' }]);
      if (ok) { await post('/queue/defaults'); toast('Default config queued for all devices'); }
    };
    $q('#btn-add').onclick = async () => {
      const mac = prompt('MAC address of the thermometer (diagnostic manual add):', 'A4:C1:38:');
      if (mac && /^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$/.test(mac)) { await post('/device/' + mac + '/add'); refreshDevices(); }
      else if (mac) toast('Invalid MAC', 'err');
    };
    refreshDevices();
  }

  // ---------------------------------------------------------------- device page
  const dl = (pairs) => pairs.map(([k, v]) => `<dt>${esc(k)}</dt><dd>${v == null || v === '' ? '—' : v}</dd>`).join('');

  async function refreshDevice(silent) {
    if (!S.current) return;
    let d;
    try { d = await api('/device/' + S.current); } catch (e) { if (!silent) toast(e.message, 'err'); return; }
    S.currentFull = d;
    if (S.view !== 'device') return;
    $q('#d-title').textContent = (d.display_name || d.mac) + (d.alias ? ` (${d.name})` : '');
    $q('#d-info').innerHTML = dl([['Name', esc(d.name)], ['Alias', esc(d.alias)], ['MAC', `<span class="mono">${esc(d.mac)}</span>`], ['Model', esc(d.model)], ['Hardware revision', d.hardware ? esc(d.hardware) + ` <span class="muted">(pvvx id ${d.hw_id}: ${esc(d.hw_name)})</span>` : (d.identified ? esc(d.hw_name) : 'not identified')], ['Firmware', d.identified ? esc(d.firmware_kind + ' ' + d.firmware) : '?'], ['Advertising', esc(d.adv_format) + (d.adv_encrypted ? ' (encrypted)' : '')], ['Connection state', esc(d.session || d.status)], ['Last seen', fmtTime(d.last_seen) + (d.last_seen_ago >= 0 ? ` (${d.last_seen_ago}s ago)` : '')]]);
    $q('#d-meas').innerHTML = dl([['Temperature', d.temperature != null ? d.temperature.toFixed(2) + ' °C' : null], ['Humidity', d.humidity != null ? d.humidity.toFixed(2) + ' %' : null], ['Battery', d.battery != null ? d.battery + ' %' : null], ['Battery voltage', d.battery_mv ? (d.battery_mv / 1000).toFixed(3) + ' V' : null], ['RSSI', d.rssi + ' dBm'], ['Live (GATT)', d.live ? `${d.live.temperature.toFixed(2)} °C / ${d.live.humidity.toFixed(2)} % / ${d.live.battery_mv} mV` : null]]);
    const fwLines = [['Installed', d.identified ? esc(d.firmware_kind + ' ' + d.firmware) : 'unknown'], ['Available', esc(d.latest_version)], ['Compatible', d.identified ? (d.compatible ? '<span class="tag ok">yes</span>' : `<span class="tag err">${esc(d.compat_code)}</span> ${esc(d.compat_message)}`) : '—'], ['Update', d.update_available ? '<span class="tag warn">Update available</span>' : d.identified && d.compatible ? '<span class="tag ok">Up-to-date</span>' : '—'], ['Last OTA', d.ota_result ? esc(d.ota_result) + ' · ' + fmtTime(d.last_ota) : null], ['Last error', d.last_error ? esc(d.last_error + ': ' + d.last_error_message) : null]];
    $q('#d-fw').innerHTML = dl(fwLines);
    $q('#d-update').disabled = !(d.update_available && d.compatible);
    $q('#d-fwcheck').innerHTML = dl([['Detected hardware', d.hardware ? esc(d.hardware) + ` (id ${d.hw_id})` : 'unknown'], ['Installed firmware', d.identified ? esc(d.firmware_kind + ' ' + d.firmware) : 'unknown'], ['Compatibility check', d.identified ? (d.compatible ? '<span class="tag ok">PASSED</span>' : '<span class="tag err">FAILED</span> ' + esc(d.compat_message)) : 'identify first'], ['Warnings', (d.compat_warnings || []).map(esc).join('<br>') || null], ['Needs Mi cloud token', d.requires_cloud_token ? 'YES – activation blocked' : 'no'], ['Ext. OTA (>128 KiB)', d.big_ota ? 'supported' : 'no']]);
    fillFwSelect(d);
    $q('#d-activate').style.display = d.firmware_kind === 'Xiaomi original' || d.firmware_kind === 'Telink (unknown)' ? '' : 'none';
    $q('#d-raw').textContent = JSON.stringify(d, null, 2);
    // pvvx section needs the DIS strings for the "Hardware Version: … B1.7" line
    if (d.dis) { window.devinfo.hrstr = d.dis.hardware || null; window.devinfo.srstr = d.dis.software || null; window.devinfo.frstr = d.dis.firmware || null; }
  }

  function fillFwSelect(d) {
    const sel = $q('#d-fwsel'); const prev = sel.value;
    const list = S.firmware.filter((f) => f.available && (f.hw_ids || []).includes(d.hw_id));
    const others = S.firmware.filter((f) => !list.includes(f) && (f.hw_ids || []).includes(d.hw_id));
    sel.innerHTML = list.map((f) => `<option value="${esc(f.id)}" ${f.id === d.latest_firmware ? 'selected' : ''}>${esc(f.name)} · v${esc(f.version)} · ${esc(f.kind)} · ${f.size} B</option>`).join('') + (others.length ? '<optgroup label="not on the ESP32 yet (download on the Firmware page)">' + others.map((f) => `<option value="${esc(f.id)}" disabled>${esc(f.name)} · v${esc(f.version)} · ${esc(f.kind)}</option>`).join('') + '</optgroup>' : '');
    if (prev && list.some((f) => f.id === prev)) sel.value = prev;
    $q('#d-flash').disabled = !(d.identified && d.compatible && sel.value);
    const dlBtn = $q('#d-download');
    if (dlBtn) dlBtn.style.display = list.length ? 'none' : '';
  }

  async function confirmFlash(d, fw, keys) {
    const html = `<h3>Update firmware</h3><dl>${dl([['Device', esc(d.display_name) + ' <span class="mono">' + esc(d.mac) + '</span>'], ['Hardware', esc(d.hardware) + ` (id ${d.hw_id}: ${esc(d.hw_name)})`], ['Current firmware', esc(d.firmware_kind + ' ' + d.firmware)], ['Target firmware', esc(fw.name) + ' v' + esc(fw.version) + ' (' + esc(fw.kind) + ')'], ['Compatibility', d.compatible ? '<span class="tag ok">OK</span>' : '<span class="tag err">FAILED</span>'], ['Firmware size', fw.size + ' bytes'], ['Checksum', 'CRC32 ' + esc(fw.crc32)], ['Warnings', (d.compat_warnings || []).map(esc).join('<br>') || 'none']])}</dl>
      <p class="warnbox"><b>WARNING:</b> Do not move the device outside BLE range during the update. ${d.firmware_kind === 'Xiaomi original' ? 'The device runs original firmware: activation will be performed first and the device will need to be re-added to Mi Home if you go back.' : ''}${fw.kind === 'original' ? ' Flashing an ORIGINAL image erases the custom configuration.' : ''}</p>`;
    const ok = await modal(html, [{ label: 'Cancel', value: false }, { label: 'Start flashing', value: true, cls: 'danger big' }]);
    if (!ok) return;
    try {
      await post('/device/' + d.mac + '/flash', Object.assign({ firmware: fw.id, confirm: true }, keys || {}));
      toast('OTA queued'); switchTab('overview');
    } catch (e) { toast(e.message, 'err'); }
  }

  function switchTab(t) { S.tab = t; $$('#d-tabs button').forEach((b) => b.classList.toggle('active', b.dataset.tab === t)); $$('section.tab').forEach((s) => s.classList.toggle('hidden', s.dataset.tab !== t)); }

  // ---------------------------------------------------------------- pvvx bridge (Configure tab)
  function pvBleWrite(bytes) {
    // replacement for settingsCharacteristics.writeValue(): raw command through the ESP32
    const hex = Array.from(bytes, (b) => ('0' + b.toString(16)).slice(-2)).join('');
    return post('/device/' + PV.mac + '/cmd', { hex: hex, wait: 0 }).then(() => 'ok');
  }
  function pvNotify(seq, hex) {
    if (seq <= PV.seq) return;
    PV.seq = seq;
    const bytes = window.hexToBytes(hex);
    try { window.CustomBlkParse(new DataView(bytes.buffer)); } catch (e) { console.warn('CustomBlkParse', e); }
  }
  async function pvPoll() {
    if (!PV.connected) return;
    try {
      const r = await api('/device/' + PV.mac + '/notify?since=' + PV.seq);
      (r.notify || []).forEach((n) => pvNotify(n.seq, n.hex));
      if (!r.connected) pvOnDisconnected();
    } catch (e) { /* ignore */ }
  }
  function pvInitialMenu() {
    // customAction(): the buttons shown right after connecting to a custom firmware
    const custcfg = $q('#custcfg');
    if (custcfg) custcfg.innerHTML = '<button type="button" onclick="sendCustomSetting(&quot;55&quot;);">Get Config</button><br>Send commands to custom firmware:<br><input type="text" id="cmdTXT" value=""><button type="button" onclick="sendCustomSetting($(&quot;cmdTXT&quot;).value,settingsCharacteristics);">Send</button><br>';
  }
  function pvOnConnected() {
    PV.connected = true; PV.connecting = false;
    window.settingsCharacteristics = { writeValue: pvBleWrite };
    window.cfg.enable = false; window.cfg.hver = null; window.hwver_id = null;
    const d = S.currentFull;
    const nm = d ? (d.name || d.model) : '';
    window.setStatus("'" + nm + (window.devinfo.hrstr ? ' HW:' + window.devinfo.hrstr : '') + "' connected – Detected custom Firmware");
    window.addLog('Connected');
    if (window.devinfo.hrstr) window.addLog('Hardware Revision String: ' + window.devinfo.hrstr);
    if (window.devinfo.srstr) window.addLog('Software Revision String: ' + window.devinfo.srstr);
    if (window.devinfo.frstr) window.addLog('Firmware Revision String: ' + window.devinfo.frstr);
    pvInitialMenu();
    if (!es || es.readyState !== 1) { PV.poll = setInterval(pvPoll, 1000); }
    else { PV.poll = setInterval(pvPoll, 3000); }  // belt and braces
  }
  function pvOnDisconnected() {
    if (!PV.connected && !PV.connecting) return;
    PV.connected = false; PV.connecting = false;
    if (PV.poll) { clearInterval(PV.poll); PV.poll = null; }
    window.settingsCharacteristics = null;
    window.cfg.enable = false; window.cfg.hver = null; window.hwver_id = null;
    window.ext.enable = false; window.pincode.enable = false; window.trg.enable = false; window.cmf.enable = false; window.dnm.enable = false;
    const c = $q('#custcfg'); if (c) c.innerHTML = '';
    const a = $q('#addSensors'); if (a) a.innerHTML = '';
    const t = $q('#tempHumiData'); if (t) t.innerHTML = 'Temp/Humidity: waiting notify for data after connecting';
    window.setStatus('Disconnected');
    window.addLog('Disconnected.');
  }
  async function pvConnect(mac) {
    PV.mac = mac.toUpperCase(); PV.seq = 0; PV.connecting = true;
    window.setStatus('Connecting to: ' + ((S.currentFull && (S.currentFull.name || S.currentFull.model)) || mac) + ' ...');
    window.addLog('Connecting to: ' + mac);
    try { const st = await api('/status'); if (st.log_seq) PV.seq = 0; } catch (e) { /* ignore */ }
    try { await post('/device/' + mac + '/connect'); } catch (e) { toast(e.message, 'err'); PV.connecting = false; return; }
    // fallback if the SSE state event is missed
    let tries = 0;
    const t = setInterval(async () => {
      if (PV.connected || !PV.connecting || ++tries > 60) { clearInterval(t); return; }
      try { const st = await api('/status'); if ((st.hold_device || '').toUpperCase() === PV.mac) pvOnConnected(); } catch (e) { /* ignore */ }
    }, 1500);
  }
  async function pvDisconnect() {
    if (PV.mac) { try { await post('/device/' + PV.mac + '/disconnect'); } catch (e) { /* ignore */ } }
    pvOnDisconnected();
  }

  async function viewDevice(mac) {
    if (PV.connected || PV.connecting) await pvDisconnect();
    S.view = 'device'; S.current = mac; show('device');
    $q('#view').insertAdjacentHTML('afterbegin', '<div id="progress-box" class="progress hidden"><div class="pstage"></div><div class="pbar"><i></i></div><div class="pinfo muted"></div></div>');
    if (!S.firmware.length) { try { S.firmware = await api('/firmware'); } catch (e) { /* ignore */ } }
    $$('#d-tabs button').forEach((b) => (b.onclick = () => switchTab(b.dataset.tab)));
    const act = (path, body, msg) => async () => { try { await post('/device/' + mac + path, body); toast(msg || 'Requested'); } catch (e) { toast(e.message, 'err'); } };
    $q('#d-identify').onclick = act('/identify', {}, 'Identification queued');
    $q('#d-config-go').onclick = () => switchTab('config');
    $q('#d-diag-go').onclick = () => switchTab('diag');
    $q('#d-update').onclick = () => { switchTab('firmware'); };
    $q('#d-alias').onclick = async () => { const a = prompt('Alias for this device:', (S.currentFull && S.currentFull.alias) || ''); if (a != null) { await post('/device/' + mac + '/alias', { alias: a }); refreshDevice(true); } };
    $q('#d-forget').onclick = async () => { if (confirm('Forget this device?')) { await post('/device/' + mac + '/forget'); location.hash = '#/'; } };
    $q('.btnrow', $q('section[data-tab="firmware"]')).insertAdjacentHTML('beforeend', '<button id="d-download" class="big" style="display:none">Download latest from GitHub to ESP32</button>');
    $q('#d-download').onclick = async () => { const d = S.currentFull; if (!d) return; await downloadLatestForDevice(d); S.firmware = await api('/firmware'); await refreshDevice(true); };
    $q('#d-flash').onclick = () => { const d = S.currentFull; const fw = S.firmware.find((f) => f.id === $q('#d-fwsel').value); if (!d || !fw) return; const keys = {}; if ($q('#d-token').value && $q('#d-bindkey').value) { keys.token = $q('#d-token').value; keys.bind_key = $q('#d-bindkey').value; } confirmFlash(d, fw, keys); };
    $q('#d-fwsel').onchange = () => { if (S.currentFull) fillFwSelect(S.currentFull); };
    $q('#d-activate').onclick = async () => { const ok = await modal('<h3>Do Activation</h3><p>Performs the Mi registration (ECDH key exchange) with the device. After this the device must be re-added in Mi Home if you want to use it there. The obtained token/bindkey are printed in the log.</p>', [{ label: 'Cancel', value: false }, { label: 'Activate', value: true, cls: 'warn' }]); if (ok) { const keys = {}; if ($q('#d-token').value && $q('#d-bindkey').value) { keys.token = $q('#d-token').value; keys.bind_key = $q('#d-bindkey').value; } act('/activate', keys, 'Activation queued')(); } };
    $q('#cfg-connect').onclick = () => pvConnect(mac);
    $q('#cfg-disconnect').onclick = () => pvDisconnect();
    await refreshDevice(false);
    switchTab(S.tab || 'overview');
    renderProgress();
  }

  // ---------------------------------------------------------------- GitHub manifest / images (browser side, no TLS on the ESP32)
  async function fetchManifest() {
    const r = await fetch(GH_RAW + 'firmware.json', { cache: 'no-cache' });
    if (!r.ok) throw new Error('GitHub HTTP ' + r.status);
    const text = await r.text();
    S.manifest = JSON.parse(text);
    await fetch('/api/firmware/manifest', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: text });
    return S.manifest;
  }
  function manifestEntries(m) {
    // same grouping as the ESP32 (firmware_provider.cpp parse_manifest): file -> hw ids
    const bcd = (v) => ((v >> 4) & 15) + '.' + (v & 15);
    const out = [];
    const add = (arr, kind, version) => (arr || []).forEach((f, i) => {
      if (!f || f === '?' || f === '/') return;
      let e = out.find((x) => x.file === f && x.kind === kind);
      if (!e) { e = { file: f, name: f.split('/').pop(), kind: kind, version: version || ((f.match(/_v([0-9._]+)\.bin/) || [])[1] || ''), hw_ids: [] }; out.push(e); }
      e.hw_ids.push(i);
    });
    add(m.custom, 'custom', bcd(m.version)); if (m.betafw && m.betaver) add(m.betafw, 'beta', bcd(m.betaver)); add(m.original, 'original', ''); add(m.signed, 'signed', '');
    return out;
  }
  async function downloadToEsp(entry) {
    const url = entry.file.startsWith('https:') ? entry.file : GH_RAW + entry.file;
    addLog(0, 'Downloading ' + entry.name + ' from GitHub…');
    const r = await fetch(url, { cache: 'no-cache' });
    if (!r.ok) throw new Error('GitHub HTTP ' + r.status);
    const buf = await r.arrayBuffer();
    const u8 = new Uint8Array(buf);
    if (u8.length < 1024 || String.fromCharCode(u8[8], u8[9], u8[10], u8[11]) !== 'KNLT') throw new Error('not a Telink OTA image');
    const q = new URLSearchParams({ name: entry.name, version: entry.version, hw: entry.hw_ids.join(','), kind: entry.kind, source: 'github ' + entry.file }).toString();
    const up = await fetch('/api/firmware/upload?' + q, { method: 'POST', headers: { 'Content-Type': 'application/octet-stream' }, body: buf });
    const j = await up.json();
    if (!up.ok) throw new Error(j.error + ' ' + (j.msg || ''));
    addLog(0, `Stored ${entry.name} on the ESP32 (${j.size} bytes, CRC32 ${j.crc32})`);
    return j;
  }
  async function downloadLatestForDevice(d) {
    try {
      const m = S.manifest || await fetchManifest();
      const e = manifestEntries(m).find((x) => x.kind === 'custom' && x.hw_ids.includes(d.hw_id));
      if (!e) return toast('No custom image listed for hardware id ' + d.hw_id, 'err');
      await downloadToEsp(e);
      toast('Firmware stored on the ESP32');
    } catch (err) { toast('Download failed: ' + err.message, 'err'); }
  }

  // ---------------------------------------------------------------- firmware page
  async function viewFirmware() {
    S.view = 'firmware'; S.current = null; show('firmware');
    const load = async () => {
      S.firmware = await api('/firmware');
      $q('#fw-rows').innerHTML = S.firmware.map((f) => `<tr class="${f.available ? '' : 'dim'}"><td class="mono small">${esc(f.id)}</td><td>${esc(f.name)}</td><td>${esc(f.version)}</td><td>${esc(f.kind)}</td><td>${f.size || '—'}</td><td class="mono">${esc(f.crc32)}</td><td class="small">${(f.hw_ids || []).map((i, k) => `${i}:${esc(f.hw_names[k])}`).join(', ')}</td><td class="small">${esc(f.source)}</td></tr>`).join('');
    };
    const renderRemote = () => {
      if (!S.manifest) return;
      const list = manifestEntries(S.manifest);
      $q('#fw-manifest-info').textContent = `version ${((S.manifest.version >> 4) & 15)}.${S.manifest.version & 15} (${list.length} images)`;
      $q('#fw-remote-list').innerHTML = '<table><thead><tr><th>File</th><th>Version</th><th>Kind</th><th>Hardware ids</th><th>On ESP32</th><th></th></tr></thead><tbody>' + list.map((e, i) => {
        const stored = S.firmware.find((f) => f.name === e.name && f.available);
        return `<tr><td>${esc(e.name)}</td><td>${esc(e.version)}</td><td>${esc(e.kind)}</td><td class="small">${e.hw_ids.join(',')}</td><td>${stored ? '<span class="tag ok">yes</span>' : '—'}</td><td><button class="small" data-dl="${i}">${stored ? 'Re-download' : 'Download to ESP32'}</button></td></tr>`;
      }).join('') + '</tbody></table>';
      $$('#fw-remote-list button[data-dl]').forEach((b) => (b.onclick = async () => { b.disabled = true; try { await downloadToEsp(list[b.dataset.dl]); toast('Stored on the ESP32'); await load(); renderRemote(); } catch (err) { toast(err.message, 'err'); } b.disabled = false; }));
    };
    $q('#fw-refresh-manifest').onclick = async () => { try { await fetchManifest(); toast('Manifest updated'); await load(); renderRemote(); } catch (e) { toast('GitHub: ' + e.message, 'err'); } };
    $q('#fw-upload').onclick = async () => {
      const f = $q('#fw-file').files[0]; if (!f) return toast('Choose a .bin file', 'err');
      const buf = await f.arrayBuffer(); const u8 = new Uint8Array(buf);
      if (u8.length < 1024 || String.fromCharCode(u8[8], u8[9], u8[10], u8[11]) !== 'KNLT') return toast('Not a Telink OTA image (no KNLT header)', 'err');
      const ok = await modal(`<h3>Upload firmware</h3><dl>${dl([['File', esc(f.name)], ['Size', u8.length + ' bytes'], ['Version label', esc($q('#fw-version').value)], ['Hardware ids', esc($q('#fw-hw').value || '0,3,4,5,10,14 (all LYWSD03MMC)')]])}</dl><p>The ESP32 validates header, size pointer and CRC32 before storing it.</p>`, [{ label: 'Cancel', value: false }, { label: 'Upload', value: true, cls: 'warn' }]);
      if (!ok) return;
      const q = new URLSearchParams({ name: f.name, version: $q('#fw-version').value, hw: $q('#fw-hw').value }).toString();
      $q('#fw-upload-result').textContent = 'Uploading…';
      try {
        const r = await fetch('/api/firmware/upload?' + q, { method: 'POST', headers: { 'Content-Type': 'application/octet-stream' }, body: buf });
        const j = await r.json();
        $q('#fw-upload-result').textContent = r.ok ? `Stored as ${j.id} (${j.size} bytes, CRC32 ${j.crc32})` : `Rejected: ${j.error} ${j.msg || ''}`;
        load();
      } catch (e) { $q('#fw-upload-result').textContent = 'Upload failed: ' + e.message; }
    };
    await load();
    try { if (!S.manifest) { const r = await fetch('/api/firmware/manifest'); S.manifest = await r.json(); } renderRemote(); } catch (e) { /* ignore */ }
  }

  // ---------------------------------------------------------------- diagnostics
  async function viewDiagnostics() {
    S.view = 'diagnostics'; S.current = null; show('diagnostics');
    const load = async () => {
      const s = await api('/status'); S.status = s;
      $q('#diag-list').innerHTML = dl([['ESP32 chip', esc(s.chip) + ' rev ' + s.chip_revision + ' (reset reason ' + s.reset_reason + ')'], ['ESPHome version', esc(s.esphome_version)], ['IP', esc(s.ip)], ['WiFi RSSI', s.wifi_rssi != null ? s.wifi_rssi + ' dBm' : '—'], ['Free heap', s.free_heap + ' B (min ' + s.min_free_heap + ', largest block ' + s.largest_block + ')'], ['Uptime', s.uptime_s + ' s'], ['Time', esc(s.time_str) || 'not synchronised'], ['BLE status', esc(s.ble_state) + ' / client ' + esc(s.ble_client)], ['Session', esc(s.session) + ' ' + esc(s.job) + ' ' + esc(s.session_device)], ['Held connection', esc(s.hold_device) || '—'], ['Number of devices', s.device_count], ['Last BLE scan', fmtTime(s.last_scan)], ['Last OTA', fmtTime(s.last_ota) + ' ' + esc(s.last_ota_result)], ['Last error', s.last_error ? esc(s.last_error + ': ' + s.last_error_message) : 'none'], ['Job queue', s.queue], ['Firmware store', s.fwstore ? 'available, ' + s.fwstore_capacity + ' B' : 'missing'], ['Manifest', 'version ' + esc(s.manifest_version) + ' (' + esc(s.manifest_source) + ')'], ['GUI assets', esc(s.assets_url) || 'embedded in flash'], ['Test device', esc(s.test_device)]]);
    };
    $q('#diag-refresh').onclick = load;
    await load();
  }

  // ---------------------------------------------------------------- router
  function route() {
    const h = location.hash || '#/';
    let m;
    if ((m = h.match(/^#\/device\/([0-9A-Fa-f:]{17})$/))) viewDevice(m[1].toUpperCase());
    else { if (PV.connected || PV.connecting) pvDisconnect(); if (h === '#/firmware') viewFirmware(); else if (h === '#/diagnostics') viewDiagnostics(); else viewDevices(); }
  }
  window.addEventListener('hashchange', route);

  // ---------------------------------------------------------------- boot
  (async () => {
    try { S.status = await api('/status'); setBadge(S.status.session, S.status.job, S.status.session_device); } catch (e) { /* ignore */ }
    try { const l = await api('/log'); l.lines.forEach((x) => { S.logSeq = x.seq; addLog(x.t, x.msg); }); } catch (e) { /* ignore */ }
    try { S.firmware = await api('/firmware'); } catch (e) { /* ignore */ }
    route();
    connectEvents();
    setInterval(() => { if (S.view === 'devices') refreshDevices(); else if (S.view === 'device') refreshDevice(true); }, 10000);
  })();
})();
