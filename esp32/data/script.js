const qs = (id) => document.getElementById(id);
const tokenKey = 'apiToken';
const core = window.GateOSWeb;
const logger = core.createLogger('dashboard');
const apiClient = core.createApiClient({ tokenKey, logger: core.createLogger('dashboard-api') });
const scheduler = core.createScheduler();
const wsManager = core.createWebSocketManager({
  key: 'gateos-dashboard-ws',
  path: '/ws',
  tokenKey,
  logger: core.createLogger('dashboard-ws'),
  baseDelayMs: 3000,
  maxDelayMs: 30000,
  cooldownMs: 30000,
  maxRapidFailures: 3,
  heartbeatIntervalMs: 15000,
  staleTimeoutMs: 45000,
});
const scheduleLiteRender = core.createRafBatcher((data) => updateStatusLite(data));
const scheduleFullRender = core.createRafBatcher((data) => {
  updateStatus(data);
  if (data && data.events && Array.isArray(data.events)) {
    const nextEvents = data.events.slice().reverse();
    state.events = nextEvents;
    renderEvents();
  }
});

const ui = {
  gateState: qs('gateState'),
  gateProgress: qs('gateProgress'),
  gatePercent: qs('gatePercent'),
  safetyBadge: qs('safetyBadge'),
  controlBadge: qs('controlBadge'),
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
  faultSeverityValue: qs('faultSeverityValue'),
  faultCodeValue: qs('faultCodeValue'),
  faultReasonValue: qs('faultReasonValue'),
  warningCountValue: qs('warningCountValue'),
  softFaultCountValue: qs('softFaultCountValue'),
  stopReasonValue: qs('stopReasonValue'),
  diagUptimeValue: qs('diagUptimeValue'),
  resetReasonValue: qs('resetReasonValue'),
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
  currentFaultSeverity: 'none',
  intervalsStarted: false,
  statusLiteInFlight: false,
  statusFullInFlight: false,
  statusAbort: null,
  fullAbort: null,
  eventListSignature: '',
  wsConnected: false,
  lastFullAppliedAt: 0,
  lastLiteAppliedAt: 0,
};

function getToken() {
  return localStorage.getItem(tokenKey) || '';
}

async function apiFetch(path, options = {}) {
  const result = await apiClient.request(path, {
    ...options,
    responseType: 'raw'
  });
  return result.res;
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
    10: 'over_current',
  };
  return map[code] || 'unknown';
}

function normalizeFaultSeverity(severity, gateState = '') {
  const value = typeof severity === 'string' ? severity.toLowerCase() : '';
  if (value === 'warning' || value === 'soft_fault' || value === 'fatal_fault') return value;
  return gateState === 'error' ? 'fatal_fault' : 'none';
}

function faultSeverityLabel(severity) {
  switch (normalizeFaultSeverity(severity)) {
    case 'warning':
      return 'WARNING';
    case 'soft_fault':
      return 'SOFT_FAULT';
    case 'fatal_fault':
      return 'FATAL_FAULT';
    default:
      return '';
  }
}

function safetyBadgeLabel(severity) {
  const normalized = normalizeFaultSeverity(severity);
  return normalized === 'none' ? 'OK' : faultSeverityLabel(normalized);
}

function safetyBadgeClass(severity) {
  switch (normalizeFaultSeverity(severity)) {
    case 'warning':
      return 'warn';
    case 'soft_fault':
      return 'soft';
    case 'fatal_fault':
      return 'fatal';
    case 'none':
    default:
      return 'ok';
  }
}

function setText(el, value) {
  if (!el) return;
  const next = value === undefined || value === null || value === '' ? '-' : `${value}`;
  if (el.textContent !== next) el.textContent = next;
}

function updateSafetyBadge(severity) {
  if (!ui.safetyBadge) return;
  ui.safetyBadge.textContent = safetyBadgeLabel(severity);
  ui.safetyBadge.className = `status-pill ${safetyBadgeClass(severity)}`;
}

function updateControlBadge() {
  if (!ui.controlBadge) return;
  const blocked = isFatalFault(state.currentFaultSeverity);
  ui.controlBadge.textContent = `Sterowanie: ${blocked ? 'zablokowane' : 'dozwolone'}`;
  ui.controlBadge.className = `status-pill ${blocked ? 'fatal' : 'neutral'}`;
}

function updateSafetyDiagnostics(details = {}) {
  if (details.faultSeverity !== undefined) setText(ui.faultSeverityValue, safetyBadgeLabel(details.faultSeverity));
  if (details.faultCode !== undefined) setText(ui.faultCodeValue, details.faultCode);
  if (details.faultReason !== undefined) setText(ui.faultReasonValue, stopReasonLabel(details.faultReason));
  if (details.warningCount !== undefined) setText(ui.warningCountValue, details.warningCount);
  if (details.softFaultCount !== undefined) setText(ui.softFaultCountValue, details.softFaultCount);
  if (details.stopReason !== undefined) setText(ui.stopReasonValue, stopReasonLabel(details.stopReason));
  if (details.uptimeMs !== undefined) setText(ui.diagUptimeValue, formatUptime(details.uptimeMs));
  if (details.resetReason !== undefined) setText(ui.resetReasonValue, details.resetReason || 'unknown');
}

function formatGateStateLabel(stateLabel, severity) {
  const faultLabel = faultSeverityLabel(severity);
  return faultLabel ? `${stateLabel} / ${faultLabel}` : stateLabel;
}

function isFatalFault(severity) {
  return normalizeFaultSeverity(severity) === 'fatal_fault';
}

function applyDashboardControlState() {
  const fatalFault = isFatalFault(state.currentFaultSeverity);
  ui.openBtn.disabled = fatalFault || state.toggleMode;
  ui.closeBtn.disabled = fatalFault || state.toggleMode;
  ui.toggleBtn.disabled = fatalFault;
  updateControlBadge();
}

function updateSafetyAlert(severity, faultReason, faultCode, warningCount, softFaultCount) {
  const normalized = normalizeFaultSeverity(severity);
  if (normalized === 'none') {
    if (ui.safetyAlert.style.display !== 'none') ui.safetyAlert.style.display = 'none';
    return;
  }

  const parts = [faultSeverityLabel(normalized)];
  const reason = stopReasonLabel(typeof faultReason === 'number' ? faultReason : 0);
  if (reason !== 'none') parts.push(`reason=${reason}`);
  if (typeof faultCode === 'number') parts.push(`code=${faultCode}`);
  if (normalized === 'warning' && typeof warningCount === 'number') parts.push(`warnings=${warningCount}`);
  if (normalized === 'soft_fault' && typeof softFaultCount === 'number') parts.push(`soft_faults=${softFaultCount}`);
  parts.push(normalized === 'fatal_fault' ? 'Sterowanie ruchem zablokowane.' : 'Sterowanie pozostaje aktywne.');

  const nextText = parts.join(' | ');
  if (ui.safetyAlert.textContent !== nextText) ui.safetyAlert.textContent = nextText;
  if (ui.safetyAlert.style.display !== 'block') ui.safetyAlert.style.display = 'block';
}

function setChip(el, text, state) {
  if (!el) return;
  if (el.textContent !== text) el.textContent = text;
  el.classList.remove('success', 'warn', 'danger');
  if (state) el.classList.add(state);
}

function setGatePercent(percent) {
  const p = Math.max(0, Math.min(100, percent));
  // Firmware exposes positionPercent as gate opening percentage: 0% closed, 100% open.
  const width = `${p}%`;
  if (ui.gateProgress.style.width !== width) ui.gateProgress.style.width = width;
  if (ui.gatePercent.textContent !== `${p}`) ui.gatePercent.textContent = `${p}`;
}

function addEvent(ev) {
  if (!ev || !ev.message) return;
  state.events.unshift(ev);
  if (state.events.length > 10) state.events.pop();
  renderEvents();
}

function renderEvents() {
  const list = state.events.filter(e => state.filter === 'all' || e.level === state.filter);
  const signature = `${state.filter}|${list.map((ev) => `${ev.level || 'info'}|${ev.message || ''}|${ev.ts || 0}`).join('||')}`;
  if (state.eventListSignature === signature) return;
  state.eventListSignature = signature;

  const fragment = document.createDocumentFragment();
  if (list.length === 0) {
    const li = document.createElement('li');
    li.className = 'event info';
    li.textContent = 'Brak zdarzen';
    fragment.appendChild(li);
    ui.eventList.replaceChildren(fragment);
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
    fragment.appendChild(li);
  });
  ui.eventList.replaceChildren(fragment);
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

  const gateStateRaw = (gate.state || 'unknown').toString().toLowerCase();
  const faultSeverity = normalizeFaultSeverity(gate.faultSeverity, gateStateRaw);
  state.currentFaultSeverity = faultSeverity;
  const gateState = formatGateStateLabel(gateStateRaw.toUpperCase(), faultSeverity);
  if (ui.gateState.textContent !== gateState) ui.gateState.textContent = gateState;
  updateSafetyBadge(faultSeverity);
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

  updateSafetyDiagnostics({
    faultSeverity,
    faultCode: gate.faultCode,
    faultReason: gate.faultReason,
    warningCount: gate.warningCount,
    softFaultCount: gate.softFaultCount,
    stopReason: gate.stopReason,
    uptimeMs: data.uptimeMs,
    resetReason: data.runtime && data.runtime.resetReason,
  });

  updateSafetyAlert(faultSeverity, gate.faultReason, gate.faultCode, gate.warningCount, gate.softFaultCount);
  applyDashboardControlState();
}

function updateStatusLite(data) {
  if (!data) return;
  const rawState = (data.state || '').toString();
  if (!rawState) return;
  const gateStateRaw = rawState.toLowerCase();
  const faultSeverity = normalizeFaultSeverity(data.faultSeverity, gateStateRaw);
  state.currentFaultSeverity = faultSeverity;
  const gateState = formatGateStateLabel(rawState.toUpperCase(), faultSeverity);
  if (ui.gateState.textContent !== gateState) ui.gateState.textContent = gateState;
  updateSafetyBadge(faultSeverity);

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

  updateSafetyDiagnostics({
    faultSeverity,
    faultCode: data.faultCode,
    faultReason: data.faultReason,
    warningCount: data.warningCount,
    softFaultCount: data.softFaultCount,
  });

  updateSafetyAlert(faultSeverity, data.faultReason, data.faultCode, data.warningCount, data.softFaultCount);
  applyDashboardControlState();
}

async function fetchJsonWithTimeout(path, timeoutMs, abortRefKey) {
  try {
    const requestKey = abortRefKey === 'statusAbort' ? 'dashboard-status-lite' : 'dashboard-status-full';
    const result = await apiClient.request(path, {
      requestKey,
      timeoutMs,
      responseType: 'json'
    });
    return result.data;
  } finally {
    state[abortRefKey] = null;
  }
}

async function fetchStatusLite() {
  if (document.hidden || state.statusLiteInFlight || state.wsConnected) return;
  state.statusLiteInFlight = true;
  state.statusAbort = true;
  try {
    const data = await fetchJsonWithTimeout('/api/status-lite', 2000, 'statusAbort');
    if (!data) return;
    state.lastLiteAppliedAt = core.nowMs();
    scheduleLiteRender(data);
  } catch {
    // ignore
  } finally {
    state.statusLiteInFlight = false;
  }
}

async function fetchStatusFull() {
  if (document.hidden || state.statusFullInFlight) return;
  state.statusFullInFlight = true;
  state.fullAbort = true;
  try {
    const data = await fetchJsonWithTimeout('/api/status', 2000, 'fullAbort');
    if (!data) return;
    state.lastFullAppliedAt = core.nowMs();
    scheduleFullRender(data);
  } catch {
    // ignore
  } finally {
    state.statusFullInFlight = false;
  }
}

async function sendControl(action) {
  if (isFatalFault(state.currentFaultSeverity) && action !== 'stop') {
    showToast('Ruch zablokowany: FATAL_FAULT', 'error');
    return;
  }
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
    applyDashboardControlState();
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
  if (connectWs._done) return;
  connectWs._done = true;

  const unsubscribe = wsManager.subscribe({
    open() {
      state.wsConnected = true;
      fetchStatusFull();
    },
    close() {
      state.wsConnected = false;
    },
    stale() {
      state.wsConnected = false;
      fetchStatusFull();
    },
    message(evt) {
      try {
        const msg = JSON.parse(evt.data);
        if (msg.type === 'status' && msg.data) {
          state.lastFullAppliedAt = core.nowMs();
          scheduleFullRender(msg.data);
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
    }
  });

  scheduler.addCleanup(unsubscribe);
  wsManager.setVisibility(!document.hidden);
  wsManager.setOnline(navigator.onLine !== false);
  wsManager.connect();
}

function startPollingOnce() {
  if (state.intervalsStarted) return;
  state.intervalsStarted = true;
  scheduler.every(fetchStatusLite, 1500);
  scheduler.every(fetchStatusFull, 30000);
}

core.bindPageLifecycle({
  onHide() {
    apiClient.abort('dashboard-status-lite', 'page_hidden');
    apiClient.abort('dashboard-status-full', 'page_hidden');
    wsManager.setVisibility(false);
  },
  onShow() {
    wsManager.setVisibility(true);
    fetchStatusLite();
    fetchStatusFull();
  },
  onWake() {
    wsManager.reconnect('wake');
    fetchStatusFull();
  },
  onOnline() {
    wsManager.setOnline(true);
    fetchStatusFull();
  },
  onOffline() {
    state.wsConnected = false;
    wsManager.setOnline(false);
  }
});

window.addEventListener('load', async () => {
  await core.ensurePreferredBaseUrlLoaded({ navigate: true, tokenKey });
  if (core.isRedirectingToPreferredBase()) return;
  setupControls();
  setupEvents();
  fetchStatusFull();
  fetchStatusLite();
  startPollingOnce();
  connectWs();
});
