const qs = (id) => document.getElementById(id);
const core = window.GateOSWeb;
const tokenKey = 'apiToken';
const cameraUrlKey = 'gateos.camera.snapshotUrl';
const snapshotRefreshMs = 5000;
const litePollMs = 1250;
const fullPollMs = 30000;
const wsReconnectBaseMs = 3000;
const wsReconnectMaxMs = 30000;
const wsReconnectCooldownMs = 30000;
const maxWsFailuresBeforeCooldown = 3;

const ui = {
  stateLabel: qs('stateLabel'),
  safetyBadge: qs('safetyBadge'),
  controlBadge: qs('controlBadge'),
  posPct: qs('posPct'),
  progressFill: qs('progressFill'),
  limitOpenValue: qs('limitOpenValue'),
  limitCloseValue: qs('limitCloseValue'),
  liveValue: qs('liveValue'),
  toggleBtn: qs('toggleBtn'),
  toggleTitle: qs('toggleTitle'),
  holdHint: qs('holdHint'),
  holdStateText: qs('holdStateText'),
  mBat: qs('mBat'),
  mCurrent: qs('mCurrent'),
  mRpm: qs('mRpm'),
  mDist: qs('mDist'),
  mArmed: qs('mArmed'),
  mFault: qs('mFault'),
  cWifi: qs('cWifi'),
  cLimO: qs('cLimO'),
  cLimC: qs('cLimC'),
  cameraFrame: qs('cameraFrame'),
  cameraImage: qs('cameraImage'),
  cameraEmbed: qs('cameraEmbed'),
  cameraEmpty: qs('cameraEmpty'),
  cameraEmptyTitle: qs('cameraEmptyTitle'),
  cameraEmptyText: qs('cameraEmptyText'),
  cameraBadge: qs('cameraBadge'),
  cameraUrlInput: qs('cameraUrlInput'),
  cameraSaveBtn: qs('cameraSaveBtn'),
  cameraClearBtn: qs('cameraClearBtn'),
  cameraRefreshBtn: qs('cameraRefreshBtn'),
  cameraOpenLink: qs('cameraOpenLink'),
  cameraConfig: qs('cameraConfig'),
  toast: qs('toast'),
};

const state = {
  liteInFlight: false,
  fullInFlight: false,
  currentState: '',
  currentFaultSeverity: 'none',
  posPercent: 0,
  toggleBusy: false,
  touchHoldEnabled: false,
  holdTimer: null,
  holdTriggered: false,
  holdPointerId: null,
  cameraUrl: '',
  cameraRefreshTimer: null,
  cameraPreviewMode: 'none',
  cameraLoadTimer: null,
  ws: null,
  wsConnected: false,
  wsFailureCount: 0,
  wsReconnectDelayMs: wsReconnectBaseMs,
  wsReconnectTimer: null,
  statusLiteTimer: null,
  statusFullTimer: null,
};

function getToken() {
  return localStorage.getItem(tokenKey) || '';
}

function detectTouchHoldMode() {
  const mobileWidth = window.matchMedia('(max-width: 900px)').matches;
  const touchCapable = navigator.maxTouchPoints > 0 || window.matchMedia('(any-pointer: coarse)').matches;
  return mobileWidth && touchCapable;
}

function safeLocalStorageSet(key, value) {
  try {
    if (value) {
      localStorage.setItem(key, value);
    } else {
      localStorage.removeItem(key);
    }
  } catch {}
}

function safeLocalStorageGet(key) {
  try {
    return localStorage.getItem(key) || '';
  } catch {
    return '';
  }
}

async function apiFetch(path, options = {}) {
  const headers = options.headers || {};
  const token = getToken();
  if (token) headers['X-Api-Key'] = token;
  if (options.body && !headers['Content-Type']) headers['Content-Type'] = 'application/json';
  if (core) await core.ensurePreferredBaseUrlLoaded({ navigate: false, tokenKey });
  const res = await fetch(core ? core.resolveApiUrl(path) : path, { ...options, headers });
  if (!res.ok) throw new Error(`${res.status}`);
  return res;
}

function showToast(msg, type = 'info') {
  ui.toast.textContent = msg;
  ui.toast.className = `toast show ${type}`;
  setTimeout(() => { ui.toast.className = 'toast'; }, 2600);
}

function setText(el, value, fallback = '-') {
  if (!el) return;
  const next = value === undefined || value === null || value === '' ? fallback : `${value}`;
  if (el.textContent !== next) el.textContent = next;
}

function setTone(el, tone) {
  if (!el) return;
  if (tone) {
    el.dataset.tone = tone;
  } else {
    delete el.dataset.tone;
  }
}

function setChip(el, text, cls) {
  if (!el) return;
  setText(el, text);
  el.classList.remove('success', 'warn', 'danger');
  if (cls) el.classList.add(cls);
}

function isMoving(stateName) {
  return stateName === 'opening' || stateName === 'closing';
}

function gateIsMostlyOpen() {
  if (state.currentState === 'open' || state.currentState === 'opening') return true;
  if (state.currentState === 'closed' || state.currentState === 'closing') return false;
  return state.posPercent >= 50;
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

function isFatalFault(severity) {
  return normalizeFaultSeverity(severity) === 'fatal_fault';
}

function updateSafetyBadge(severity) {
  ui.safetyBadge.textContent = safetyBadgeLabel(severity);
  ui.safetyBadge.className = `status-pill ${safetyBadgeClass(severity)}`;
}

function updateControlBadge() {
  const blocked = isFatalFault(state.currentFaultSeverity);
  ui.controlBadge.textContent = `Sterowanie: ${blocked ? 'zablokowane' : 'dozwolone'}`;
  ui.controlBadge.className = `status-pill ${blocked ? 'fatal' : 'neutral'}`;
}

function formatStateLabel(stateLabel, severity) {
  const faultLabel = faultSeverityLabel(severity);
  return faultLabel ? `${stateLabel} / ${faultLabel}` : stateLabel;
}

function updateActionCopy() {
  const blocked = isFatalFault(state.currentFaultSeverity);
  const mostlyOpen = gateIsMostlyOpen();

  ui.toggleBtn.classList.toggle('is-on', mostlyOpen);
  ui.toggleBtn.classList.toggle('is-off', !mostlyOpen);

  if (blocked) {
    setText(ui.toggleTitle, 'Zablokowane');
    setText(ui.holdHint, 'Sterowanie zablokowane przez FATAL_FAULT');
    setText(ui.holdStateText, 'Brama nie przyjmie ruchu');
    return;
  }

  const moving = isMoving(state.currentState);
  setText(ui.toggleTitle, moving ? 'STOP' : (mostlyOpen ? 'Zamknij' : 'Otworz'));

  if (state.touchHoldEnabled) {
    setText(ui.holdHint, moving
      ? 'Przytrzymaj kciukiem, aby zatrzymac'
      : (mostlyOpen ? 'Przytrzymaj kciukiem, aby zamknac' : 'Przytrzymaj kciukiem, aby otworzyc'));
    setText(ui.holdStateText, moving ? 'Po 0,5 s wysle STOP' : 'Po 0,5 s wysle TOGGLE');
  } else {
    setText(ui.holdHint, moving ? 'Kliknij, aby zatrzymac' : (mostlyOpen ? 'Kliknij, aby zamknac' : 'Kliknij, aby otworzyc'));
    setText(ui.holdStateText, moving ? 'Przycisk wysle STOP' : 'Przycisk wysle TOGGLE');
  }
}

function updateLiveValue(isLive) {
  setText(ui.liveValue, isLive ? 'LIVE' : 'OFFLINE');
  setTone(ui.liveValue, isLive ? 'good' : 'muted');
}

function updateLimitTiles(limitOpen, limitClose) {
  setText(ui.limitOpenValue, limitOpen ? 'OK' : 'NIE');
  setTone(ui.limitOpenValue, limitOpen ? 'good' : 'warn');
  setText(ui.limitCloseValue, limitClose ? 'OK' : 'NIE');
  setTone(ui.limitCloseValue, limitClose ? 'good' : 'warn');
}

function updateTelemetry(hb = {}, faultSeverity = 'none') {
  setText(ui.mBat, hb.batV && hb.batV > 0 ? `${hb.batV.toFixed(1)}V` : '--');
  setText(ui.mCurrent, typeof hb.iA === 'number' && hb.iA >= 0 ? `${hb.iA.toFixed(1)}A` : '--');
  setText(ui.mRpm, typeof hb.rpm === 'number' ? `${hb.rpm}` : '--');
  const dist = typeof hb.dist_mm === 'number' && hb.dist_mm >= 0
    ? (hb.dist_mm >= 1000 ? `${(hb.dist_mm / 1000).toFixed(2)}m` : `${hb.dist_mm}mm`)
    : '--';
  setText(ui.mDist, dist);
  const armed = hb.armed ? 'ON' : 'OFF';
  setText(ui.mArmed, armed);
  setTone(ui.mArmed, hb.armed ? 'good' : 'muted');

  if (typeof hb.fault === 'number' && hb.fault > 0) {
    setText(ui.mFault, `${hb.fault}`);
    setTone(ui.mFault, 'danger');
  } else {
    setText(ui.mFault, safetyBadgeLabel(faultSeverity));
    setTone(ui.mFault, normalizeFaultSeverity(faultSeverity) === 'none' ? 'good' : 'warn');
  }
}

function applyToggleAvailability(severity, gateState) {
  const normalized = normalizeFaultSeverity(severity, gateState);
  state.currentFaultSeverity = normalized;
  const fatalFault = isFatalFault(normalized);
  ui.toggleBtn.disabled = fatalFault;
  ui.toggleBtn.classList.toggle('error', fatalFault);
  ui.toggleBtn.classList.toggle('moving', isMoving(gateState));
  updateControlBadge();
  updateActionCopy();
}

function updateCoreState(gateState, faultSeverity, percent) {
  state.currentState = gateState;
  state.posPercent = percent;

  const label = formatStateLabel(gateState.toUpperCase(), faultSeverity);
  setText(ui.stateLabel, label);
  ui.stateLabel.className = `state-label ${gateState}`;

  setText(ui.posPct, `${percent}%`);
  ui.progressFill.style.width = `${percent}%`;
  updateSafetyBadge(faultSeverity);
  applyToggleAvailability(faultSeverity, gateState);
  updateLiveValue(true);
}

function updateUI(data) {
  if (!data) return;
  const gate = data.gate || {};
  const wifi = data.wifi || {};
  const hb = data.hb || {};
  const inputs = data.inputs || {};

  const gateState = (gate.state || 'unknown').toLowerCase();
  const faultSeverity = normalizeFaultSeverity(gate.faultSeverity, gateState);
  const pct = typeof gate.positionPercent === 'number'
    ? Math.min(100, Math.max(0, Math.round(gate.positionPercent)))
    : 0;

  updateCoreState(gateState, faultSeverity, pct);
  updateLimitTiles(Boolean(inputs.limitOpen), Boolean(inputs.limitClose));
  updateTelemetry(hb, faultSeverity);

  setChip(ui.cWifi, `WiFi: ${wifi.connected ? (wifi.ssid || 'OK') : 'OFF'}`, wifi.connected ? 'success' : 'warn');
  setChip(ui.cLimO, `OPEN: ${inputs.limitOpen ? 'ON' : 'OFF'}`, inputs.limitOpen ? 'success' : '');
  setChip(ui.cLimC, `CLOSE: ${inputs.limitClose ? 'ON' : 'OFF'}`, inputs.limitClose ? 'success' : '');
}

function updateLite(data) {
  if (!data) return;
  const gateState = (data.state || '').toLowerCase();
  if (!gateState) return;

  const faultSeverity = normalizeFaultSeverity(data.faultSeverity, gateState);
  const pct = typeof data.positionPercent === 'number'
    ? Math.min(100, Math.max(0, Math.round(data.positionPercent)))
    : 0;

  updateCoreState(gateState, faultSeverity, pct);
  updateLimitTiles(Boolean(data.limitOpen), Boolean(data.limitClose));
  setChip(ui.cLimO, `OPEN: ${data.limitOpen ? 'ON' : 'OFF'}`, data.limitOpen ? 'success' : '');
  setChip(ui.cLimC, `CLOSE: ${data.limitClose ? 'ON' : 'OFF'}`, data.limitClose ? 'success' : '');

  if (typeof data.rpm === 'number') setText(ui.mRpm, `${data.rpm}`);
  if (typeof data.iA === 'number' && data.iA >= 0) setText(ui.mCurrent, `${data.iA.toFixed(1)}A`);
}

async function fetchJson(path, timeoutMs) {
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), timeoutMs);
  try {
    const headers = {};
    const token = getToken();
    if (token) headers['X-Api-Key'] = token;
    if (core) await core.ensurePreferredBaseUrlLoaded({ navigate: false, tokenKey });
    const res = await fetch(core ? core.resolveApiUrl(path) : path, { signal: ctrl.signal, cache: 'no-store', headers });
    if (!res.ok) return null;
    return await res.json();
  } catch {
    return null;
  } finally {
    clearTimeout(timer);
  }
}

async function fetchLite() {
  if (document.hidden || state.liteInFlight || state.wsConnected) return;
  state.liteInFlight = true;
  try {
    const data = await fetchJson('/api/status-lite', 2000);
    if (data) updateLite(data);
  } finally {
    state.liteInFlight = false;
  }
}

async function fetchFull() {
  if (document.hidden || state.fullInFlight) return;
  state.fullInFlight = true;
  try {
    const data = await fetchJson('/api/status', 2000);
    if (data) updateUI(data);
  } finally {
    state.fullInFlight = false;
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

async function performToggleAction() {
  if (state.toggleBusy || ui.toggleBtn.disabled) return;
  state.toggleBusy = true;
  try {
    if (isMoving(state.currentState)) {
      await sendControl('stop');
    } else {
      await sendControl('toggle');
    }
  } finally {
    setTimeout(() => {
      state.toggleBusy = false;
      ui.toggleBtn.classList.remove('hold-fired');
      updateActionCopy();
    }, 400);
  }
}

function vibrate(ms) {
  if (!state.touchHoldEnabled) return;
  if (typeof navigator.vibrate === 'function') navigator.vibrate(ms);
}

function clearHoldTimer() {
  if (state.holdTimer) {
    clearTimeout(state.holdTimer);
    state.holdTimer = null;
  }
}

function resetHoldState() {
  clearHoldTimer();
  state.holdTriggered = false;
  state.holdPointerId = null;
  ui.toggleBtn.classList.remove('is-holding');
  updateActionCopy();
}

function triggerHoldAction() {
  state.holdTriggered = true;
  ui.toggleBtn.classList.remove('is-holding');
  ui.toggleBtn.classList.add('hold-fired');
  setText(ui.holdStateText, 'Aktywowano');
  vibrate(30);
  performToggleAction();
}

function onPointerDown(event) {
  if (!state.touchHoldEnabled || ui.toggleBtn.disabled || state.toggleBusy) return;
  if (event.pointerType === 'mouse') return;
  event.preventDefault();
  clearHoldTimer();
  state.holdTriggered = false;
  state.holdPointerId = event.pointerId;
  ui.toggleBtn.classList.add('is-holding');
  setText(ui.holdStateText, 'Trzymaj jeszcze chwile');
  state.holdTimer = setTimeout(triggerHoldAction, 500);
}

function onPointerEnd(event) {
  if (!state.touchHoldEnabled) return;
  if (state.holdPointerId !== null && event.pointerId !== undefined && event.pointerId !== state.holdPointerId) return;
  if (state.holdTriggered) {
    state.holdTriggered = false;
    state.holdPointerId = null;
    setTimeout(() => {
      ui.toggleBtn.classList.remove('hold-fired');
      updateActionCopy();
    }, 180);
    return;
  }
  resetHoldState();
}

function buildCameraSrc(url) {
  try {
    const parsed = new URL(core ? core.resolveHttpUrl(url) : url, window.location.href);
    parsed.searchParams.set('_ts', Date.now().toString());
    return parsed.toString();
  } catch {
    const sep = url.includes('?') ? '&' : '?';
    return `${url}${sep}_ts=${Date.now()}`;
  }
}

function looksLikeSnapshotUrl(url) {
  const value = (url || '').toLowerCase();
  return /\.(jpg|jpeg|png|gif|bmp|webp)(\?|$)/.test(value)
    || value.includes('/shot.jpg')
    || value.includes('/snapshot')
    || /(?:^|[?&])(jpg|jpeg|snapshot|snap|capture|still)=/.test(value)
    || value.includes('action=snapshot');
}

function parseCameraUrl(url) {
  const resolvedUrl = core ? core.resolveHttpUrl(url) : url;
  try {
    const parsed = new URL(resolvedUrl, window.location.href);
    return {
      rawUrl: parsed.toString(),
      previewUrl: (() => {
        const clean = new URL(parsed.toString());
        clean.username = '';
        clean.password = '';
        return clean.toString();
      })(),
      hasEmbeddedCredentials: Boolean(parsed.username || parsed.password),
    };
  } catch {
    return {
      rawUrl: resolvedUrl,
      previewUrl: resolvedUrl,
      hasEmbeddedCredentials: false,
    };
  }
}

function clearCameraLoadTimer() {
  if (state.cameraLoadTimer) {
    clearTimeout(state.cameraLoadTimer);
    state.cameraLoadTimer = null;
  }
}

function resetCameraPreviewMedia() {
  clearCameraLoadTimer();
  state.cameraPreviewMode = 'none';
  ui.cameraFrame.classList.remove('has-image', 'has-embed');

  if (ui.cameraImage.getAttribute('src')) {
    ui.cameraImage.removeAttribute('src');
  }

  if (ui.cameraEmbed.getAttribute('src')) {
    ui.cameraEmbed.removeAttribute('src');
  }
}

function activateCameraImagePreview() {
  clearCameraLoadTimer();
  state.cameraPreviewMode = 'image';
  ui.cameraFrame.classList.remove('has-embed');
  ui.cameraFrame.classList.add('has-image');
  ui.cameraBadge.textContent = looksLikeSnapshotUrl(state.cameraUrl) ? 'SNAPSHOT' : 'PODGLAD';
  ui.cameraBadge.className = 'status-pill ok';
  setText(ui.cameraEmptyTitle, 'Podglad kamery aktywny');
  setText(
    ui.cameraEmptyText,
    looksLikeSnapshotUrl(state.cameraUrl)
      ? 'Snapshot JPEG jest odswiezany lokalnie w tej przegladarce, bez proxy przez ESP32.'
      : 'Obraz jest ladowany bezposrednio przez te przegladarke, bez proxy przez ESP32.'
  );
}

function activateCameraEmbedPreview() {
  clearCameraLoadTimer();
  state.cameraPreviewMode = 'iframe';
  const cameraUrl = parseCameraUrl(state.cameraUrl);
  ui.cameraFrame.classList.remove('has-image');
  ui.cameraFrame.classList.add('has-embed');
  ui.cameraBadge.textContent = 'OSADZONO';
  ui.cameraBadge.className = 'status-pill neutral';
  setText(ui.cameraEmptyTitle, 'Osadzony podglad strony kamery');
  setText(ui.cameraEmptyText, 'Bezposredni obraz nie zaladowal sie jako snapshot, wiec UI osadza strone kamery.');
  stopCameraRefresh();
  ui.cameraEmbed.src = cameraUrl.previewUrl;
}

function updateCameraOpenLink() {
  const hasUrl = Boolean(state.cameraUrl);
  if (hasUrl) {
    ui.cameraOpenLink.href = parseCameraUrl(state.cameraUrl).rawUrl;
    ui.cameraOpenLink.classList.remove('is-disabled');
  } else {
    ui.cameraOpenLink.href = '#';
    ui.cameraOpenLink.classList.add('is-disabled');
  }
}

function openCameraConfig(shouldFocus = false) {
  if (ui.cameraConfig && typeof ui.cameraConfig.open === 'boolean') ui.cameraConfig.open = true;
  if (!shouldFocus || !ui.cameraUrlInput) return;
  if (!ui.cameraUrlInput) return;
  try {
    ui.cameraUrlInput.focus();
    ui.cameraUrlInput.select();
  } catch {}
}

function showCameraPlaceholder(title, text, badgeText = 'BRAK OBRAZU', badgeClass = 'neutral') {
  resetCameraPreviewMedia();
  setText(ui.cameraEmptyTitle, title);
  setText(ui.cameraEmptyText, text);
  ui.cameraBadge.textContent = badgeText;
  ui.cameraBadge.className = `status-pill ${badgeClass}`;
}

function refreshCamera(force = false) {
  const cameraUrl = parseCameraUrl(state.cameraUrl);
  const snapshotLike = looksLikeSnapshotUrl(cameraUrl.previewUrl);
  if (!state.cameraUrl) {
    showCameraPlaceholder(
      'Dodaj lokalny URL snapshotu',
      'Wklej bezposredni adres JPG lub PNG. Ten adres jest zapisywany tylko lokalnie w tej przegladarce.',
      'BRAK URL',
      'neutral'
    );
    openCameraConfig(false);
    return;
  }
  if (document.hidden && !force) return;

  clearCameraLoadTimer();
  ui.cameraFrame.classList.remove('has-embed');
  ui.cameraFrame.classList.add('has-image');
  ui.cameraBadge.textContent = state.cameraPreviewMode === 'image' ? 'ODSWIEZANIE' : 'LADOWANIE';
  ui.cameraBadge.className = 'status-pill neutral';
  state.cameraPreviewMode = 'loading-image';
  ui.cameraImage.src = buildCameraSrc(cameraUrl.previewUrl);
  state.cameraLoadTimer = setTimeout(() => {
    if (state.cameraPreviewMode === 'loading-image' && state.cameraUrl) {
      if (cameraUrl.hasEmbeddedCredentials) {
        showCameraPlaceholder(
          'Wymagana autoryzacja przegladarki',
          'Ta przegladarka blokuje osadzanie adresu z loginem i haslem. Otworz kamere w nowej karcie raz, wroc tutaj i kliknij Odswiez.',
          'WYMAGA LOGOWANIA',
          'warn'
        );
      } else {
        if (snapshotLike) {
          showCameraPlaceholder(
            'Nie udalo sie pobrac snapshotu JPEG',
            'Sprawdz bezposredni URL obrazka albo otworz kamere w nowej karcie i skopiuj docelowy adres JPG.',
            'BRAK SNAPSHOTA',
            'warn'
          );
        } else {
          activateCameraEmbedPreview();
        }
      }
    }
  }, snapshotLike ? 5000 : 3200);
}

function stopCameraRefresh() {
  if (state.cameraRefreshTimer) {
    clearInterval(state.cameraRefreshTimer);
    state.cameraRefreshTimer = null;
  }
}

function startCameraRefresh() {
  stopCameraRefresh();
  if (!state.cameraUrl) return;
  refreshCamera(true);
  const refreshMs = looksLikeSnapshotUrl(state.cameraUrl) ? snapshotRefreshMs : 15000;
  state.cameraRefreshTimer = setInterval(() => {
    if (!document.hidden && state.cameraPreviewMode !== 'iframe') refreshCamera(true);
  }, refreshMs);
}

function applyCameraUrl(url) {
  state.cameraUrl = (url || '').trim();
  safeLocalStorageSet(cameraUrlKey, state.cameraUrl);
  if (ui.cameraUrlInput && ui.cameraUrlInput.value !== state.cameraUrl) ui.cameraUrlInput.value = state.cameraUrl;
  updateCameraOpenLink();
  if (state.cameraUrl) {
    if (ui.cameraConfig && typeof ui.cameraConfig.open === 'boolean') ui.cameraConfig.open = false;
    startCameraRefresh();
    showToast('URL kamery zapisany lokalnie');
  } else {
    if (ui.cameraConfig && typeof ui.cameraConfig.open === 'boolean') ui.cameraConfig.open = true;
    stopCameraRefresh();
    refreshCamera(true);
  }
}

function clearWsReconnectTimer() {
  if (!state.wsReconnectTimer) return;
  clearTimeout(state.wsReconnectTimer);
  state.wsReconnectTimer = null;
}

function scheduleWsReconnect(delayMs) {
  if (document.hidden) return;
  clearWsReconnectTimer();
  state.wsReconnectTimer = setTimeout(() => {
    state.wsReconnectTimer = null;
    connectWs();
  }, delayMs);
}

function closeWsConnection() {
  clearWsReconnectTimer();
  state.wsConnected = false;
  const ws = state.ws;
  state.ws = null;
  if (!ws) return;
  if (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING) {
    try {
      ws.close(1000, 'page_hidden');
    } catch {}
  }
}

function stopStatusPolling() {
  if (state.statusLiteTimer) {
    clearInterval(state.statusLiteTimer);
    state.statusLiteTimer = null;
  }
  if (state.statusFullTimer) {
    clearInterval(state.statusFullTimer);
    state.statusFullTimer = null;
  }
}

function startStatusPolling() {
  stopStatusPolling();
  fetchLite();
  setTimeout(() => {
    if (!document.hidden) fetchFull();
  }, 250);
  state.statusLiteTimer = setInterval(fetchLite, litePollMs);
  state.statusFullTimer = setInterval(fetchFull, fullPollMs);
}

async function connectWs() {
  if (document.hidden) return;
  if (state.ws && (state.ws.readyState === WebSocket.OPEN || state.ws.readyState === WebSocket.CONNECTING)) return;
  clearWsReconnectTimer();
  try {
    if (core) await core.ensurePreferredBaseUrlLoaded({ navigate: false, tokenKey });
    const url = core ? core.resolveWebSocketUrl('/ws') : `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`;
    const ws = new WebSocket(url);
    state.ws = ws;
    ws.onopen = () => {
      state.wsConnected = true;
      state.wsFailureCount = 0;
      state.wsReconnectDelayMs = wsReconnectBaseMs;
    };
    ws.onmessage = (evt) => {
      try {
        const msg = JSON.parse(evt.data);
        if (msg.type === 'status') updateUI(msg.data);
      } catch {}
    };
    ws.onerror = () => {};
    ws.onclose = (event) => {
      if (state.ws === ws) state.ws = null;
      state.wsConnected = false;

      const rapidFailure = event.code === 1006 || event.code === 1007 || event.code === 1002 || event.wasClean === false;
      if (rapidFailure) {
        state.wsFailureCount += 1;
      } else {
        state.wsFailureCount = 0;
      }

      if (state.wsFailureCount >= maxWsFailuresBeforeCooldown) {
        state.wsFailureCount = 0;
        state.wsReconnectDelayMs = wsReconnectBaseMs;
        scheduleWsReconnect(wsReconnectCooldownMs);
        return;
      }

      const delayMs = state.wsReconnectDelayMs;
      state.wsReconnectDelayMs = Math.min(wsReconnectMaxMs, Math.round(state.wsReconnectDelayMs * 1.8));
      scheduleWsReconnect(delayMs);
    };
  } catch {
    state.ws = null;
    state.wsConnected = false;
    const delayMs = state.wsReconnectDelayMs;
    state.wsReconnectDelayMs = Math.min(wsReconnectMaxMs, Math.round(state.wsReconnectDelayMs * 1.8));
    scheduleWsReconnect(delayMs);
  }
}

function bindCameraUi() {
  state.cameraUrl = safeLocalStorageGet(cameraUrlKey);
  if (ui.cameraUrlInput) ui.cameraUrlInput.value = state.cameraUrl;
  if (!state.cameraUrl && ui.cameraConfig && typeof ui.cameraConfig.open === 'boolean') ui.cameraConfig.open = true;
  updateCameraOpenLink();
  refreshCamera(true);

  ui.cameraImage.addEventListener('load', () => {
    activateCameraImagePreview();
  });

  ui.cameraImage.addEventListener('error', () => {
    if (!state.cameraUrl) return;
    const cameraUrl = parseCameraUrl(state.cameraUrl);
    const snapshotLike = looksLikeSnapshotUrl(cameraUrl.previewUrl);
    if (cameraUrl.hasEmbeddedCredentials) {
      showCameraPlaceholder(
        'Wymagana autoryzacja przegladarki',
        'Kliknij Otworz kamerę w nowej karcie, pozwol przegladarce zapamietac logowanie, a potem wroc i kliknij Odswiez.',
        'WYMAGA LOGOWANIA',
        'warn'
      );
      return;
    }
    if (snapshotLike) {
      showCameraPlaceholder(
        'Nie udalo sie pobrac snapshotu JPEG',
        'Ten adres nie zwrocil obrazka. Upewnij sie, ze podajesz bezposredni URL JPG lub PNG, a nie strone HTML.',
        'BRAK SNAPSHOTA',
        'warn'
      );
      return;
    }
    activateCameraEmbedPreview();
  });

  ui.cameraEmbed.addEventListener('load', () => {
    if (state.cameraPreviewMode !== 'iframe') return;
    ui.cameraBadge.textContent = 'OSADZONO';
    ui.cameraBadge.className = 'status-pill ok';
  });

  ui.cameraSaveBtn.addEventListener('click', () => {
    applyCameraUrl(ui.cameraUrlInput.value);
  });

  ui.cameraUrlInput.addEventListener('keydown', (event) => {
    if (event.key !== 'Enter') return;
    event.preventDefault();
    applyCameraUrl(ui.cameraUrlInput.value);
  });

  ui.cameraClearBtn.addEventListener('click', () => {
    ui.cameraUrlInput.value = '';
    applyCameraUrl('');
  });

  ui.cameraRefreshBtn.addEventListener('click', () => {
    if (!state.cameraUrl) {
      openCameraConfig(true);
      showToast('Najpierw wklej lokalny URL snapshotu');
      return;
    }
    refreshCamera(true);
  });

  ui.cameraOpenLink.addEventListener('click', (event) => {
    if (state.cameraUrl) return;
    event.preventDefault();
    openCameraConfig(true);
    showToast('Najpierw wklej lokalny URL snapshotu');
  });
}

function bindToggleUi() {
  ui.toggleBtn.addEventListener('pointerdown', onPointerDown);
  ui.toggleBtn.addEventListener('pointerup', onPointerEnd);
  ui.toggleBtn.addEventListener('pointercancel', onPointerEnd);
  ui.toggleBtn.addEventListener('pointerleave', onPointerEnd);
  ui.toggleBtn.addEventListener('click', (event) => {
    if (state.touchHoldEnabled) {
      event.preventDefault();
      return;
    }
    performToggleAction();
  });
}

function refreshInteractionMode() {
  state.touchHoldEnabled = detectTouchHoldMode();
  updateActionCopy();
}

document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    clearHoldTimer();
    stopCameraRefresh();
    stopStatusPolling();
    closeWsConnection();
    return;
  }
  startStatusPolling();
  startCameraRefresh();
  connectWs();
});

window.addEventListener('resize', refreshInteractionMode);

window.addEventListener('load', async () => {
  if (core) {
    await core.ensurePreferredBaseUrlLoaded({ navigate: true, tokenKey });
    if (core.isRedirectingToPreferredBase()) return;
  }
  bindCameraUi();
  bindToggleUi();
  refreshInteractionMode();
  startStatusPolling();
  connectWs();
});
