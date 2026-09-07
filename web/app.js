/* Xiaomi ESP Flasher – web GUI (no external dependencies, works offline). */
(function () {
  'use strict';
  const $ = (s, r) => (r || document).querySelector(s);
  const $$ = (s, r) => Array.from((r || document).querySelectorAll(s));
  const esc = (s) => String(s == null ? '' : s).replace(/[&<>"]/g, (c) => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'}[c]));
  const fmtTime = (epoch) => epoch ? new Date(epoch * 1000).toLocaleString() : '—';
  const clock = () => new Date().toLocaleTimeString();

  // ---------------------------------------------------------------- state
  const S = { devices: [], firmware: [], status: {}, current: null, currentFull: null, logSeq: 0, ota: null, view: 'devices', ctab: 'general', tab: 'overview', cfgDirty: false };

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
  const logEl = $('#log');
  function addLog(t, msg) {
    const ts = t ? new Date(t * 1000).toLocaleTimeString() : clock();
    logEl.textContent += `[${ts}] ${msg}\n`;
    if (logEl.textContent.length > 60000) logEl.textContent = logEl.textContent.slice(-40000);
    logEl.scrollTop = logEl.scrollHeight;
  }
  let toastTimer;
  function toast(msg, kind) {
    const t = $('#toast');
    t.textContent = msg;
    t.className = 'toast ' + (kind || '');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => t.classList.add('hidden'), 4000);
  }
  $('#log-clear').onclick = () => (logEl.textContent = '');
  $('#log-toggle').onclick = (e) => { logEl.classList.toggle('hidden'); e.target.textContent = logEl.classList.contains('hidden') ? 'Show' : 'Hide'; };

  // ---------------------------------------------------------------- theme
  const applyTheme = () => document.documentElement.setAttribute('data-theme', localStorage.getItem('theme') || (matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light'));
  $('#theme-toggle').onclick = () => { localStorage.setItem('theme', document.documentElement.getAttribute('data-theme') === 'dark' ? 'light' : 'dark'); applyTheme(); };
  applyTheme();

  // ---------------------------------------------------------------- modal
  function modal(html, buttons) {
    return new Promise((resolve) => {
      const box = $('#modal-box');
      box.innerHTML = html + '<div class="btnrow modal-btns"></div>';
      const row = $('.modal-btns', box);
      buttons.forEach((b) => {
        const btn = document.createElement('button');
        btn.textContent = b.label; btn.className = b.cls || '';
        btn.onclick = () => { $('#modal').classList.add('hidden'); resolve(b.value); };
        row.appendChild(btn);
      });
      $('#modal').classList.remove('hidden');
    });
  }

  // ---------------------------------------------------------------- SSE / polling
  let es = null, pollTimer = null;
  function connectEvents() {
    if (!window.EventSource) return startPolling();
    es = new EventSource('/api/events');
    es.onopen = () => { $('#conn-state').textContent = 'live'; stopPolling(); };
    es.onerror = () => { $('#conn-state').textContent = 'reconnecting…'; startPolling(); };
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
    } catch (e) { $('#conn-state').textContent = 'offline'; }
  }
  function handleEvent(ev) {
    switch (ev.type) {
      case 'log': if (ev.seq > S.logSeq) { S.logSeq = ev.seq; addLog(ev.t, ev.msg); } break;
      case 'state': setBadge(ev.state, ev.job, ev.mac); if (ev.state === 'SUCCESS' || ev.state === 'ERROR') { refreshDevices(); if (S.current) refreshDevice(true); } break;
      case 'ota_progress': S.ota = ev; renderProgress(); if (ev.session) setBadge(ev.session, 'flash', ev.device); break;
      case 'device': if (S.view === 'devices') refreshDevices(); if (S.current && S.current.toUpperCase() === (ev.mac || '').toUpperCase()) refreshDevice(true); break;
      case 'result': toast(ev.msg, ev.ok ? 'ok' : 'err'); if (!ev.ok) addLog(0, 'ERROR ' + (ev.error || '') + ' ' + ev.msg); refreshDevices(); if (S.current) refreshDevice(true); break;
      default: break;
    }
  }
  function setBadge(state, job, mac) {
    const b = $('#session-badge');
    b.textContent = state + (job && job !== 'none' && state !== 'IDLE' ? ' · ' + job + (mac ? ' ' + mac : '') : '');
    b.className = 'badge ' + (state === 'ERROR' ? 'err' : state === 'SUCCESS' || state === 'IDLE' ? 'ok' : 'busy');
  }

  // ---------------------------------------------------------------- progress rendering
  function renderProgress() {
    const p = S.ota; const box = $('#progress-box');
    if (!p || !box) return;
    const active = p.state === 'starting' || p.state === 'writing' || p.state === 'checking' || p.state === 'finishing';
    const stage = { CONNECTING: 'Connecting...', CONNECTED: 'Connected', IDENTIFYING: 'Identifying...', ACTIVATING: 'Activating...', READY: 'Ready', ERASING: 'Erasing...', WRITING: `Writing ${Math.floor(p.progress || 0)}%`, VERIFYING: 'Verifying...', REBOOTING: 'Rebooting...', RECONNECTING: 'Reconnecting...', SUCCESS: 'Done', ERROR: 'Error' }[p.session] || p.session;
    box.classList.remove('hidden');
    $('.pbar i', box).style.width = (p.progress || 0) + '%';
    $('.pstage', box).textContent = stage;
    const secs = ((p.elapsed_ms || 0) / 1000).toFixed(1);
    $('.pinfo', box).textContent = `${p.bytes_sent || 0} / ${p.total_bytes || 0} bytes · ${Math.round(p.speed || 0)} B/s · ${secs} s · retries ${p.retries || 0}` + (p.last_error ? ` · last error ${p.last_error}` : '') + (active ? '' : ` · ${p.state}`);
  }

  // ---------------------------------------------------------------- views
  function show(tpl) { const v = $('#view'); v.innerHTML = ''; v.appendChild($('#tpl-' + tpl).content.cloneNode(true)); $$('nav a').forEach((a) => a.classList.toggle('active', a.dataset.nav === (tpl === 'device' ? 'devices' : tpl))); }

  async function refreshDevices() {
    try { S.devices = await api('/devices'); } catch (e) { return; }
    if (S.view !== 'devices') return;
    const tb = $('#dev-rows'); if (!tb) return;
    $('#dev-count').textContent = S.devices.length + ' device(s)';
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
    $('#btn-scan').onclick = async () => { await post('/scan'); toast('Scan requested'); };
    $('#btn-refresh').onclick = refreshDevices;
    $('#btn-update-all').onclick = async () => {
      const list = S.devices.filter((d) => d.update_available && d.compatible);
      if (!list.length) return toast('No device with an available and compatible update', 'err');
      const ok = await modal(`<h3>Update all</h3><p>The following devices will be updated <b>sequentially</b>, each one verified before the next starts:</p><ul>${list.map((d) => `<li>${esc(d.display_name)} (${esc(d.mac)}): ${esc(d.firmware)} → ${esc(d.latest_version)}</li>`).join('')}</ul><p class="warnbox">Do not move the devices out of BLE range during the update.</p>`, [{ label: 'Cancel', value: false }, { label: 'Start updates', value: true, cls: 'danger' }]);
      if (ok) { await post('/queue/all'); toast('Update queue started'); }
    };
    $('#btn-add').onclick = async () => {
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
    $('#d-title').textContent = (d.display_name || d.mac) + (d.alias ? ` (${d.name})` : '');
    $('#d-info').innerHTML = dl([['Name', esc(d.name)], ['Alias', esc(d.alias)], ['MAC', `<span class="mono">${esc(d.mac)}</span>`], ['Model', esc(d.model)], ['Hardware revision', d.hardware ? esc(d.hardware) + ` <span class="muted">(pvvx id ${d.hw_id}: ${esc(d.hw_name)})</span>` : (d.identified ? esc(d.hw_name) : 'not identified')], ['Firmware', d.identified ? esc(d.firmware_kind + ' ' + d.firmware) : '?'], ['Advertising', esc(d.adv_format) + (d.adv_encrypted ? ' (encrypted)' : '')], ['Connection state', esc(d.session || d.status)], ['Last seen', fmtTime(d.last_seen) + (d.last_seen_ago >= 0 ? ` (${d.last_seen_ago}s ago)` : '')]]);
    $('#d-meas').innerHTML = dl([['Temperature', d.temperature != null ? d.temperature.toFixed(2) + ' °C' : null], ['Humidity', d.humidity != null ? d.humidity.toFixed(2) + ' %' : null], ['Battery', d.battery != null ? d.battery + ' %' : null], ['Battery voltage', d.battery_mv ? (d.battery_mv / 1000).toFixed(3) + ' V' : null], ['RSSI', d.rssi + ' dBm'], ['Live (GATT)', d.live ? `${d.live.temperature.toFixed(2)} °C / ${d.live.humidity.toFixed(2)} % / ${d.live.battery_mv} mV` : null]]);
    const fwLines = [['Installed', d.identified ? esc(d.firmware_kind + ' ' + d.firmware) : 'unknown'], ['Available', esc(d.latest_version)], ['Compatible', d.identified ? (d.compatible ? '<span class="tag ok">yes</span>' : `<span class="tag err">${esc(d.compat_code)}</span> ${esc(d.compat_message)}`) : '—'], ['Update', d.update_available ? '<span class="tag warn">Update available</span>' : d.identified && d.compatible ? '<span class="tag ok">Up-to-date</span>' : '—'], ['Last OTA', d.ota_result ? esc(d.ota_result) + ' · ' + fmtTime(d.last_ota) : null], ['Last error', d.last_error ? esc(d.last_error + ': ' + d.last_error_message) : null]];
    $('#d-fw').innerHTML = dl(fwLines);
    $('#d-update').disabled = !(d.update_available && d.compatible);
    // firmware tab
    $('#d-fwcheck').innerHTML = dl([['Detected hardware', d.hardware ? esc(d.hardware) + ` (id ${d.hw_id})` : 'unknown'], ['Installed firmware', d.identified ? esc(d.firmware_kind + ' ' + d.firmware) : 'unknown'], ['Compatibility check', d.identified ? (d.compatible ? '<span class="tag ok">PASSED</span>' : '<span class="tag err">FAILED</span> ' + esc(d.compat_message)) : 'identify first'], ['Warnings', (d.compat_warnings || []).map(esc).join('<br>') || null], ['Needs Mi cloud token', d.requires_cloud_token ? 'YES – activation blocked' : 'no'], ['Ext. OTA (>128 KiB)', d.big_ota ? 'supported' : 'no']]);
    fillFwSelect(d);
    $('#d-activate').style.display = d.firmware_kind === 'Xiaomi original' || d.firmware_kind === 'Telink (unknown)' ? '' : 'none';
    // config
    if (!S.cfgDirty) fillConfig(d);
    $('#cfg-note').textContent = d.cfg ? `Firmware ${d.cfg.version}: ${d.cfg.offsets_in_cfg ? 'offsets are part of the main config (×0.1)' : 'offsets are stored in the sensor calibration block (×0.01, fw ≥ 4.7)'}.` : (d.identified ? 'Configuration requires pvvx custom firmware.' : 'Read the configuration first.');
    $('#d-raw').textContent = JSON.stringify(d, null, 2);
  }

  function fillFwSelect(d) {
    const sel = $('#d-fwsel'); const prev = sel.value;
    const list = S.firmware.filter((f) => f.available && (f.hw_ids || []).includes(d.hw_id));
    const others = S.firmware.filter((f) => !list.includes(f));
    sel.innerHTML = list.map((f) => `<option value="${esc(f.id)}" ${f.id === d.latest_firmware ? 'selected' : ''}>${esc(f.name)} · v${esc(f.version)} · ${esc(f.kind)} · ${f.size} B</option>`).join('') + (others.length ? '<optgroup label="not compatible / not downloaded">' + others.map((f) => `<option value="${esc(f.id)}" disabled>${esc(f.name)} · v${esc(f.version)} · ${esc(f.kind)}${f.available ? '' : ' (not downloaded)'}</option>`).join('') + '</optgroup>' : '');
    if (prev && list.some((f) => f.id === prev)) sel.value = prev;
    $('#d-flash').disabled = !(d.identified && d.compatible && sel.value);
  }

  const RF = [[63, 'VBAT+10.46'], [61, 'VBAT+10.29'], [58, 'VBAT+10.01'], [56, 'VBAT+9.81'], [53, 'VBAT+9.48'], [51, 'VBAT+9.24'], [49, 'VBAT+8.97'], [47, 'VBAT+8.73'], [45, 'VBAT+8.44'], [43, 'VBAT+8.13'], [41, 'VBAT+7.79'], [39, 'VBAT+7.41'], [37, 'VBAT+7.02'], [35, 'VBAT+6.60'], [33, 'VBAT+6.14'], [31, 'VBAT+5.65'], [29, 'VBAT+5.13'], [27, 'VBAT+4.57'], [25, 'VBAT+3.94'], [23, 'VBAT+3.23'], [191, 'VANT+3.01'], [189, 'VANT+2.81'], [187, 'VANT+2.61'], [185, 'VANT+2.39'], [182, 'VANT+1.99'], [180, 'VANT+1.73'], [178, 'VANT+1.45'], [176, 'VANT+1.17'], [174, 'VANT+0.90'], [172, 'VANT+0.58'], [169, 'VANT+0.04'], [168, 'VANT-0.14'], [164, 'VANT-0.97'], [161, 'VANT-1.42'], [158, 'VANT-1.83'], [156, 'VANT-2.51'], [154, 'VANT-3.25'], [152, 'VANT-4.10'], [148, 'VANT-5.20'], [146, 'VANT-6.02'], [143, 'VANT-7.47'], [140, 'VANT-8.10'], [138, 'VANT-9.30'], [135, 'VANT-10.30'], [130, 'VANT-13.73']];
  function advTypes(ver) {
    // TelinkMiFlasher.html CustomConfig() option list for LCD devices
    return [[0, 'ATC1441'], [1, 'PVVX (Custom)'], [2, 'MIJIA (MiHome)'], [3, ver >= 0x45 ? 'BTHome v2' : ver >= 0x37 ? 'BTHome v1' : 'All']];
  }
  const setV = (id, v) => { const e = $('#c-' + id); if (!e) return; if (e.type === 'checkbox') e.checked = !!v; else e.value = v == null ? '' : v; };
  const getV = (id) => { const e = $('#c-' + id); if (!e) return null; return e.type === 'checkbox' ? e.checked : e.value; };

  function fillConfig(d) {
    const c = d.cfg;
    const rf = $('#c-rf_tx_power'); rf.innerHTML = RF.map(([v, l]) => `<option value="${v}">${l} dBm (${v})</option>`).join('');
    const at = $('#c-advertising_type'); at.innerHTML = advTypes(c ? c.ver : 0x59).map(([v, l]) => `<option value="${v}">${l}</option>`).join('');
    $$('#cfg-form input, #cfg-form select').forEach((e) => (e.disabled = !c));
    if (!c) return;
    const offCfg = c.offsets_in_cfg;
    if (offCfg) { setV('temp_offset', (c.temp_offset / 10).toFixed(1)); setV('humi_offset', (c.humi_offset / 10).toFixed(1)); }
    else if (d.sensor) { setV('temp_offset', (d.sensor.temp_z / 100).toFixed(2)); setV('humi_offset', (d.sensor.humi_z / 100).toFixed(2)); }
    setV('measure_interval', c.measure_interval); setV('av_meas_mem', c.av_meas_mem);
    setV('tx_measures', c.tx_measures); setV('lp_measures', c.lp_measures);
    setV('temp_fahrenheit', c.temp_fahrenheit ? 1 : 0); setV('smiley', c.smiley); setV('comfort_smiley', c.comfort_smiley);
    setV('show_battery', c.show_battery); setV('show_clock', c.show_clock); setV('screen_off', c.screen_off);
    setV('lcd_tint', (c.lcd_tint * 0.05).toFixed(2));
    setV('advertising_type', c.advertising_type); setV('advertising_interval', (c.advertising_interval * 62.5).toFixed(1));
    setV('adv_flags', c.adv_flags); setV('adv_crypto', c.adv_crypto); setV('bt5phy', c.bt5phy); setV('longrange', c.longrange);
    setV('rf_tx_power', c.rf_tx_power); setV('connect_latency', (c.connect_latency + 1) * 20);
    if (d.comfort) { setV('cmf_temp_lo', d.comfort.temp_lo.toFixed(2)); setV('cmf_temp_hi', d.comfort.temp_hi.toFixed(2)); setV('cmf_humi_lo', d.comfort.humi_lo.toFixed(2)); setV('cmf_humi_hi', d.comfort.humi_hi.toFixed(2)); }
    if (d.sensor) { setV('temp_k', d.sensor.temp_k); setV('temp_z', d.sensor.temp_z); setV('humi_k', d.sensor.humi_k); setV('humi_z', d.sensor.humi_z); }
    if (d.trigger) { setV('trg_temp_thr', (d.trigger.temp_threshold / 100).toFixed(2)); setV('trg_humi_thr', (d.trigger.humi_threshold / 100).toFixed(2)); setV('trg_temp_hst', (d.trigger.temp_hysteresis / 100).toFixed(2)); setV('trg_humi_hst', (d.trigger.humi_hysteresis / 100).toFixed(2)); setV('rds_type', d.trigger.rds_type & 3); setV('rds_invert', !!(d.trigger.rds_type & 0x10)); setV('rds_rpint', d.trigger.rds_rpint); }
    setV('devname', d.device_name);
    $('#c-time').innerHTML = dl([['Device clock', d.device_time ? new Date(d.device_time * 1000).toISOString().replace('T', ' ').slice(0, 19) + ' (device local)' : 'not set'], ['Last set', d.device_time_set ? new Date(d.device_time_set * 1000).toISOString().replace('T', ' ').slice(0, 19) : '—'], ['ESP32 time', esc(S.status.time_str || '?')]]);
  }

  function setLocalDefaults() {
    // def_cfg of the pvvx firmware (app.c) – applied to the local form only
    setV('temp_offset', 0); setV('humi_offset', 0); setV('measure_interval', 4); setV('av_meas_mem', 180);
    setV('tx_measures', false); setV('lp_measures', true); setV('temp_fahrenheit', 0); setV('smiley', 0); setV('comfort_smiley', true);
    setV('show_battery', true); setV('show_clock', false); setV('screen_off', false); setV('lcd_tint', '2.45');
    setV('advertising_type', 3); setV('advertising_interval', '2500.0'); setV('adv_flags', true); setV('adv_crypto', false);
    setV('bt5phy', false); setV('longrange', false); setV('rf_tx_power', 191); setV('connect_latency', 1000);
    setV('cmf_temp_lo', '21.00'); setV('cmf_temp_hi', '26.00'); setV('cmf_humi_lo', '30.00'); setV('cmf_humi_hi', '60.00');
    S.cfgDirty = true;
    toast('Defaults loaded into the form – nothing sent yet');
  }

  function collectConfig(d) {
    const c = d.cfg; if (!c) throw new Error('read the config first');
    const cfg = { ver: c.ver, flg: c.flg, flg2: c.flg2, hver: c.hver, temp_offset: c.temp_offset, humi_offset: c.humi_offset,
      advertising_interval: Math.round(parseFloat(getV('advertising_interval')) / 62.5),
      measure_interval: parseInt(getV('measure_interval'), 10),
      rf_tx_power: parseInt(getV('rf_tx_power'), 10),
      connect_latency: Math.round(parseFloat(getV('connect_latency')) / 20 - 1),
      lcd_tint: Math.round(parseFloat(getV('lcd_tint')) / 0.05),
      av_meas_mem: parseInt(getV('av_meas_mem'), 10) || 0,
      advertising_type: parseInt(getV('advertising_type'), 10),
      comfort_smiley: getV('comfort_smiley'), show_clock: getV('show_clock'), temp_fahrenheit: getV('temp_fahrenheit') == 1,
      show_battery: getV('show_battery'), tx_measures: getV('tx_measures'), lp_measures: getV('lp_measures'),
      smiley: parseInt(getV('smiley'), 10), adv_crypto: getV('adv_crypto'), adv_flags: getV('adv_flags'), bt5phy: getV('bt5phy'), longrange: getV('longrange'), screen_off: getV('screen_off') };
    const body = { cfg };
    if (c.offsets_in_cfg) { cfg.temp_offset = Math.round(parseFloat(getV('temp_offset') || 0) * 10); cfg.humi_offset = Math.round(parseFloat(getV('humi_offset') || 0) * 10); }
    else if (d.sensor) { body.sensor = { temp_k: d.sensor.temp_k, humi_k: d.sensor.humi_k, temp_z: Math.round(parseFloat(getV('temp_offset') || 0) * 100), humi_z: Math.round(parseFloat(getV('humi_offset') || 0) * 100) }; }
    body.comfort = { temp_lo: parseFloat(getV('cmf_temp_lo')), temp_hi: parseFloat(getV('cmf_temp_hi')), humi_lo: parseFloat(getV('cmf_humi_lo')), humi_hi: parseFloat(getV('cmf_humi_hi')) };
    if (getV('send-advanced')) {
      body.sensor = { temp_k: parseInt(getV('temp_k'), 10), humi_k: parseInt(getV('humi_k'), 10), temp_z: parseInt(getV('temp_z'), 10), humi_z: parseInt(getV('humi_z'), 10) };
      body.trigger = { temp_threshold: Math.round(parseFloat(getV('trg_temp_thr')) * 100), humi_threshold: Math.round(parseFloat(getV('trg_humi_thr')) * 100), temp_hysteresis: Math.round(parseFloat(getV('trg_temp_hst')) * 100), humi_hysteresis: Math.round(parseFloat(getV('trg_humi_hst')) * 100), rds_rpint: parseInt(getV('rds_rpint'), 10), rds_type: (parseInt(getV('rds_type'), 10) & 3) | (getV('rds_invert') ? 0x10 : 0) };
    }
    return body;
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
  function switchCTab(t) { S.ctab = t; $$('#cfg-tabs button').forEach((b) => b.classList.toggle('active', b.dataset.ctab === t)); $$('.ctab').forEach((s) => s.classList.toggle('hidden', s.dataset.ctab !== t)); }

  async function viewDevice(mac) {
    S.view = 'device'; S.current = mac; S.cfgDirty = false; show('device');
    $('#view').insertAdjacentHTML('afterbegin', '<div id="progress-box" class="progress hidden"><div class="pstage"></div><div class="pbar"><i></i></div><div class="pinfo muted"></div></div>');
    if (!S.firmware.length) { try { S.firmware = await api('/firmware'); } catch (e) { /* ignore */ } }
    $$('#d-tabs button').forEach((b) => (b.onclick = () => switchTab(b.dataset.tab)));
    $$('#cfg-tabs button').forEach((b) => (b.onclick = () => switchCTab(b.dataset.ctab)));
    $('#cfg-form').addEventListener('input', () => (S.cfgDirty = true));
    const act = (path, body, msg) => async () => { try { await post('/device/' + mac + path, body); toast(msg || 'Requested'); } catch (e) { toast(e.message, 'err'); } };
    $('#d-identify').onclick = act('/identify', {}, 'Identification queued');
    $('#d-config-go').onclick = () => switchTab('config');
    $('#d-diag-go').onclick = () => switchTab('diag');
    $('#d-update').onclick = () => { switchTab('firmware'); };
    $('#d-alias').onclick = async () => { const a = prompt('Alias for this device:', (S.currentFull && S.currentFull.alias) || ''); if (a != null) { await post('/device/' + mac + '/alias', { alias: a }); refreshDevice(true); } };
    $('#d-forget').onclick = async () => { if (confirm('Forget this device?')) { await post('/device/' + mac + '/forget'); location.hash = '#/'; } };
    $('#d-flash').onclick = () => { const d = S.currentFull; const fw = S.firmware.find((f) => f.id === $('#d-fwsel').value); if (!d || !fw) return; const keys = {}; if ($('#d-token').value && $('#d-bindkey').value) { keys.token = $('#d-token').value; keys.bind_key = $('#d-bindkey').value; } confirmFlash(d, fw, keys); };
    $('#d-fwsel').onchange = () => { if (S.currentFull) fillFwSelect(S.currentFull); };
    $('#d-activate').onclick = async () => { const ok = await modal('<h3>Do Activation</h3><p>Performs the Mi registration (ECDH key exchange) with the device. After this the device must be re-added in Mi Home if you want to use it there. The obtained token/bindkey are printed in the log.</p>', [{ label: 'Cancel', value: false }, { label: 'Activate', value: true, cls: 'warn' }]); if (ok) { const keys = {}; if ($('#d-token').value && $('#d-bindkey').value) { keys.token = $('#d-token').value; keys.bind_key = $('#d-bindkey').value; } act('/activate', keys, 'Activation queued')(); } };
    $('#cfg-read').onclick = () => { S.cfgDirty = false; act('/identify', {}, 'Read Config queued')(); };
    $('#cfg-defaults').onclick = setLocalDefaults;
    $('#cfg-device-defaults').onclick = async () => { if (await modal('<h3>Reset device configuration</h3><p>Sends <code>56</code> (CMD_ID_CFG_DEF) – the thermometer restores its factory configuration.</p>', [{ label: 'Cancel', value: false }, { label: 'Reset', value: true, cls: 'danger' }])) act('/defaults', {}, 'Reset queued')(); };
    $('#cfg-send').onclick = async () => {
      const d = S.currentFull; if (!d) return;
      let body; try { body = collectConfig(d); } catch (e) { return toast(e.message, 'err'); }
      const ok = await modal(`<h3>Send Config</h3><p>The configuration will be written to <b>${esc(d.display_name)}</b>, read back and compared.</p><pre>${esc(JSON.stringify(body, null, 1))}</pre>`, [{ label: 'Cancel', value: false }, { label: 'Send Config', value: true, cls: 'warn' }]);
      if (ok) { S.cfgDirty = false; act('/config', body, 'Configuration write queued')(); }
    };
    $('#c-settime').onclick = act('/settime', {}, 'Set time queued');
    $('#c-setname').onclick = () => act('/name', { name: getV('devname') || '' }, 'Set name queued')();
    $('#c-setpin').onclick = async () => { const pin = getV('pin'); if (!/^[0-9]{6}$/.test(pin)) return toast('PIN must be 6 digits', 'err'); if (await modal('<h3>Set PIN</h3><p class="warnbox">If the PIN is forgotten only a hardware flasher can recover the device.</p>', [{ label: 'Cancel', value: false }, { label: 'Set PIN', value: true, cls: 'danger' }])) act('/pin', { pin: parseInt(pin, 10) }, 'PIN queued')(); };
    $('#c-reboot').onclick = act('/reboot', {}, 'Reboot queued');
    await refreshDevice(false);
    switchTab(S.tab || 'overview'); switchCTab('general');
    renderProgress();
  }

  // ---------------------------------------------------------------- firmware page
  async function viewFirmware() {
    S.view = 'firmware'; S.current = null; show('firmware');
    const load = async () => {
      S.firmware = await api('/firmware');
      $('#fw-rows').innerHTML = S.firmware.map((f) => `<tr class="${f.available ? '' : 'dim'}"><td class="mono small">${esc(f.id)}</td><td>${esc(f.name)}</td><td>${esc(f.version)}</td><td>${esc(f.kind)}</td><td>${f.size || '—'}</td><td class="mono">${esc(f.crc32)}</td><td class="small">${(f.hw_ids || []).map((i, k) => `${i}:${esc(f.hw_names[k])}`).join(', ')}</td><td class="small">${esc(f.source)}</td></tr>`).join('');
    };
    $('#fw-check').onclick = async () => { await post('/firmware/check'); toast('Online check requested – see log'); setTimeout(load, 8000); };
    $('#fw-upload').onclick = async () => {
      const f = $('#fw-file').files[0]; if (!f) return toast('Choose a .bin file', 'err');
      const buf = await f.arrayBuffer(); const u8 = new Uint8Array(buf);
      // client-side pre-check (same as testOTAFirmware): 'KNLT' at 0x08
      if (u8.length < 1024 || String.fromCharCode(u8[8], u8[9], u8[10], u8[11]) !== 'KNLT') return toast('Not a Telink OTA image (no KNLT header)', 'err');
      const ok = await modal(`<h3>Upload firmware</h3><dl>${dl([['File', esc(f.name)], ['Size', u8.length + ' bytes'], ['Version label', esc($('#fw-version').value)], ['Hardware ids', esc($('#fw-hw').value || '0,3,4,5,10,14 (all LYWSD03MMC)')]])}</dl><p>The ESP32 validates header, size pointer and CRC32 before storing it.</p>`, [{ label: 'Cancel', value: false }, { label: 'Upload', value: true, cls: 'warn' }]);
      if (!ok) return;
      const q = new URLSearchParams({ name: f.name, version: $('#fw-version').value, hw: $('#fw-hw').value }).toString();
      $('#fw-upload-result').textContent = 'Uploading…';
      try {
        const r = await fetch('/api/firmware/upload?' + q, { method: 'POST', headers: { 'Content-Type': 'application/octet-stream' }, body: buf });
        const j = await r.json();
        $('#fw-upload-result').textContent = r.ok ? `Stored as ${j.id} (${j.size} bytes, CRC32 ${j.crc32})` : `Rejected: ${j.error} ${j.msg || ''}`;
        load();
      } catch (e) { $('#fw-upload-result').textContent = 'Upload failed: ' + e.message; }
    };
    await load();
  }

  // ---------------------------------------------------------------- diagnostics
  async function viewDiagnostics() {
    S.view = 'diagnostics'; S.current = null; show('diagnostics');
    const load = async () => {
      const s = await api('/status'); S.status = s;
      $('#diag-list').innerHTML = dl([['ESP32 chip', esc(s.chip) + ' rev ' + s.chip_revision], ['ESPHome version', esc(s.esphome_version)], ['IP', esc(s.ip)], ['WiFi', esc(s.wifi_ssid) + ' ' + (s.wifi_rssi != null ? s.wifi_rssi + ' dBm' : '')], ['Free heap', s.free_heap + ' B (min ' + s.min_free_heap + ', largest block ' + s.largest_block + ')'], ['Uptime', s.uptime_s + ' s'], ['Time', esc(s.time_str) || 'not synchronised'], ['BLE status', esc(s.ble_state) + ' / client ' + esc(s.ble_client)], ['Session', esc(s.session) + ' ' + esc(s.job) + ' ' + esc(s.session_device)], ['Number of devices', s.device_count], ['Last BLE scan', fmtTime(s.last_scan)], ['Last OTA', fmtTime(s.last_ota) + ' ' + esc(s.last_ota_result)], ['Last error', s.last_error ? esc(s.last_error + ': ' + s.last_error_message) : 'none'], ['Job queue', s.queue], ['Firmware store', s.fwstore ? 'available, ' + s.fwstore_capacity + ' B' : 'missing'], ['Bundled manifest', 'version ' + esc(s.manifest_version)], ['Remote manifest', esc(s.remote_manifest) || 'disabled'], ['Test device', esc(s.test_device)]]);
    };
    $('#diag-refresh').onclick = load;
    await load();
  }

  // ---------------------------------------------------------------- router
  function route() {
    const h = location.hash || '#/';
    let m;
    if ((m = h.match(/^#\/device\/([0-9A-Fa-f:]{17})$/))) viewDevice(m[1].toUpperCase());
    else if (h === '#/firmware') viewFirmware();
    else if (h === '#/diagnostics') viewDiagnostics();
    else viewDevices();
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
