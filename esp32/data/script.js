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
  hbTelAgeValue: qs('hbTelAgeValue'),
  hbCmdAgeValue: qs('hbCmdAgeValue'),
  maxDistanceValue: qs('maxDistanceValue'),
  targetPositionValue: qs('targetPositionValue'),
  gateStopReasonValue: qs('gateStopReasonValue'),
  eventList: qs('eventList'),
  eventFilter: qs('eventFilter'),
  toast: qs('toast'),
  safetyAlert: qs('safetyAlert'),
  openBtn: qs('openBtn'),
  closeBtn: qs('closeBtn'),
  stopBtn: qs('stopBtn'),
  toggleBtn: qs('toggleBtn'),
  zeroBtn: qs('zeroBtn'),
  toggleMode: qs('toggleMode'),
};

const state = {
  events: [],
  filter: 'all',
  toggleMode: false,
  intervalsStarted: false,
  statusLiteInFlight: false,
  statusFullInFlight: false,
  statusAbort: null,
  fullAbort: null,
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

function stopReasonLabel(code) {
  const map = {
    0: 'none',
    1: 'user',
    2: 'soft_limit',
    3: 'tel_timeout',
    4: 'tel_stall',
    5: 'hover_fault',
    6: 'limit_open',
    7: 'limit_close',
    8: 'obstacle',
    9: 'error',
  };
  return map[code] || 'unknown';
}

function setChip(el, text, state) {
  if (!el) return;
  if (el.textContent !== text) el.textContent = text;
  el.classList.remove('success', 'warn', 'danger');
  if (state) el.classList.add(state);
}

function setGatePercent(percent) {
  const p = Math.max(0, Math.min(100, percent));
  // The dashboard bar shows the "closed" portion of the travel:
  // 0% when fully open, 100% when fully closed.
  const shown = 100 - p;
  const width = `${shown}%`;
  if (ui.gateProgress.style.width !== width) ui.gateProgress.style.width = width;
  if (ui.gatePercent.textContent !== `${shown}`) ui.gatePercent.textContent = `${shown}`;
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
  const hbEnabled = hb.enabled !== false;
  const last = (data.remotes && data.remotes.last) || {};

  const gateState = (gate.state || 'unknown').toUpperCase();
  if (ui.gateState.textContent !== gateState) ui.gateState.textContent = gateState;
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
  setChip(ui.uptimeChip, `Uptime: ${formatUptime(data.uptimeMs)}`);

  const ip = wifi.ip || '-';
  if (ui.ipValue.textContent !== ip) ui.ipValue.textContent = ip;
  const rssi = wifi.connected ? `${wifi.rssi} dBm` : '-';
  if (ui.rssiValue.textContent !== rssi) ui.rssiValue.textContent = rssi;
  const wifiMode = wifi.mode || '-';
  if (ui.wifiModeValue.textContent !== wifiMode) ui.wifiModeValue.textContent = wifiMode;
  const lastRemote = last.serial ? `${last.serial}` : '-';
  if (ui.lastRemoteValue.textContent !== lastRemote) ui.lastRemoteValue.textContent = lastRemote;
  if (hbEnabled) {
    const telOk = hb.lastTelMs && hb.lastTelMs > 0;
      const hbDist = telOk ? formatDistance(hb.dist_mm) : 'brak telemetrii';
      if (ui.hbDistValue.textContent !== hbDist) ui.hbDistValue.textContent = hbDist;
      const hbBat = telOk ? formatBattery(hb.batV) : 'brak telemetrii';
      if (ui.hbBatValue.textContent !== hbBat) ui.hbBatValue.textContent = hbBat;
      if (ui.hbIAValue) {
        const hbIA = telOk && hb.iA !== undefined && hb.iA !== null && hb.iA >= 0 ? `${hb.iA.toFixed(2)} A` : (telOk ? '---' : 'brak telemetrii');
        if (ui.hbIAValue.textContent !== hbIA) ui.hbIAValue.textContent = hbIA;
      }
      if (ui.hbArmedValue) {
        const hbArmed = telOk ? (hb.armed ? 'ON' : 'OFF') : 'brak telemetrii';
        if (ui.hbArmedValue.textContent !== hbArmed) ui.hbArmedValue.textContent = hbArmed;
      }
      const hbRpm = telOk && (hb.rpm !== undefined && hb.rpm !== null) ? `${hb.rpm}` : '-';
      if (ui.hbRpmValue.textContent !== hbRpm) ui.hbRpmValue.textContent = hbRpm;
    if (hb.fault === 0) {
      const fault = telOk ? 'OK' : '-';
      if (ui.hbFaultValue.textContent !== fault) ui.hbFaultValue.textContent = fault;
    } else if (hb.fault !== undefined && hb.fault !== null) {
      const fault = `${hb.fault}`;
      if (ui.hbFaultValue.textContent !== fault) ui.hbFaultValue.textContent = fault;
    } else {
      if (ui.hbFaultValue.textContent !== '-') ui.hbFaultValue.textContent = '-';
    }
    if (ui.hbTelAgeValue) {
      const telAge = telOk && hb.telAgeMs !== undefined && hb.telAgeMs !== null ? `${hb.telAgeMs} ms` : '-';
      if (ui.hbTelAgeValue.textContent !== telAge) ui.hbTelAgeValue.textContent = telAge;
    }
    if (ui.hbCmdAgeValue) {
      const cmdAge = telOk && hb.cmdAgeMs !== undefined && hb.cmdAgeMs !== null ? `${hb.cmdAgeMs} ms` : '-';
      if (ui.hbCmdAgeValue.textContent !== cmdAge) ui.hbCmdAgeValue.textContent = cmdAge;
    }
  } else {
    if (ui.hbDistValue.textContent !== '-') ui.hbDistValue.textContent = '-';
    if (ui.hbBatValue.textContent !== '-') ui.hbBatValue.textContent = '-';
    if (ui.hbRpmValue.textContent !== '-') ui.hbRpmValue.textContent = '-';
    if (ui.hbFaultValue.textContent !== '-') ui.hbFaultValue.textContent = '-';
    if (ui.hbTelAgeValue && ui.hbTelAgeValue.textContent !== '-') ui.hbTelAgeValue.textContent = '-';
    if (ui.hbCmdAgeValue && ui.hbCmdAgeValue.textContent !== '-') ui.hbCmdAgeValue.textContent = '-';
  }
  if (gate.maxDistance && gate.maxDistance > 0) {
    const maxDistance = `${gate.maxDistance.toFixed(2)} m`;
    if (ui.maxDistanceValue.textContent !== maxDistance) ui.maxDistanceValue.textContent = maxDistance;
  } else {
    if (ui.maxDistanceValue.textContent !== '-') ui.maxDistanceValue.textContent = '-';
  }
  const targetPos = gate.targetPosition;
  if (typeof targetPos === 'number' && targetPos >= 0) {
    const target = `${targetPos.toFixed(2)} m`;
    if (ui.targetPositionValue.textContent !== target) ui.targetPositionValue.textContent = target;
  } else {
    if (ui.targetPositionValue.textContent !== '-') ui.targetPositionValue.textContent = '-';
  }
  if (ui.gateStopReasonValue) {
    const reason = stopReasonLabel(gate.stopReason);
    if (ui.gateStopReasonValue.textContent !== reason) ui.gateStopReasonValue.textContent = reason;
  }

  const safetyDisplay = gate.state === 'error' ? 'block' : 'none';
  if (ui.safetyAlert.style.display !== safetyDisplay) ui.safetyAlert.style.display = safetyDisplay;
  const disableActions = gate.state === 'error';
  ui.openBtn.disabled = disableActions || state.toggleMode;
  ui.closeBtn.disabled = disableActions || state.toggleMode;
  ui.toggleBtn.disabled = disableActions;
}

function updateStatusLite(data) {
  if (!data) return;
  const rawState = (data.state || '').toString();
  if (!rawState) return;
  const gateState = rawState.toUpperCase();
  if (ui.gateState.textContent !== gateState) ui.gateState.textContent = gateState;

  const pct = typeof data.positionPercent === 'number' ? data.positionPercent : 0;
  setGatePercent(pct >= 0 ? pct : 0);

  const limitOpen = Boolean(data.limitOpen);
  const limitClose = Boolean(data.limitClose);
  setChip(ui.limitOpenChip, `OPEN: ${limitOpen ? 'ON' : 'OFF'}`, limitOpen ? 'success' : '');
  setChip(ui.limitCloseChip, `CLOSE: ${limitClose ? 'ON' : 'OFF'}`, limitClose ? 'success' : '');

  if (typeof data.rpm === 'number') {
    const rpm = `${data.rpm}`;
    if (ui.hbRpmValue.textContent !== rpm) ui.hbRpmValue.textContent = rpm;
  }
  if (typeof data.iA === 'number' && ui.hbIAValue) {
    const iA = data.iA >= 0 ? `${data.iA.toFixed(2)} A` : '---';
    if (ui.hbIAValue.textContent !== iA) ui.hbIAValue.textContent = iA;
  }

  const safetyDisplay = gateState === 'ERROR' ? 'block' : 'none';
  if (ui.safetyAlert.style.display !== safetyDisplay) ui.safetyAlert.style.display = safetyDisplay;
}

async function fetchJsonWithTimeout(path, timeoutMs, abortRefKey) {
  const controller = new AbortController();
  state[abortRefKey] = controller;
  const timeout = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const res = await fetch(path, { signal: controller.signal, cache: 'no-store' });
    if (!res.ok) return null;
    return await res.json();
  } finally {
    clearTimeout(timeout);
    if (state[abortRefKey] === controller) state[abortRefKey] = null;
  }
}

async function fetchStatusLite() {
  if (document.hidden || state.statusLiteInFlight) return;
  state.statusLiteInFlight = true;
  try {
    const data = await fetchJsonWithTimeout('/api/status-lite', 2000, 'statusAbort');
    if (!data) return;
    updateStatusLite(data);
  } catch {
    // ignore
  } finally {
    state.statusLiteInFlight = false;
  }
}

async function fetchStatusFull() {
  if (document.hidden || state.statusFullInFlight) return;
  state.statusFullInFlight = true;
  try {
    const data = await fetchJsonWithTimeout('/api/status', 2000, 'fullAbort');
    if (!data) return;
    updateStatus(data);
    if (data.events && Array.isArray(data.events)) {
      state.events = data.events.slice().reverse();
      renderEvents();
    }
  } catch {
    // ignore
  } finally {
    state.statusFullInFlight = false;
  }
}

async function sendControl(action) {
  try {
    await apiFetch('/api/control', { method: 'POST', body: JSON.stringify({ action }) });
  } catch {
    showToast('Brak dostepu do sterowania', 'error');
  }
}

async function sendZero() {
  try {
    await apiFetch('/api/zero', { method: 'POST' });
    showToast('Ustawiono ZERO (stol).', 'success');
  } catch {
    showToast('Nie udalo sie ustawic ZERO.', 'error');
  }
}

function setupControls() {
  if (setupControls._done) return;
  setupControls._done = true;
  ui.openBtn.addEventListener('click', () => sendControl('open'));
  ui.closeBtn.addEventListener('click', () => sendControl('close'));
  ui.stopBtn.addEventListener('click', () => sendControl('stop'));
  ui.toggleBtn.addEventListener('click', () => sendControl('toggle'));
  if (ui.zeroBtn) ui.zeroBtn.addEventListener('click', () => sendZero());
  ui.toggleMode.addEventListener('change', () => {
    state.toggleMode = ui.toggleMode.checked;
    ui.openBtn.disabled = state.toggleMode;
    ui.closeBtn.disabled = state.toggleMode;
  });
}

function setupEvents() {
  if (setupEvents._done) return;
  setupEvents._done = true;
  ui.eventFilter.addEventListener('change', () => {
    state.filter = ui.eventFilter.value;
    renderEvents();
  });
}

function connectWs() {
  if (connectWs._active) return;
  connectWs._active = true;
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
  ws.onclose = () => {
    connectWs._active = false;
    setTimeout(connectWs, 2000);
  };
}

function startPollingOnce() {
  if (state.intervalsStarted) return;
  state.intervalsStarted = true;
  setInterval(fetchStatusLite, 500);
  setInterval(fetchStatusFull, 2000);
}

document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    if (state.statusAbort) state.statusAbort.abort();
    if (state.fullAbort) state.fullAbort.abort();
    return;
  }
  fetchStatusLite();
  fetchStatusFull();
});

window.addEventListener('load', () => {
  setupControls();
  setupEvents();
  fetchStatusFull();
  fetchStatusLite();
  startPollingOnce();
  connectWs();
});
