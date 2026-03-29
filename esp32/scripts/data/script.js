const qs = (id) => document.getElementById(id);
const tokenKey = 'apiToken';

const ui = {
  gateState: qs('gateState'),
  gateProgress: qs('gateProgress'),
  gatePercent: qs('gatePercent'),
  wifiChip: qs('wifiChip'),
  mqttChip: qs('mqttChip'),
  limitsChip: qs('limitsChip'),
  limitOpenChip: qs('limitOpenChip'),
  limitCloseChip: qs('limitCloseChip'),
  photocellChip: qs('photocellChip'),
  ld2410Chip: qs('ld2410Chip'),
  hallChip: qs('hallChip'),
  uptimeChip: qs('uptimeChip'),
  ipValue: qs('ipValue'),
  rssiValue: qs('rssiValue'),
  wifiModeValue: qs('wifiModeValue'),
  lastRemoteValue: qs('lastRemoteValue'),
  hbDistValue: qs('hbDistValue'),
  hbBatValue: qs('hbBatValue'),
  hbIAValue: qs('hbIAValue'),
  hbArmedValue: qs('hbArmedValue'),
  hbRpmValue: qs('hbRpmValue'),
  hbFaultValue: qs('hbFaultValue'),
  maxDistanceValue: qs('maxDistanceValue'),
  targetPositionValue: qs('targetPositionValue'),
  eventList: qs('eventList'),
  eventFilter: qs('eventFilter'),
  toast: qs('toast'),
  safetyAlert: qs('safetyAlert'),
  openBtn: qs('openBtn'),
  closeBtn: qs('closeBtn'),
  stopBtn: qs('stopBtn'),
  toggleBtn: qs('toggleBtn'),
  toggleMode: qs('toggleMode'),
};

const state = {
  events: [],
  filter: 'all',
  toggleMode: false,
};

function getToken() {
  return localStorage.getItem(tokenKey) || '';
}

async function apiFetch(path, options = {}) {
  const headers = options.headers || {};
  const token = getToken();
  if (token) headers['X-Api-Key'] = token;
  if (options.body && !headers['Content-Type']) {
    headers['Content-Type'] = 'application/json';
  }
  const res = await fetch(path, { ...options, headers });
  if (!res.ok) throw new Error(`${res.status}`);
  return res;
}

function showToast(message, type = 'info') {
  ui.toast.textContent = message;
  ui.toast.className = `toast show ${type}`;
  setTimeout(() => ui.toast.className = 'toast', 2400);
}

function formatUptime(ms) {
  if (!ms && ms !== 0) return '--';
  const sec = Math.floor(ms / 1000);
  const h = Math.floor(sec / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = sec % 60;
  return `${h}h ${m}m ${s}s`;
}

function formatDistance(distMm) {
  if (distMm === undefined || distMm === null) return '--';
  if (distMm < 0) return '---';
  const meters = distMm / 1000;
  return `${distMm} mm (${meters.toFixed(2)} m)`;
}

function formatBattery(batV) {
  if (batV === undefined || batV === null) return 'brak danych';
  if (typeof batV !== 'number' || !Number.isFinite(batV) || batV <= 0) return '---';
  return `${batV.toFixed(1)} V`;
}

function setChip(el, text, state) {
  if (!el) return;
  el.textContent = text;
  el.classList.remove('success', 'warn', 'danger');
  if (state) el.classList.add(state);
}

function setGatePercent(percent) {
  const p = Math.max(0, Math.min(100, percent));
  ui.gateProgress.style.width = `${p}%`;
  ui.gatePercent.textContent = `${p}`;
}

function addEvent(ev) {
  if (!ev || !ev.message) return;
  state.events.unshift(ev);
  if (state.events.length > 10) state.events.pop();
  renderEvents();
}

function renderEvents() {
  ui.eventList.innerHTML = '';
  const list = state.events.filter(e => state.filter === 'all' || e.level === state.filter);
  if (list.length === 0) {
    const li = document.createElement('li');
    li.className = 'event info';
    li.textContent = 'Brak zdarzen';
    ui.eventList.appendChild(li);
    return;
  }
  list.forEach(ev => {
    const li = document.createElement('li');
    li.className = `event ${ev.level || 'info'}`;
    const left = document.createElement('div');
    left.textContent = ev.message || '-';
    const right = document.createElement('div');
    right.className = 'meta';
    right.textContent = ev.ts ? `${Math.floor(ev.ts / 1000)}s` : '--';
    li.appendChild(left);
    li.appendChild(right);
    ui.eventList.appendChild(li);
  });
}

function updateStatus(data) {
  if (!data) return;
  const gate = data.gate || {};
  const wifi = data.wifi || {};
  const mqtt = data.mqtt || {};
  const hb = data.hb || {};
  const limits = data.limits || {};
  const inputs = data.inputs || {};
  const ld = data.ld2410 || {};
  const hbEnabled = hb.enabled !== false;
  const last = (data.remotes && data.remotes.last) || {};

  ui.gateState.textContent = (gate.state || 'unknown').toUpperCase();
  setGatePercent(gate.positionPercent >= 0 ? gate.positionPercent : 0);

  setChip(ui.wifiChip, `WiFi: ${wifi.connected ? (wifi.ssid || 'OK') : 'OFF'}`, wifi.connected ? 'success' : 'warn');
  setChip(ui.mqttChip, `MQTT: ${mqtt.connected ? 'OK' : 'OFF'}`, mqtt.connected ? 'success' : 'warn');
  setChip(ui.limitsChip, `Limity: ${limits.enabled ? 'ON' : 'OFF'}`, limits.enabled ? 'success' : 'warn');
  const limitOpen = Boolean(inputs.limitOpen);
  const limitClose = Boolean(inputs.limitClose);
  setChip(ui.limitOpenChip, `OPEN: ${limitOpen ? 'ON' : 'OFF'}`, limitOpen ? 'success' : '');
  setChip(ui.limitCloseChip, `CLOSE: ${limitClose ? 'ON' : 'OFF'}`, limitClose ? 'success' : '');
  const photocellBlocked = Boolean(inputs.photocellBlocked);
  setChip(ui.photocellChip, `Fotokomorka: ${photocellBlocked ? 'BLOCKED' : 'CLEAR'}`, photocellBlocked ? 'danger' : 'success');
  const hallEnabled = Boolean(inputs.hallEnabled);
  const hallPps = typeof inputs.hallPps === 'number' ? inputs.hallPps : 0;
  setChip(ui.hallChip, hallEnabled ? `Hall: ${hallPps.toFixed(1)} pps` : 'Hall: OFF', hallEnabled ? 'success' : 'warn');
  const ldAvailable = Boolean(ld.available);
  const ldDistance = typeof ld.distanceCm === 'number' ? ld.distanceCm : -1;
  const ldState = ldAvailable ? (ld.present ? 'PRESENT' : 'CLEAR') : 'NO DATA';
  const ldExtra = ldAvailable && ldDistance >= 0 ? ` ${ldDistance}cm` : '';
  setChip(ui.ld2410Chip, `LD2410: ${ldState}${ldExtra}`, ldAvailable ? 'success' : 'warn');
  setChip(ui.uptimeChip, `Uptime: ${formatUptime(data.uptimeMs)}`);

  ui.ipValue.textContent = wifi.ip || '-';
  ui.rssiValue.textContent = wifi.connected ? `${wifi.rssi} dBm` : '-';
  ui.wifiModeValue.textContent = wifi.mode || '-';
  ui.lastRemoteValue.textContent = last.serial ? `${last.serial}` : '-';
  if (hbEnabled) {
    const telOk = hb.lastTelMs && hb.lastTelMs > 0;
    ui.hbDistValue.textContent = telOk ? formatDistance(hb.dist_mm) : 'brak telemetrii';
    ui.hbBatValue.textContent = telOk ? formatBattery(hb.batV) : 'brak telemetrii';
    if (ui.hbIAValue) ui.hbIAValue.textContent = telOk && hb.iA !== undefined && hb.iA !== null && hb.iA >= 0 ? `${hb.iA.toFixed(2)} A` : (telOk ? '---' : 'brak telemetrii');
    if (ui.hbArmedValue) ui.hbArmedValue.textContent = telOk ? (hb.armed ? 'ON' : 'OFF') : 'brak telemetrii';
    ui.hbRpmValue.textContent = telOk && (hb.rpm !== undefined && hb.rpm !== null) ? `${hb.rpm}` : '-';
    if (hb.fault === 0) {
      ui.hbFaultValue.textContent = telOk ? 'OK' : '-';
    } else if (hb.fault !== undefined && hb.fault !== null) {
      ui.hbFaultValue.textContent = `${hb.fault}`;
    } else {
      ui.hbFaultValue.textContent = '-';
    }
  } else {
    ui.hbDistValue.textContent = '-';
    ui.hbBatValue.textContent = '-';
    ui.hbRpmValue.textContent = '-';
    ui.hbFaultValue.textContent = '-';
  }
  if (gate.maxDistance && gate.maxDistance > 0) {
    ui.maxDistanceValue.textContent = `${gate.maxDistance.toFixed(2)} m`;
  } else {
    ui.maxDistanceValue.textContent = '-';
  }
  const targetPos = gate.targetPosition;
  if (typeof targetPos === 'number' && targetPos >= 0) {
    ui.targetPositionValue.textContent = `${targetPos.toFixed(2)} m`;
  } else {
    ui.targetPositionValue.textContent = '-';
  }

  ui.safetyAlert.style.display = gate.state === 'error' ? 'block' : 'none';
  const disableActions = gate.state === 'error';
  ui.openBtn.disabled = disableActions || state.toggleMode;
  ui.closeBtn.disabled = disableActions || state.toggleMode;
  ui.toggleBtn.disabled = disableActions;
}

async function fetchStatus() {
  try {
    const res = await fetch('/api/status');
    if (!res.ok) return;
    const data = await res.json();
    updateStatus(data);
    if (data.events) {
      state.events = data.events.slice().reverse();
      renderEvents();
    }
  } catch {
    // ignore
  }
}

async function sendControl(action) {
  try {
    await apiFetch('/api/control', { method: 'POST', body: JSON.stringify({ action }) });
  } catch {
    showToast('Brak dostepu do sterowania', 'error');
  }
}

function setupControls() {
  ui.openBtn.addEventListener('click', () => sendControl('open'));
  ui.closeBtn.addEventListener('click', () => sendControl('close'));
  ui.stopBtn.addEventListener('click', () => sendControl('stop'));
  ui.toggleBtn.addEventListener('click', () => sendControl('toggle'));
  ui.toggleMode.addEventListener('change', () => {
    state.toggleMode = ui.toggleMode.checked;
    ui.openBtn.disabled = state.toggleMode;
    ui.closeBtn.disabled = state.toggleMode;
  });
}

function setupEvents() {
  ui.eventFilter.addEventListener('change', () => {
    state.filter = ui.eventFilter.value;
    renderEvents();
  });
}

function connectWs() {
  const url = `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`;
  const ws = new WebSocket(url);
  ws.onmessage = (evt) => {
    try {
      const msg = JSON.parse(evt.data);
      if (msg.type === 'status') {
        updateStatus(msg.data);
      } else if (msg.type === 'event') {
        addEvent({ level: msg.level, message: msg.message, ts: Date.now() });
      } else if (msg.type === 'learn') {
        showToast(`Dodano pilota ${msg.serial}`, 'info');
      } else if (msg.type === 'test_remote') {
        showToast('Test pilota odebrany', 'info');
      }
    } catch {
      // ignore
    }
  };
  ws.onclose = () => setTimeout(connectWs, 2000);
}

window.addEventListener('load', () => {
  setupControls();
  setupEvents();
  fetchStatus();
  setInterval(fetchStatus, 5000);
  connectWs();
});
