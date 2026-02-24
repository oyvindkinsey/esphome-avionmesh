const API = '/api/';
let devices = [], groups = [], bleState = 0;
let meshMqttExposed = false;
let unassocHashes = [];
let claimingHash = null;
let evtSource = null;

const BLE_NAMES = ['Idle', 'Scanning', 'Connecting', 'Discovering', 'Ready', 'Disconnected'];
const PRODUCTS = [
  {v:0,n:'Group'}, {v:90,n:'Lamp Dimmer'}, {v:93,n:'Recessed DL'},
  {v:94,n:'Light Adapter'}, {v:97,n:'Smart Dimmer'}, {v:134,n:'Smart Bulb'},
  {v:137,n:'Surface DL'}, {v:162,n:'Micro Edge'}, {v:167,n:'Smart Switch'}
];

const $ = id => document.getElementById(id);
const esc = s => { const d = document.createElement('div'); d.textContent = s; return d.innerHTML };

async function api(path, opts) {
  const r = await fetch(API + path, opts);
  if (!r.ok) {
    const err = await r.json().catch(() => ({error: r.statusText}));
    const msg = err.error || err.message || 'API error ' + r.status;
    if (msg === 'mesh_not_initialized') throw new Error('Mesh not initialized — set a passphrase first');
    if (msg === 'ble_not_ready') throw new Error('BLE not ready');
    throw new Error(msg);
  }
  const text = await r.text();
  feedLog('← ' + text.substring(0, 120));
  return JSON.parse(text);
}

async function postJson(path, data) {
  return api(path, {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(data)});
}

function showMsg(el, text, ok) {
  el.className = 'msg ' + (ok ? 'msg-ok' : 'msg-err');
  el.textContent = text;
  setTimeout(() => el.textContent = '', 5000);
}

function productSelect(selected) {
  return '<select class="prod-sel">' +
    PRODUCTS.filter(p => p.v !== 0)
      .map(p => `<option value="${p.v}"${p.v === selected ? ' selected' : ''}>${p.n}</option>`)
      .join('') +
    '</select>';
}

function feedLog(text) {
  const feed = $('eventFeed');
  const d = document.createElement('div');
  d.textContent = new Date().toLocaleTimeString() + ' ' + text;
  feed.prepend(d);
  while (feed.children.length > 50) feed.lastChild.remove();
}

/* ── Status bar ──────────────────────────── */
function updateStatusBar() {
  const bs = $('bleState');
  bs.textContent = BLE_NAMES[bleState] || '?';
  bs.className = 'v ' + (bleState === 4 ? 'ble-ok' : 'ble-bad');
  $('devCount').textContent = devices.length;
  $('grpCount').textContent = groups.length;
}

/* ── Device cards ────────────────────────── */
function renderDevices() {
  const grid = $('deviceGrid');
  if (!devices.length) {
    grid.innerHTML = '<p class="empty">No devices added yet</p>';
    return;
  }
  grid.innerHTML = devices.map(d => deviceCard(d)).join('');
}

function deviceCard(d) {
  const knownBri = d.brightness !== undefined;
  const on = knownBri && d.brightness > 0;
  const stateText = knownBri ? (on ? `ON · ${d.brightness}` : 'OFF') : '';
  const stateClass = knownBri ? (on ? 'on' : 'off') : 'unknown';
  const hasCT = d.color_temp !== undefined;
  const ctVal = hasCT ? d.color_temp : 2700;

  const grpTags = (d.groups || []).map(gid => {
    const g = groups.find(x => x.group_id === gid);
    return `<span class="tag">${g ? esc(g.name) : gid}</span>`;
  }).join('');

  return `<div class="card" data-avid="${d.avion_id}">
  <div class="card-header">
    <div>
      <div class="card-name">${esc(d.name)}</div>
      <div class="card-sub">${esc(d.product_name)} · #${d.avion_id}</div>
    </div>
    <span id="stPill${d.avion_id}" class="state-pill ${stateClass}">${stateText}</span>
  </div>
  <div class="on-off">
    <button class="sm ghost" onclick="ctrlDev(${d.avion_id},255)">On</button>
    <button class="sm ghost" onclick="ctrlDev(${d.avion_id},0)">Off</button>
    <label class="toggle-row">
      <span class="toggle">
        <input type="checkbox" ${d.mqtt_exposed ? 'checked' : ''} onchange="toggleMqtt(${d.avion_id},this.checked)">
        <span class="knob"></span>
      </span>
      MQTT
    </label>
  </div>
  ${grpTags ? `<div class="tags">${grpTags}</div>` : ''}
  <details class="ctrl">
    <summary>Controls</summary>
    <div class="ctrl-body">
      <div class="slider-row">
        <div class="slider-label">
          <span>Brightness</span>
          <span id="bv${d.avion_id}">${knownBri ? d.brightness : '—'}</span>
        </div>
        <input type="range" min="0" max="255" value="${d.brightness ?? 0}" class="bri-range"
          oninput="$('bv${d.avion_id}').textContent=this.value"
          onchange="ctrlDev(${d.avion_id},+this.value)">
      </div>
      <div class="slider-row">
        <div class="slider-label">
          <span>Color temp</span>
          <span id="cv${d.avion_id}">${hasCT ? ctVal + 'K' : '—'}</span>
        </div>
        <input type="range" min="2000" max="6500" step="100" value="${ctVal}" class="ct-range"
          oninput="$('cv${d.avion_id}').textContent=this.value+'K'"
          onchange="ctrlTemp(${d.avion_id},+this.value)">
      </div>
      <div class="card-actions">
        <button class="sm ghost" id="exBtn${d.avion_id}" onclick="examDev(${d.avion_id})">Examine</button>
        <button class="sm danger" onclick="removeDev(${d.avion_id})">Remove</button>
      </div>
      <div id="exRes${d.avion_id}" style="font-size:.76em;color:var(--accent2)"></div>
    </div>
  </details>
</div>`;
}

function applyDeviceState(d) {
  const on = d.brightness > 0;
  const pill = $('stPill' + d.avion_id);
  if (pill) {
    pill.textContent = on ? `ON · ${d.brightness}` : 'OFF';
    pill.className = 'state-pill ' + (on ? 'on' : 'off');
  }
  const bvEl = $('bv' + d.avion_id);
  const bri = document.querySelector(`.card[data-avid="${d.avion_id}"] .bri-range`);
  if (bri !== document.activeElement) {
    if (bvEl) bvEl.textContent = d.brightness;
    if (bri) bri.value = d.brightness;
  }
  if (d.color_temp !== undefined) {
    const cvEl = $('cv' + d.avion_id);
    const ct = document.querySelector(`.card[data-avid="${d.avion_id}"] .ct-range`);
    if (ct !== document.activeElement) {
      if (cvEl) cvEl.textContent = d.color_temp + 'K';
      if (ct) ct.value = d.color_temp;
    }
  }
}

/* ── Group cards ─────────────────────────── */
function renderGroups() {
  const grid = $('groupGrid');
  const all = {group_id: 0, name: 'All (Broadcast)', members: [], mqtt_exposed: meshMqttExposed};
  grid.innerHTML = [all, ...groups].map(g => groupCard(g)).join('');
}

function groupCard(g) {
  const isAll = g.group_id === 0;

  const memberTags = g.members.map(mid => {
    const d = devices.find(x => x.avion_id === mid);
    return `<span class="tag">${d ? esc(d.name) : mid} <a href="#" onclick="rmFromGrp(${mid},${g.group_id});return false">&times;</a></span>`;
  }).join('');

  const devOpts = devices
    .filter(d => !g.members.includes(d.avion_id))
    .map(d => `<option value="${d.avion_id}">${esc(d.name)}</option>`)
    .join('');

  return `<div class="card">
  <div>
    <div class="card-name">${esc(g.name)}</div>
    <div class="card-sub">Group · #${g.group_id}</div>
  </div>
  <div class="on-off">
    <button class="sm" onclick="ctrlDev(${g.group_id},255)">On</button>
    <button class="sm ghost" onclick="ctrlDev(${g.group_id},0)">Off</button>
  </div>
  <label class="toggle-row">
    <span class="toggle">
      <input type="checkbox" ${g.mqtt_exposed ? 'checked' : ''} onchange="toggleMqtt(${g.group_id},this.checked)">
      <span class="knob"></span>
    </span>
    MQTT
  </label>
  ${!isAll && memberTags ? `<div class="tags">${memberTags}</div>` : ''}
  <details class="ctrl">
    <summary>${isAll ? 'Controls' : 'Controls &amp; Members'}</summary>
    <div class="ctrl-body">
      <div class="slider-row">
        <div class="slider-label">
          <span>Brightness</span>
          <span id="gbv${g.group_id}">—</span>
        </div>
        <input type="range" min="0" max="255" value="128"
          oninput="$('gbv${g.group_id}').textContent=this.value"
          onchange="ctrlDev(${g.group_id},+this.value)">
      </div>
      <div class="slider-row">
        <div class="slider-label">
          <span>Color temp</span>
          <span id="gcv${g.group_id}">—</span>
        </div>
        <input type="range" min="2000" max="6500" step="100" value="2700"
          oninput="$('gcv${g.group_id}').textContent=this.value+'K'"
          onchange="ctrlTemp(${g.group_id},+this.value)">
      </div>
      ${!isAll ? `<div class="member-add">
        ${devOpts
          ? `<select id="addMbr${g.group_id}"><option value="">Add device…</option>${devOpts}</select>
             <button class="sm" onclick="addToGrp(${g.group_id})">Add</button>`
          : '<span style="color:var(--dim);font-size:.8em">All devices in group</span>'}
      </div>` : ''}
    </div>
  </details>
  ${!isAll ? `<div class="card-actions">
    <button class="sm danger" onclick="delGroup(${g.group_id})">Delete</button>
  </div>` : ''}
</div>`;
}

/* ── SSE ─────────────────────────────────── */
function connectSSE() {
  if (evtSource) evtSource.close();
  evtSource = new EventSource(API + 'events');

  evtSource.onopen = () => {
    $('sseDot').className = 'sse-dot sse-on';
    devices = [];
    groups = [];
    feedLog('SSE connected');
  };

  evtSource.onerror = () => {
    $('sseDot').className = 'sse-dot sse-off';
  };

  evtSource.addEventListener('meta', e => {
    const d = JSON.parse(e.data);
    bleState = d.ble_state;
    $('rxCount').textContent = d.rx_count;
    updateStatusBar();
  });

  evtSource.addEventListener('devices', e => {
    JSON.parse(e.data).devices?.forEach(dev => {
      const i = devices.findIndex(x => x.avion_id === dev.avion_id);
      if (i >= 0) devices[i] = dev; else devices.push(dev);
    });
    updateStatusBar();
    renderDevices();
  });

  evtSource.addEventListener('groups', e => {
    JSON.parse(e.data).groups?.forEach(grp => {
      const i = groups.findIndex(x => x.group_id === grp.group_id);
      if (i >= 0) groups[i] = grp; else groups.push(grp);
    });
    updateStatusBar();
    renderGroups();
  });

  evtSource.addEventListener('sync_complete', () => {
    feedLog(`Sync: ${devices.length} devices, ${groups.length} groups`);
    updateStatusBar();
    renderDevices();
    renderGroups();
  });

  evtSource.addEventListener('device_added', e => {
    const d = JSON.parse(e.data);
    const i = devices.findIndex(x => x.avion_id === d.avion_id);
    if (i >= 0) devices[i] = d; else devices.push(d);
    updateStatusBar();
    renderDevices();
    feedLog('Device added: ' + d.name);
  });

  evtSource.addEventListener('device_removed', e => {
    const d = JSON.parse(e.data);
    devices = devices.filter(x => x.avion_id !== d.avion_id);
    updateStatusBar();
    renderDevices();
    renderGroups();
    feedLog('Device removed: #' + d.avion_id);
  });

  evtSource.addEventListener('group_added', e => {
    const d = JSON.parse(e.data);
    const i = groups.findIndex(x => x.group_id === d.group_id);
    if (i >= 0) groups[i] = d; else groups.push(d);
    updateStatusBar();
    renderGroups();
    feedLog('Group added: ' + d.name);
  });

  evtSource.addEventListener('group_removed', e => {
    const d = JSON.parse(e.data);
    groups = groups.filter(x => x.group_id !== d.group_id);
    updateStatusBar();
    renderGroups();
    feedLog('Group removed: #' + d.group_id);
  });

  evtSource.addEventListener('group_updated', e => {
    const d = JSON.parse(e.data);
    const i = groups.findIndex(x => x.group_id === d.group_id);
    if (i >= 0) groups[i] = d; else groups.push(d);
    renderGroups();
  });

  evtSource.addEventListener('discover_mesh', e => {
    const d = JSON.parse(e.data);
    $('btnMeshScan').disabled = false;
    $('btnMeshScan').textContent = 'Mesh Ping Scan';
    renderScanResults(d.devices || []);
    feedLog('Mesh scan: ' + (d.devices?.length || 0) + ' found');
  });

  evtSource.addEventListener('scan_unassoc', e => {
    const d = JSON.parse(e.data);
    $('btnUnassocScan').disabled = false;
    $('btnUnassocScan').textContent = 'Scan for New';
    renderUnassoc(d.uuid_hashes || []);
    feedLog('Unassoc scan: ' + (d.uuid_hashes?.length || 0) + ' found');
  });

  evtSource.addEventListener('claim_result', e => {
    const d = JSON.parse(e.data);
    feedLog('Claim: ' + (d.status === 'ok' ? 'ID ' + d.device_id : d.message || 'failed'));
    if (d.status === 'ok' && claimingHash) {
      const idx = unassocHashes.indexOf(claimingHash);
      if (idx >= 0) {
        $('unassocRow' + idx)?.remove();
        unassocHashes.splice(idx, 1);
      }
      claimingHash = null;
    }
  });

  evtSource.addEventListener('examine', e => {
    const d = JSON.parse(e.data);
    const res = $('exRes' + d.avion_id);
    const btn = $('exBtn' + d.avion_id);
    if (btn) btn.disabled = false;
    if (res) {
      res.innerHTML = d.error
        ? `<span style="color:var(--err)">${esc(d.error)}</span>`
        : `FW ${d.fw} · Vendor ${d.vendor_id} · CSR ${d.csr_product_id} · Flags ${d.flags}`;
      setTimeout(() => res.textContent = '', 15000);
    }
    feedLog('Examine #' + d.avion_id + ': ' + (d.error || 'ok'));
  });

  evtSource.addEventListener('state', e => {
    const d = JSON.parse(e.data);
    const dev = devices.find(x => x.avion_id === d.avion_id);
    if (dev) {
      dev.brightness = d.brightness;
      if (d.color_temp !== undefined) dev.color_temp = d.color_temp;
      applyDeviceState(d);
    }
    const name = dev ? dev.name : '#' + d.avion_id;
    feedLog(name + ': bri=' + d.brightness + (d.color_temp !== undefined ? ' ct=' + d.color_temp + 'K' : ''));
  });

  evtSource.addEventListener('mesh_status', e => {
    meshMqttExposed = JSON.parse(e.data).mesh_mqtt_exposed;
    renderGroups();
  });

  evtSource.addEventListener('save_result', () => {
    $('btnSave').disabled = false;
    $('saveMsg').textContent = 'Saved';
    setTimeout(() => $('saveMsg').textContent = '', 3000);
    feedLog('DB saved');
  });

  evtSource.addEventListener('mqtt_toggled', e => {
    const d = JSON.parse(e.data);
    if (d.id === 0) {
      meshMqttExposed = d.mqtt_exposed;
      renderGroups();
    } else {
      const dev = devices.find(x => x.avion_id === d.id);
      if (dev) { dev.mqtt_exposed = d.mqtt_exposed; renderDevices(); }
      const grp = groups.find(x => x.group_id === d.id);
      if (grp) { grp.mqtt_exposed = d.mqtt_exposed; renderGroups(); }
    }
    feedLog('MQTT ' + (d.mqtt_exposed ? 'on' : 'off') + ': #' + d.id);
  });

  evtSource.addEventListener('import_result', e => {
    const d = JSON.parse(e.data);
    showMsg($('setupMsg'), `Import: ${d.added_devices} devices, ${d.added_groups} groups`, true);
  });

  evtSource.addEventListener('debug', e => {
    feedLog('DBG: ' + e.data);
  });
}

/* ── Device / group control ──────────────── */
async function ctrlDev(id, brightness) {
  await postJson('control', {avion_id: id, brightness});
}

async function ctrlTemp(id, kelvin) {
  if (kelvin) await postJson('control', {avion_id: id, color_temp: kelvin});
}

async function examDev(id) {
  const btn = $('exBtn' + id);
  const res = $('exRes' + id);
  btn.disabled = true;
  res.innerHTML = '<span class="spinner"></span>';
  try {
    await postJson('examine_device', {avion_id: id});
  } catch(e) {
    btn.disabled = false;
    res.textContent = 'Error: ' + e.message;
  }
}

async function removeDev(id) {
  if (!confirm('Remove device ' + id + '?')) return;
  await postJson('unclaim_device', {avion_id: id});
}

async function toggleMqtt(id, exposed) {
  await postJson('set_mqtt_exposed', {id, exposed});
}

/* ── Groups ──────────────────────────────── */
async function createGroup() {
  const input = $('newGrpName');
  const name = input.value.trim();
  if (!name) return;
  await postJson('create_group', {name});
  input.value = '';
}

async function delGroup(id) {
  if (!confirm('Delete group ' + id + '?')) return;
  await postJson('delete_group', {group_id: id});
}

async function addToGrp(gid) {
  const sel = $('addMbr' + gid);
  const mid = +sel.value;
  if (!mid) return;
  await postJson('add_to_group', {avion_id: mid, group_id: gid});
}

async function rmFromGrp(mid, gid) {
  await postJson('remove_from_group', {avion_id: mid, group_id: gid});
}

/* ── Scan ────────────────────────────────── */
async function startMeshScan() {
  const btn = $('btnMeshScan');
  btn.disabled = true;
  btn.innerHTML = '<span class="spinner"></span> Scanning\u2026';
  $('scanResults').classList.remove('hidden');
  $('scanBody').innerHTML = '<tr><td colspan="6"><span class="spinner"></span> Scanning\u2026</td></tr>';
  try {
    await postJson('discover_mesh', {});
  } catch(e) {
    btn.disabled = false;
    btn.textContent = 'Mesh Ping Scan';
    $('scanBody').innerHTML = `<tr><td colspan="6" style="color:var(--err)">Error: ${esc(e.message)}</td></tr>`;
  }
}

function renderScanResults(devs) {
  $('scanResults').classList.remove('hidden');
  const tb = $('scanBody');
  if (!devs.length) {
    tb.innerHTML = '<tr><td colspan="6" class="empty">No devices found</td></tr>';
    return;
  }
  tb.innerHTML = devs.map(d => `<tr>
    <td>${d.device_id}</td>
    <td>${d.fw}</td>
    <td>${d.vendor_id}</td>
    <td>${d.csr_product_id}</td>
    <td>${d.known ? '<span style="color:var(--ok)">Known</span>' : 'New'}</td>
    <td>${d.known ? '' : `<input placeholder="Name" id="sn${d.device_id}" style="width:90px" value="Device ${d.device_id}">
      ${productSelect(0)}
      <button class="sm" onclick="addScanned(${d.device_id})">Add</button>`}</td>
  </tr>`).join('');
}

async function addScanned(did) {
  const name = $('sn' + did).value || ('Device ' + did);
  const sel = $('sn' + did).parentElement.querySelector('.prod-sel');
  const pt = sel ? +sel.value : 134;
  await postJson('add_discovered', {device_id: did, name, product_type: pt});
  showMsg($('scanMsg'), 'Device added', true);
}

async function startUnassocScan() {
  const btn = $('btnUnassocScan');
  btn.disabled = true;
  btn.innerHTML = '<span class="spinner"></span> Scanning\u2026';
  $('unassocResults').classList.remove('hidden');
  $('unassocList').innerHTML = '<span class="spinner"></span>';
  try {
    await postJson('scan_unassociated', {});
  } catch(e) {
    btn.disabled = false;
    btn.textContent = 'Scan for New';
    $('unassocList').innerHTML = `<span style="color:var(--err)">Error: ${esc(e.message)}</span>`;
  }
}

function renderUnassoc(hashes) {
  $('unassocResults').classList.remove('hidden');
  const el = $('unassocList');
  unassocHashes = hashes;
  if (!hashes.length) {
    el.innerHTML = '<p class="empty">No unassociated devices found</p>';
    return;
  }
  el.innerHTML = hashes.map((hash, i) => `<div class="row" id="unassocRow${i}">
    <span class="mono">${hash}</span>
    <input placeholder="Name" id="cn${i}" style="width:100px">
    ${productSelect(134)}
    <button class="sm" id="claimBtn${i}" onclick="claimDev('${hash}',${i})">Claim</button>
    <span id="claimMsg${i}" style="font-size:.8em"></span>
  </div>`).join('');
}

async function claimDev(hash, idx) {
  claimingHash = hash;
  const btn = $('claimBtn' + idx);
  const name = $('cn' + idx).value || 'Unknown Device';
  const sel = $('cn' + idx).parentElement.querySelector('.prod-sel');
  const pt = sel ? +sel.value : 134;
  btn.disabled = true;
  btn.textContent = 'Claiming\u2026';
  $('claimMsg' + idx).textContent = 'Starting\u2026';
  await postJson('claim_device', {uuid_hash: parseInt(hash, 16), name, product_type: pt});
}

/* ── Save / Setup ────────────────────────── */
async function saveDb() {
  $('btnSave').disabled = true;
  $('saveMsg').textContent = 'Saving\u2026';
  try {
    await postJson('save', {});
  } catch(e) {
    $('saveMsg').textContent = 'Error';
    $('btnSave').disabled = false;
  }
}

async function setPassphrase() {
  const pp = $('newPassphrase').value;
  const msgEl = $('setupMsg');
  if (!pp) { showMsg(msgEl, 'Passphrase cannot be empty', false); return; }
  if (pp.length < 8) { showMsg(msgEl, 'Must be at least 8 characters', false); return; }
  if (pp.length % 4 === 0) {
    try {
      if (atob(pp).length < 16) { showMsg(msgEl, 'Base64 must decode to \u226516 bytes', false); return; }
    } catch(e) {
      showMsg(msgEl, 'Invalid base64', false); return;
    }
  }
  await postJson('set_passphrase', {passphrase: pp});
  showMsg(msgEl, 'Passphrase set \u2014 mesh will reconnect', true);
  $('newPassphrase').value = '';
}

async function generatePassphrase() {
  const res = await postJson('generate_passphrase', {});
  const msgEl = $('setupMsg');
  if (res.passphrase) showMsg(msgEl, 'Generated: ' + res.passphrase, true);
  else showMsg(msgEl, 'Failed to generate', false);
}

async function factoryReset() {
  if (!confirm('Factory reset removes all devices, groups, and passphrase. Continue?')) return;
  await postJson('factory_reset', {});
  showMsg($('setupMsg'), 'Reset complete \u2014 reloading\u2026', true);
  setTimeout(() => location.reload(), 2000);
}

connectSSE();
