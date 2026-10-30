const API = '/api/';
let devices = [], groups = [], bleState = 0;
let meshMqttExposed = false;
let unassocHashes = [];
let claimingHash = null;
let evtSource = null;
const controllerEvents = new Map();
const controllerBindings = new Map();
const isController = d => d.is_controller || [91, 81, 127, 126].includes(d.product_type);

const BLE_NAMES = ['Idle', 'Scanning', 'Connecting', 'Discovering', 'Ready', 'Disconnected'];
const PRODUCTS = [
  {v:0,n:'Group'}, {v:90,n:'Lamp Dimmer'}, {v:93,n:'Recessed DL'},
  {v:94,n:'Light Adapter'}, {v:97,n:'Smart Dimmer'}, {v:134,n:'Smart Bulb'},
  {v:137,n:'Surface DL'}, {v:162,n:'Micro Edge'}, {v:167,n:'Smart Switch'},
  {v:81,n:'Anywhere Dimmer'}, {v:91,n:'Accessory Dimmer'},
  {v:126,n:'Battery Scene Keypad'}, {v:127,n:'Wired Scene Keypad'}
];

// CSR product id (from ping replies) → Avi-on product type, learned from
// observed meshes. Extend as new hardware is seen; unknown ids fall back
// to manual selection.
const CSR_TO_PRODUCT = {18: 162, 19: 167};

// (vendor_code, product_code) from UUID announces → Avi-on product type.
// Vendor 20 is Leedarson; codes verified against the cloud product catalog.
// Only ids present in PRODUCTS are mapped; anything else needs manual selection.
const UUID_CATALOG = {
  20: {4: 93, 5: 90, 6: 97, 8: 94, 10: 134, 13: 137, 18: 162, 19: 167},
};

function productName(v) {
  const p = PRODUCTS.find(p => p.v === v);
  return p ? p.n : 'Unknown';
}

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

function showMsg(el, text, ok, sticky) {
  el.className = 'msg ' + (ok ? 'msg-ok' : 'msg-err');
  el.textContent = text;
  if (!sticky) setTimeout(() => el.textContent = '', 5000);
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
  const lights = devices.filter(d => !isController(d));
  const controllers = devices.filter(isController);
  grid.innerHTML = lights.length ? lights.map(deviceCard).join('') : '<p class="empty">No lights added yet</p>';
  $('controllerGrid').innerHTML = controllers.length ? controllers.map(controllerCard).join('') :
    '<p class="empty">No controllers added yet</p>';
}

function controllerCard(d) {
  const last = controllerEvents.get(d.avion_id);
  const inputLabels = {disabled:'HA events disabled', waiting_for_baseline:'Reading starting level…',
    ready_local_reports:'Ready · local controller reports', native_commands:'Native command decoder selected'};
  const model = {91: 'Accessory Dimmer', 81: 'Anywhere Dimmer', 127: 'Wired Scene Keypad', 126: 'Battery Scene Keypad'}[d.product_type];
  return `<div class="card" data-controller="${d.avion_id}">
    <div class="card-name">${esc(d.name)} <a href="#" class="rename-link" onclick="renameDev(${d.avion_id});return false" title="Rename">✎</a></div>
    <div class="card-sub">${esc(model)} · #${d.avion_id}</div>
    ${[81,91].includes(d.product_type) ? `<p style="font-size:.82em;color:var(--accent2)">${esc(inputLabels[d.ha_input_status] || 'Checking HA input…')}</p>` : ''}
    <label class="toggle-row" style="margin-top:12px">
      <span class="toggle">
        <input type="checkbox" ${d.mqtt_exposed ? 'checked' : ''} onchange="toggleMqtt(${d.avion_id},this.checked,this)">
        <span class="knob"></span>
      </span>
      HA command events
    </label>
    <p style="font-size:.82em;color:var(--dim)">${[81,91].includes(d.product_type) ? 'HA exposes Brightness, On, Off, Raise and Lower from observed levels and commands. Native assignment state is unknown; assignment programming is not supported for this model.' : 'HA exposes named On, Off, Raise and Lower command events. Button events require verified assignment reads.'}</p>
    ${d.product_type === 91 ? `<label class="toggle-row"><span class="toggle"><input type="checkbox" ${d.ha_local_level ? 'checked' : ''} onchange="setLocalDimmerMode(${d.avion_id},this.checked,this)"><span class="knob"></span></span>Local level reports</label>
      <p style="font-size:.82em;color:var(--dim)">${d.ha_local_level ? 'Use Brightness to follow local reports from a separately confirmed unassigned dimmer. The first report after reconnect and query replies are silent. Repeated On/Off reports are preserved; Raise/Lower follow level changes. These are not press/release gestures.' : 'Raise/Lower follow changes in native brightness commands. Holding a button can produce repeated events.'} This selects an event decoder only; it does not read, clear or change HALO assignments or prove HA-only operation.</p>` : ''}
    ${d.product_type === 81 ? '<p style="font-size:.82em;color:var(--dim)">Use the HA Brightness event to follow the touch slider’s level. Raise/Lower follow sliding direction; On/Off are the full-brightness and zero-level commands. Unassigned setup needs a spare-device capture; enabling HA events preserves existing native targets, including managed empty targets.</p>' : ''}
    ${controllerBindingsCard(d)}
    <p style="font-size:.82em">${last ? `Last ${last.input_source === 'local_level_report' ? 'input report' : 'command'}: ${esc(last.event_type)}${last.input_source === 'local_level_report' ? ' · local level' : last.target_id != null ? ` · ${esc(last.target_kind)} #${last.target_id}` : ''}` : 'No command observed in this browser session.'}</p>
    <div class="card-actions">
      <button class="sm ghost" id="exBtn${d.avion_id}" onclick="examDev(${d.avion_id})">Examine</button>
      <button class="sm danger" onclick="removeDev(${d.avion_id})">Remove</button>
    </div>
    <div id="exRes${d.avion_id}" style="font-size:.76em;color:var(--accent2)"></div>
  </div>`;
}

function controllerBindingsCard(d) {
  if (![127,126].includes(d.product_type)) return '';
  const binding = controllerBindings.get(d.avion_id);
  const labels = {ready:'Ready', unassigned:'Unassigned', needs_refresh:'Read assignments to enable',
    model_unverified:'Model not validated yet', unsupported_binding:'Unsupported assignment',
    ambiguous_target:'Shared target — button ambiguous'};
  const rows = [1,2,3,4].map(slot => {
    const item = binding?.slots?.find(s => s.slot === slot);
    const status = item?.status || 'needs_refresh';
    let target = '';
    if (status === 'ready') {
      const group = groups.find(g => g.group_id === item.target_id);
      const device = devices.find(v => v.avion_id === item.target_id);
      target = ` · ${group?.name?.startsWith('HA event target: ') ? 'HA automation target' : esc(group?.name || device?.name || '#' + item.target_id)}`;
    }
    return `<li><strong>Button ${slot}</strong>: ${esc(labels[status] || status)}${target}</li>`;
  }).join('');
  const reading = binding?.busy;
  const setupBusy = binding?.setup_busy;
  const setupStatus = binding?.setup_status || (d.ha_setup_pending ? 'pending' : 'not_requested');
  const outcome = binding?.status;
  let message = '';
  if (reading && outcome === 'waiting_for_wake') message = 'Close the HALO app, then press the battery keypad’s bottom Power/Off button once to begin reading.';
  else if (reading) message = `Reading assignments: round ${binding.round}/2, ${binding.query_slot ? 'button ' + binding.query_slot : 'count'}…`;
  else if (outcome === 'saved') message = 'Mapping verified twice and saved. Native assignments were not changed.';
  else if (outcome && !['needs_refresh','model_unverified'].includes(outcome)) message = `Read status: ${outcome.replaceAll('_',' ')}.`;
  const setupLabels = {pending:'Automatic HA setup is pending.',
    waiting_for_wake:'Close the HALO app, then press the battery keypad’s bottom Power/Off button once to begin setup.',
    reading_assignments:'Reading existing assignments…',
    planning_targets:'Reserving empty HA targets…', checking_unused_button:'Checking the unused button…',
    programming_unused_button:'Configuring the unused button…', verifying_all_assignments:'Verifying all assignments…',
    ready:'HA button setup complete. Existing assignments were preserved.',
    existing_assignments_need_review:'Unused buttons configured. Some existing assignments need review before HA can distinguish them.',
    model_unverified:'Automatic setup for this model still needs validation.'};
  const setupMessage = setupStatus === 'not_requested' ? '' : (setupLabels[setupStatus] || `Setup stopped: ${setupStatus.replaceAll('_',' ')}.`);
  return `<div class="button-mapping"><ul>${rows}</ul>
    <div class="row"><button class="sm ghost" ${reading || setupBusy ? 'disabled' : ''} onclick="readControllerButtons(${d.avion_id})">Read button assignments</button>
      <button class="sm" ${reading || setupBusy || d.product_type !== 127 ? 'disabled' : ''} onclick="setupControllerButtons(${d.avion_id})">Set up unused HA buttons</button></div>
    <p>${esc(setupMessage)}</p>
    <p>${esc(message || 'New wired keypads are set up automatically. Setup fills unused buttons and preserves existing assignments. Battery keypad programming is awaiting validation; assignment reads require a Power/Off wake press.')}</p></div>`;
}

async function readControllerButtons(id) {
  try { await postJson('controller_buttons/read', {controller_id:id}); }
  catch (error) { feedLog(`Read buttons: ${error.message}`); }
}

async function setLocalDimmerMode(id, enabled, input) {
  input.disabled = true;
  try { await postJson('controller_input_mode', {avion_id:id, enabled}); }
  catch (error) { input.checked = !enabled; feedLog(`Dimmer input: ${error.message}`); }
  finally { input.disabled = false; }
}

async function setupControllerButtons(id) {
  try { await postJson('controller_buttons/setup', {controller_id:id}); }
  catch (error) { feedLog(`Set up buttons: ${error.message}`); }
}

function deviceCard(d) {
  const knownBri = d.brightness !== undefined;
  const on = knownBri && d.brightness > 0;
  const stateText = knownBri ? (on ? `ON · ${d.brightness}` : 'OFF') : '';
  const stateClass = knownBri ? (on ? 'on' : 'off') : 'unknown';
  const hasDim = d.has_dimming !== false;
  const hasCT  = d.has_color_temp === true;
  const ctVal  = d.color_temp ?? 2700;

  const grpTags = (d.groups || []).map(gid => {
    const g = groups.find(x => x.group_id === gid);
    return `<span class="tag">${g ? esc(g.name) : gid}</span>`;
  }).join('');

  return `<div class="card" data-avid="${d.avion_id}">
  <div class="card-header">
    <div>
      <div class="card-name">${esc(d.name)} <a href="#" class="rename-link" onclick="renameDev(${d.avion_id});return false" title="Rename">✎</a></div>
      <div class="card-sub">${esc(d.product_name)} · #${d.avion_id}</div>
    </div>
    <span id="stPill${d.avion_id}" class="state-pill ${stateClass}">${stateText}</span>
  </div>
  <div class="on-off">
    <button class="sm ghost" onclick="ctrlDev(${d.avion_id},255)">On</button>
    <button class="sm ghost" onclick="ctrlDev(${d.avion_id},0)">Off</button>
    <label class="toggle-row">
      <span class="toggle">
        <input type="checkbox" ${d.mqtt_exposed ? 'checked' : ''} onchange="toggleMqtt(${d.avion_id},this.checked,this)">
        <span class="knob"></span>
      </span>
      MQTT
    </label>
  </div>
  ${grpTags ? `<div class="tags">${grpTags}</div>` : ''}
  <details class="ctrl">
    <summary>Controls</summary>
    <div class="ctrl-body">
      ${hasDim ? `<div class="slider-row">
        <div class="slider-label">
          <span>Brightness</span>
          <span id="bv${d.avion_id}">${knownBri ? d.brightness : '—'}</span>
        </div>
        <input type="range" min="0" max="255" value="${d.brightness ?? 0}" class="bri-range"
          oninput="$('bv${d.avion_id}').textContent=this.value"
          onchange="ctrlDev(${d.avion_id},+this.value)">
      </div>` : ''}
      ${hasCT ? `<div class="slider-row">
        <div class="slider-label">
          <span>Color temp</span>
          <span id="cv${d.avion_id}">${d.color_temp ? ctVal + 'K' : '—'}</span>
        </div>
        <input type="range" min="2000" max="6500" step="100" value="${ctVal}" class="ct-range"
          oninput="$('cv${d.avion_id}').textContent=this.value+'K'"
          onchange="ctrlTemp(${d.avion_id},+this.value)">
      </div>` : ''}
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
  const device = devices.find(x => x.avion_id === d.avion_id);
  if (device && isController(device)) return;
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
  if (g.ha_event_target) {
    return `<div class="card"><div class="card-name">${esc(g.name)}</div>
      <div class="card-sub">HA-only event target #${g.group_id}</div>
      <p>Reserved for controller events. This target has no light members; the bridge keeps this target empty.</p></div>`;
  }
  if (g.name.startsWith('HA event target: ')) {
    const owner = g.name.slice('HA event target: '.length).split(':');
    return `<div class="card">
      <div class="card-name">HA-only button ${esc(owner[1])}</div>
      <div class="card-sub">Keypad #${esc(owner[0])} · event target #${g.group_id}</div>
      <p style="font-size:.82em;color:var(--dim)">Reserved for controller events. This target has no light members.</p>
    </div>`;
  }
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
    <div class="card-name">${esc(g.name)} <a href="#" class="rename-link" onclick="renameGrp(${g.group_id});return false" title="Rename">✎</a></div>
    <div class="card-sub">Group · #${g.group_id}</div>
  </div>
  <div class="on-off">
    <button class="sm" onclick="ctrlDev(${g.group_id},255)">On</button>
    <button class="sm ghost" onclick="ctrlDev(${g.group_id},0)">Off</button>
  </div>
  <label class="toggle-row">
    <span class="toggle">
      <input type="checkbox" ${g.mqtt_exposed ? 'checked' : ''} onchange="toggleMqtt(${g.group_id},this.checked,this)">
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

  evtSource.addEventListener('controller_event', e => {
    const command = JSON.parse(e.data);
    controllerEvents.set(command.source_id, command);
    renderDevices();
    feedLog(`Controller #${command.source_id}: ${command.event_type}`);
  });
  evtSource.addEventListener('controller_input_mode', e => {
    const result = JSON.parse(e.data);
    const device = devices.find(d => d.avion_id === result.avion_id);
    if (device && result.saved) {
      device.ha_local_level = result.enabled;
      device.ha_input_status = result.ha_input_status;
    }
    renderDevices();
    feedLog(result.saved ? `Dimmer #${result.avion_id}: input mode saved` : `Dimmer input: ${result.message}`);
  });
  evtSource.addEventListener('controller_input_status', e => {
    const result = JSON.parse(e.data);
    const device = devices.find(d => d.avion_id === result.controller_id);
    if (device) device.ha_input_status = result.status;
    renderDevices();
  });
  evtSource.addEventListener('controller_buttons', e => {
    const binding = JSON.parse(e.data);
    controllerBindings.set(binding.controller_id, binding);
    renderDevices();
  });

  evtSource.addEventListener('device_added', e => {
    const d = JSON.parse(e.data);
    const i = devices.findIndex(x => x.avion_id === d.avion_id);
    if (i >= 0) devices[i] = d; else devices.push(d);
    updateStatusBar();
    renderDevices();
    feedLog('Device added: ' + d.name);
  });

  evtSource.addEventListener('device_updated', e => {
    const d = JSON.parse(e.data);
    const i = devices.findIndex(x => x.avion_id === d.avion_id);
    if (i >= 0) devices[i].name = d.name;
    renderDevices();
    feedLog('Device renamed: ' + d.name);
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
    if (i >= 0) {
      // Rename payloads carry only group_id + name — merge, don't replace.
      if (d.name !== undefined) groups[i].name = d.name;
      if (d.members !== undefined) groups[i].members = d.members;
      if (d.mqtt_exposed !== undefined) groups[i].mqtt_exposed = d.mqtt_exposed;
    } else if (d.members !== undefined) {
      groups.push(d);
    }
    renderGroups();
    if (d.name !== undefined) feedLog('Group renamed: ' + d.name);
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
    const devs = d.devices || (d.uuid_hashes || []).map(h => ({uuid_hash: h}));
    renderUnassoc(devs);
    feedLog('Unassoc scan: ' + devs.length + ' found');
  });

  evtSource.addEventListener('claim_result', e => {
    const d = JSON.parse(e.data);
    feedLog('Claim: ' + (d.status === 'ok' ? 'ID ' + d.device_id : d.status === 'started' ? 'started for ID ' + d.device_id : d.message || 'failed'));
    if (d.status === 'ok' && claimingHash) {
      const idx = unassocHashes.indexOf(claimingHash);
      if (idx >= 0) {
        $('unassocRow' + idx)?.remove();
        unassocHashes.splice(idx, 1);
      }
      claimingHash = null;
    } else if (d.status === 'error' && claimingHash) {
      const idx = unassocHashes.indexOf(claimingHash);
      const msg = $('claimMsg' + idx);
      if (msg) msg.textContent = 'Failed: ' + (d.message || 'association failed') + '. Re-scan before retrying.';
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
        : d.short
          ? `Short ping reply (no FW info) · raw ${esc(d.raw)}`
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

  evtSource.addEventListener('save_result', e => {
    const result = JSON.parse(e.data);
    $('btnSave').disabled = false;
    $('saveMsg').textContent = result.status === 'ok' ? 'Saved' : 'Save failed';
    $('saveMsg').style.color = result.status === 'ok' ? 'var(--ok)' : 'var(--err)';
    setTimeout(() => $('saveMsg').textContent = '', 3000);
    feedLog(result.status === 'ok' ? 'DB saved' : 'DB save failed');
  });

  evtSource.addEventListener('mqtt_toggled', e => {
    const d = JSON.parse(e.data);
    if (d.id === 0) {
      meshMqttExposed = d.mqtt_exposed;
      renderGroups();
    } else {
      const dev = devices.find(x => x.avion_id === d.id);
      if (dev) {
        dev.mqtt_exposed = d.mqtt_exposed;
        if (d.ha_input_status !== undefined) dev.ha_input_status = d.ha_input_status;
        if (d.ha_setup_pending !== undefined) dev.ha_setup_pending = d.ha_setup_pending;
        renderDevices();
      }
      const grp = groups.find(x => x.group_id === d.id);
      if (grp) { grp.mqtt_exposed = d.mqtt_exposed; renderGroups(); }
    }
    const saved = d.saved === true;
    $('saveMsg').textContent = saved ? 'MQTT setting saved' : 'MQTT setting was not saved';
    $('saveMsg').style.color = saved ? 'var(--ok)' : 'var(--err)';
    if (saved) setTimeout(() => $('saveMsg').textContent = '', 3500);
    feedLog(saved ? 'MQTT ' + (d.mqtt_exposed ? 'on' : 'off') + ' saved: #' + d.id : 'MQTT save failed: #' + d.id);
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

async function renameDev(id) {
  const dev = devices.find(x => x.avion_id === id);
  const name = prompt('New name for device #' + id + ':', dev ? dev.name : '');
  if (name === null) return;
  if (!name.trim()) { alert('Name cannot be empty'); return; }
  await postJson('rename_device', {avion_id: id, name: name.trim()});
}

async function renameGrp(gid) {
  const grp = groups.find(x => x.group_id === gid);
  const name = prompt('New name for group #' + gid + ':', grp ? grp.name : '');
  if (name === null) return;
  if (!name.trim()) { alert('Name cannot be empty'); return; }
  await postJson('rename_group', {group_id: gid, name: name.trim()});
}

async function removeDev(id) {
  if (!confirm('Remove device ' + id + '?')) return;
  await postJson('unclaim_device', {avion_id: id});
}

async function toggleMqtt(id, exposed, cb) {
  if (cb) cb.disabled = true;
  try {
    await postJson('set_mqtt_exposed', {id, exposed});
  } catch(e) {
    if (cb) cb.checked = !exposed;
    feedLog('MQTT toggle failed: ' + e.message);
  } finally {
    if (cb) cb.disabled = false;
  }
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
  tb.innerHTML = devs.map(d => {
    const suggested = d.short ? 0 : (CSR_TO_PRODUCT[d.csr_product_id] || 0);
    const type_note = d.short
      ? '<div style="font-size:.75em;color:var(--warn)">Short reply: type not detectable, pick manually</div>'
      : (suggested ? '' : '<div style="font-size:.75em;color:var(--warn)">Unknown CSR id, pick manually</div>');
    return `<tr>
    <td>${d.device_id}</td>
    <td>${d.fw}</td>
    <td>${d.vendor_id}</td>
    <td>${d.csr_product_id}</td>
    <td>${d.known ? '<span style="color:var(--ok)">Known</span>' : 'New'}</td>
    <td>${d.known ? '' : `<input placeholder="Name" id="sn${d.device_id}" style="width:90px" value="Device ${d.device_id}">
      ${productSelect(suggested)}
      ${type_note}
      <button class="sm" onclick="addScanned(${d.device_id})">Add</button>`}</td>
  </tr>`; }).join('');
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

function renderUnassoc(devs) {
  $('unassocResults').classList.remove('hidden');
  const el = $('unassocList');
  unassocHashes = devs.map(d => d.uuid_hash);
  if (!devs.length) {
    el.innerHTML = '<p class="empty">No unassociated devices found</p>';
    return;
  }
  el.innerHTML = devs.map((d, i) => {
    const suggested = (UUID_CATALOG[d.vendor_code] || {})[d.product_code] || 0;
    const ident = (d.vendor_code !== undefined)
      ? `<div style="font-size:.75em">Vendor ${d.vendor_code} · Product ${d.product_code}` +
        (suggested ? ` · <span style="color:var(--ok)">${esc(productName(suggested))}</span>` : ' · pick type manually') +
        `</div>`
      : '';
    return `<div class="row" id="unassocRow${i}">
    <span class="mono">${d.uuid_hash}</span>
    <input placeholder="Name" id="cn${i}" style="width:100px">
    ${productSelect(suggested || 134)}
    ${ident}
    <button class="sm" id="claimBtn${i}" onclick="claimDev('${d.uuid_hash}',${i})">Claim</button>
    <span id="claimMsg${i}" style="font-size:.8em"></span>
  </div>`; }).join('');
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
  try {
    await postJson('claim_device', {uuid_hash: parseInt(hash, 16), name, product_type: pt});
  } catch (e) {
    $('claimMsg' + idx).textContent = 'Failed: ' + e.message;
    btn.disabled = false;
    btn.textContent = 'Claim';
    claimingHash = null;
  }
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
  if (!res.passphrase) { showMsg(msgEl, 'Failed to generate', false); return; }
  const field = $('generatedPassphrase');
  field.value = res.passphrase;
  $('generatedRow').style.display = '';
  field.select();
  showMsg(msgEl, 'New passphrase generated and saved. Copy it now — generating again re-keys the mesh and orphans already-claimed devices.', true, true);
}

async function copyGenerated() {
  const field = $('generatedPassphrase');
  field.select();
  try {
    await navigator.clipboard.writeText(field.value);
    showMsg($('setupMsg'), 'Copied to clipboard', true);
  } catch (e) {
    document.execCommand('copy');
    showMsg($('setupMsg'), 'Copied (select the field and press Ctrl+C if it is still selected)', true);
  }
}

async function exportBackup() {
  const res = await fetch('api/export');
  if (!res.ok) { showMsg($('setupMsg'), 'Export failed', false); return; }
  const text = await res.text();
  const blob = new Blob([text], {type: 'application/json'});
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'avionmesh-backup.json';
  document.body.appendChild(a);
  a.click();
  URL.revokeObjectURL(a.href);
  a.remove();
  showMsg($('setupMsg'), 'Backup downloaded — it contains your mesh passphrase, store it somewhere safe', true, true);
}

async function factoryReset() {
  if (!confirm('Factory reset removes all devices, groups, and passphrase. Continue?')) return;
  await postJson('factory_reset', {});
  showMsg($('setupMsg'), 'Reset complete \u2014 reloading\u2026', true);
  setTimeout(() => location.reload(), 2000);
}

connectSSE();
