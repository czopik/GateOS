const qs = (id) => document.getElementById(id);
const core = window.GateOSWeb;
const tokenKey = 'apiToken';
const cameraUrlKey = 'gateos.camera.snapshotUrl';
const snapshotRefreshMs = 5000;
const litePollMs = 1250;
const wsManager = core.createWebSocketManager({
  key: 'gateos-control-ws',
  path: '/ws',
  tokenKey,
  logger: core.createLogger('control-ws'),
  baseDelayMs: 3000,
  maxDelayMs: 30000,
  cooldownMs: 30000,
  maxRapidFailures: 3,
  heartbeatIntervalMs: 15000,
  staleTimeoutMs: 90000,
});

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
  cameraFrame: qs('cameraFrame'),
  cameraImage: qs('cameraImage'),
  cameraEmbed: qs('cameraEmbed'),
  cameraEmpty: qs('cameraEmpty'),
  cameraEmptyTitle: qs('cameraEmptyTitle'),
  cameraEmptyText: qs('cameraEmptyText'),
  toast: qs('toast'),
};

const state = {
  liteInFlight: false,
  currentState: '',
  currentFaultSeverity: 'none',
  posPercent: 0,
  toggleBusy: false,
  touchFeedback: false,
  holdTimer: null,
  holdTriggered: false,
  holdPointerId: null,
  cameraUrl: '',
  cameraRefreshTimer: null,
  cameraPreviewMode: 'none',
  cameraLoadTimer: null,
  wsConnected: false,
  statusLiteTimer: null,
};

function getToken() {
  return localStorage.getItem(tokenKey) || '';
}

function detectTouchFeedback() {
  return navigator.maxTouchPoints > 0 || window.matchMedia('(any-pointer: coarse)').matches;
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
  setTimeout(() => {
    ui.toast.className = 'toast';
  }, 2600);
}

function setText(el, value, fallback = '-') {
  if (!el) return;
  const next = value === undefined || value === null || value === '' ? fallback : `${value}`;
  if (el.textContent !== next) el.textContent = next;
}

function isMoving(stateName) {
  return stateName === 'opening' || stateName === 'closing';
}

function normalizeFaultSeverity(severity, gateState = '') {
  const value = typeof severity === 'string' ? severity.toLowerCase() : '';
  if (value === 'warning' || value === 'soft_fault' || value === 'fatal_fault') return value;
  return gateState === 'error' ? 'fatal_fault' : 'none';
}

function isFatalFault(severity) {
  return normalizeFaultSeverity(severity) === 'fatal_fault';
}

function gateIsMostlyOpen() {
  if (state.currentState === 'open' || state.currentState === 'opening') return true;
  if (state.currentState === 'closed' || state.currentState === 'closing') return false;
  return state.posPercent >= 50;
}

function updateActionCopy() {
  const blocked = isFatalFault(state.currentFaultSeverity);
  const mostlyOpen = gateIsMostlyOpen();
  const moving = isMoving(state.currentState);

  ui.toggleBtn.classList.toggle('is-on', mostlyOpen);
  ui.toggleBtn.classList.toggle('is-off', !mostlyOpen);

  if (blocked) {
    setText(ui.toggleTitle, 'Zablokowane');
    setText(ui.holdHint, 'Sterowanie zablokowane przez FATAL_FAULT');
    setText(ui.holdStateText, 'Brama nie przyjmie ruchu');
    return;
  }

  setText(ui.toggleTitle, moving ? 'STOP' : (mostlyOpen ? 'Zamknij' : 'Otworz'));
  setText(ui.holdHint, 'Przytrzymaj 1 s, aby aktywowac');
  setText(ui.holdStateText, moving ? 'Przytrzymaj 1 s, aby wyslac STOP' : 'Przytrzymaj 1 s, aby wyslac sygnal');
}

function updateLimitIndicators(limitOpen, limitClose) {
  if (ui.limitOpenValue) {
    ui.limitOpenValue.classList.toggle('is-active', limitOpen);
    ui.limitOpenValue.setAttribute('aria-label', limitOpen ? 'Krancowka otwarcia aktywna' : 'Krancowka otwarcia nieaktywna');
  }
  if (ui.limitCloseValue) {
    ui.limitCloseValue.classList.toggle('is-active', limitClose);
    ui.limitCloseValue.setAttribute('aria-label', limitClose ? 'Krancowka zamkniecia aktywna' : 'Krancowka zamkniecia nieaktywna');
  }
}

function applyToggleAvailability(severity, gateState) {
  const normalized = normalizeFaultSeverity(severity, gateState);
  state.currentFaultSeverity = normalized;
  const fatalFault = isFatalFault(normalized);
  ui.toggleBtn.disabled = fatalFault;
  ui.toggleBtn.classList.toggle('error', fatalFault);
  ui.toggleBtn.classList.toggle('moving', isMoving(gateState));
  updateActionCopy();
}

function updateCoreState(gateState, faultSeverity, percent) {
  state.currentState = gateState;
  state.posPercent = percent;
  setText(ui.stateLabel, gateState.toUpperCase());
  setText(ui.posPct, `${percent}%`);
  ui.progressFill.style.width = `${percent}%`;
  applyToggleAvailability(faultSeverity, gateState);
}

function updateUI(data) {
  if (!data) return;
  const gate = data.gate || {};
  const inputs = data.inputs || {};
  const gateState = (gate.state || 'unknown').toLowerCase();
  const faultSeverity = normalizeFaultSeverity(gate.faultSeverity, gateState);
  const percent = typeof gate.positionPercent === 'number'
    ? Math.min(100, Math.max(0, Math.round(gate.positionPercent)))
    : 0;

  updateCoreState(gateState, faultSeverity, percent);
  updateLimitIndicators(Boolean(inputs.limitOpen), Boolean(inputs.limitClose));
}

function normalizeLitePayload(data) {
  if (!data) return null;
  return {
    gate: {
      state: data.state,
      moving: data.moving,
      positionPercent: data.positionPercent,
      faultSeverity: data.faultSeverity
    },
    inputs: {
      limitOpen: data.limitOpen,
      limitClose: data.limitClose
    }
  };
}

function updateLite(data) {
  const normalized = normalizeLitePayload(data);
  if (!normalized) return;
  updateUI(normalized);
}

async function fetchJson(path, timeoutMs) {
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), timeoutMs);
  try {
    const headers = {};
    const token = getToken();
    if (token) headers['X-Api-Key'] = token;
    if (core) await core.ensurePreferredBaseUrlLoaded({ navigate: false, tokenKey });
    const res = await fetch(core ? core.resolveApiUrl(path) : path, {
      signal: ctrl.signal,
      cache: 'no-store',
      headers
    });
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
  ui.toggleBtn.classList.add('is-sending');
  ui.toggleBtn.setAttribute('aria-busy', 'true');
  setText(ui.holdStateText, 'Wysylanie...');
  try {
    if (isMoving(state.currentState)) {
      await sendControl('stop');
    } else {
      await sendControl('toggle');
    }
  } finally {
    setTimeout(() => {
      state.toggleBusy = false;
      ui.toggleBtn.classList.remove('hold-fired', 'is-sending');
      ui.toggleBtn.removeAttribute('aria-busy');
      updateActionCopy();
    }, 420);
  }
}

function vibrate(ms) {
  if (!state.touchFeedback) return;
  if (typeof navigator.vibrate === 'function') navigator.vibrate(ms);
}

function clearHoldTimer() {
  if (!state.holdTimer) return;
  clearTimeout(state.holdTimer);
  state.holdTimer = null;
}

function resetHoldState() {
  clearHoldTimer();
  state.holdTriggered = false;
  state.holdPointerId = null;
  ui.toggleBtn.classList.remove('is-holding', 'hold-fired');
  updateActionCopy();
}

function triggerHoldAction() {
  state.holdTriggered = true;
  ui.toggleBtn.classList.remove('is-holding');
  ui.toggleBtn.classList.add('hold-fired');
  setText(ui.holdStateText, 'Aktywowano');
  vibrate(45);
  performToggleAction();
}

function onPointerDown(event) {
  if (ui.toggleBtn.disabled || state.toggleBusy) return;
  event.preventDefault();
  clearHoldTimer();
  state.holdTriggered = false;
  state.holdPointerId = event.pointerId;
  ui.toggleBtn.classList.add('is-holding');
  setText(ui.holdStateText, 'Trzymaj jeszcze chwile');
  state.holdTimer = setTimeout(triggerHoldAction, 1000);
}

function onPointerEnd(event) {
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
  if (!state.cameraLoadTimer) return;
  clearTimeout(state.cameraLoadTimer);
  state.cameraLoadTimer = null;
}

function resetCameraPreviewMedia() {
  clearCameraLoadTimer();
  state.cameraPreviewMode = 'none';
  ui.cameraFrame.classList.remove('has-image', 'has-embed');
  if (ui.cameraImage.getAttribute('src')) ui.cameraImage.removeAttribute('src');
  if (ui.cameraEmbed.getAttribute('src')) ui.cameraEmbed.removeAttribute('src');
}

function activateCameraImagePreview() {
  clearCameraLoadTimer();
  state.cameraPreviewMode = 'image';
  ui.cameraFrame.classList.remove('has-embed');
  ui.cameraFrame.classList.add('has-image');
  setText(ui.cameraEmptyTitle, 'Podglad kamery aktywny');
  setText(
    ui.cameraEmptyText,
    looksLikeSnapshotUrl(state.cameraUrl)
      ? 'Snapshot jest odswiezany lokalnie w tej przegladarce.'
      : 'Obraz jest ladowany bezposrednio przez te przegladarke.'
  );
}

function activateCameraEmbedPreview() {
  clearCameraLoadTimer();
  state.cameraPreviewMode = 'iframe';
  const cameraUrl = parseCameraUrl(state.cameraUrl);
  ui.cameraFrame.classList.remove('has-image');
  ui.cameraFrame.classList.add('has-embed');
  setText(ui.cameraEmptyTitle, 'Osadzony podglad strony kamery');
  setText(ui.cameraEmptyText, 'Bezposredni snapshot nie zaladowal sie, wiec osadzono strone kamery.');
  stopCameraRefresh();
  ui.cameraEmbed.src = cameraUrl.previewUrl;
}

function showCameraPlaceholder(title, text) {
  resetCameraPreviewMedia();
  setText(ui.cameraEmptyTitle, title);
  setText(ui.cameraEmptyText, text);
}

function refreshCamera(force = false) {
  const cameraUrl = parseCameraUrl(state.cameraUrl);
  const snapshotLike = looksLikeSnapshotUrl(cameraUrl.previewUrl);

  if (!state.cameraUrl) {
    showCameraPlaceholder(
      'Brak obrazu kamery',
      'Ustaw lokalny URL snapshotu w Ustawieniach.'
    );
    return;
  }

  if (document.hidden && !force) return;

  clearCameraLoadTimer();
  ui.cameraFrame.classList.remove('has-embed');
  ui.cameraFrame.classList.add('has-image');
  state.cameraPreviewMode = 'loading-image';
  ui.cameraImage.src = buildCameraSrc(cameraUrl.previewUrl);
  state.cameraLoadTimer = setTimeout(() => {
    if (state.cameraPreviewMode !== 'loading-image' || !state.cameraUrl) return;

    if (cameraUrl.hasEmbeddedCredentials) {
      showCameraPlaceholder(
        'Wymagane logowanie przegladarki',
        'Sprawdz URL kamery w Ustawieniach albo zaloguj sie do kamery w tej przegladarce.'
      );
      return;
    }

    if (snapshotLike) {
      showCameraPlaceholder(
        'Nie udalo sie pobrac snapshotu JPEG',
        'Sprawdz URL kamery w Ustawieniach.'
      );
      return;
    }

    activateCameraEmbedPreview();
  }, snapshotLike ? 5000 : 3200);
}

function stopCameraRefresh() {
  if (!state.cameraRefreshTimer) return;
  clearInterval(state.cameraRefreshTimer);
  state.cameraRefreshTimer = null;
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

function stopStatusPolling() {
  if (!state.statusLiteTimer) return;
  clearInterval(state.statusLiteTimer);
  state.statusLiteTimer = null;
}

function startStatusPolling() {
  stopStatusPolling();
  fetchLite();
  state.statusLiteTimer = setInterval(fetchLite, litePollMs);
}

async function connectWs() {
  if (connectWs._done) return;
  connectWs._done = true;

  wsManager.subscribe({
    open() {
      state.wsConnected = true;
    },
    close() {
      state.wsConnected = false;
    },
    stale() {
      state.wsConnected = false;
      fetchLite();
    },
    message(evt) {
      try {
        const msg = JSON.parse(evt.data);
        if (msg.type === 'status' && msg.data) updateUI(msg.data);
        if (msg.type === 'status_lite' && msg.data) updateLite(msg.data);
      } catch {}
    }
  });

  wsManager.setVisibility(!document.hidden);
  wsManager.setOnline(navigator.onLine !== false);
  wsManager.connect();
}

function bindCameraUi() {
  state.cameraUrl = safeLocalStorageGet(cameraUrlKey);
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
        'Wymagane logowanie przegladarki',
        'Sprawdz URL kamery w Ustawieniach albo zaloguj sie do kamery w tej przegladarce.'
      );
      return;
    }

    if (snapshotLike) {
      showCameraPlaceholder(
        'Nie udalo sie pobrac snapshotu JPEG',
        'Ten adres nie zwrocil obrazka. Sprawdz URL kamery w Ustawieniach.'
      );
      return;
    }

    activateCameraEmbedPreview();
  });

  ui.cameraEmbed.addEventListener('load', () => {
    if (state.cameraPreviewMode !== 'iframe') return;
    setText(ui.cameraEmptyTitle, 'Osadzony podglad strony kamery');
  });
}

function bindToggleUi() {
  ui.toggleBtn.addEventListener('pointerdown', onPointerDown);
  ui.toggleBtn.addEventListener('pointerup', onPointerEnd);
  ui.toggleBtn.addEventListener('pointercancel', onPointerEnd);
  ui.toggleBtn.addEventListener('pointerleave', onPointerEnd);
  ui.toggleBtn.addEventListener('click', (event) => {
    event.preventDefault();
  });
}

function refreshInteractionMode() {
  state.touchFeedback = detectTouchFeedback();
  updateActionCopy();
}

core.bindPageLifecycle({
  onHide() {
    clearHoldTimer();
    stopCameraRefresh();
    stopStatusPolling();
    state.wsConnected = false;
    wsManager.setVisibility(false);
  },
  onShow() {
    startStatusPolling();
    startCameraRefresh();
    wsManager.setVisibility(true);
    fetchLite();
  },
  onWake() {
    wsManager.reconnect('wake');
    fetchLite();
  },
  onOnline() {
    wsManager.setOnline(true);
    fetchLite();
  },
  onOffline() {
    state.wsConnected = false;
    wsManager.setOnline(false);
  }
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
  startCameraRefresh();
  connectWs();
});
