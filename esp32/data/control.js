const qs = (id) => document.getElementById(id);
const tokenKey = 'apiToken';

const ui = {
  stateLabel: qs('stateLabel'),
  posPct: qs('posPct'),
  progressFill: qs('progressFill'),
  gatePanel: qs('gatePanel'),
  dirArrow: qs('dirArrow'),
  arrowShape: qs('arrowShape'),
  toggleBtn: qs('toggleBtn'),
  openBtn: qs('openBtn'),
  stopBtn: qs('stopBtn'),
  closeBtn: qs('closeBtn'),
  mBat: qs('mBat'),
  mCurrent: qs('mCurrent'),
  mRpm: qs('mRpm'),
  mDist: qs('mDist'),
  mArmed: qs('mArmed'),
  mFault: qs('mFault'),
  cWifi: qs('cWifi'),
  cLimO: qs('cLimO'),
  cLimC: qs('cLimC'),
  toast: qs('toast'),
};

const state = {
  liteInFlight: false,
  fullInFlight: false,
  currentState: '',
  posPercent: 0,
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

function showToast(msg, type = 'info') {
  ui.toast.textContent = msg;
  ui.toast.className = `toast show ${type}`;
  setTimeout(() => ui.toast.className = 'toast', 2400);
}

/* ---- Gate animation ---- */

function updateGateVisual(pct) {
  // pct: 0 = fully open, 100 = fully closed
  // Gate panel slides from right (open) to left (closed)
  // At 0% (open): panel is shifted completely to the right (hidden)
  // At 100% (closed): panel is in place
  const openAmount = 100 - Math.max(0, Math.min(100, pct));
  const translateX = (openAmount / 100) * 320; // 320 = gate panel width in SVG units
  ui.gatePanel.style.transform = `translateX(${translateX}px)`;
}

function updateDirectionArrow(gateState) {
  const s = (gateState || '').toLowerCase();
  if (s === 'opening') {
    ui.dirArrow.style.opacity = '1';
    ui.arrowShape.setAttribute('d', 'M190 60 L210 50 L210 70 Z'); // right arrow
    ui.arrowShape.setAttribute('fill', 'var(--accent)');
  } else if (s === 'closing') {
    ui.dirArrow.style.opacity = '1';
    ui.arrowShape.setAttribute('d', 'M210 60 L190 50 L190 70 Z'); // left arrow
    ui.arrowShape.setAttribute('fill', 'var(--warn)');
  } else {
    ui.dirArrow.style.opacity = '0';
  }
}

/* ---- State rendering ---- */

function setChip(el, text, cls) {
  if (!el) return;
  if (el.textContent !== text) el.textContent = text;
  el.classList.remove('success', 'warn', 'danger');
  if (cls) el.classList.add(cls);
}

function isMoving(s) {
  return s === 'opening' || s === 'closing';
}

function updateUI(data) {
  if (!data) return;
  const gate = data.gate || {};
  const wifi = data.wifi || {};
  const hb = data.hb || {};
  const inputs = data.inputs || {};

  const gateState = (gate.state || 'unknown').toLowerCase();
  state.currentState = gateState;

  // State label
  const label = gateState.toUpperCase();
  if (ui.stateLabel.textContent !== label) ui.stateLabel.textContent = label;
  ui.stateLabel.className = 'state-label ' + gateState;

  // Position
  const pct = typeof gate.positionPercent === 'number' && gate.positionPercent >= 0
    ? Math.round(gate.positionPercent) : 0;
  state.posPercent = pct;
  // Show "open" percentage (inverted: 0% closed = 100% open)
  const openPct = 100 - pct;
  if (ui.posPct.textContent !== `${openPct}%`) ui.posPct.textContent = `${openPct}%`;
  ui.progressFill.style.width = `${openPct}%`;
  updateGateVisual(pct);
  updateDirectionArrow(gateState);

  // Toggle button state
  ui.toggleBtn.classList.toggle('moving', isMoving(gateState));
  ui.toggleBtn.classList.toggle('error', gateState === 'error');

  // Metrics
  const telOk = hb.lastTelMs && hb.lastTelMs > 0;
  if (telOk) {
    ui.mBat.textContent = hb.batV && hb.batV > 0 ? `${hb.batV.toFixed(1)}V` : '---';
    ui.mCurrent.textContent = typeof hb.iA === 'number' && hb.iA >= 0 ? `${hb.iA.toFixed(1)}A` : '---';
    ui.mRpm.textContent = typeof hb.rpm === 'number' ? `${hb.rpm}` : '--';
    ui.mDist.textContent = typeof hb.dist_mm === 'number' && hb.dist_mm >= 0
      ? (hb.dist_mm >= 1000 ? `${(hb.dist_mm / 1000).toFixed(2)}m` : `${hb.dist_mm}mm`)
      : '--';
    ui.mArmed.textContent = hb.armed ? 'ON' : 'OFF';
    ui.mArmed.style.color = hb.armed ? 'var(--success)' : 'var(--muted)';
    if (hb.fault === 0) {
      ui.mFault.textContent = 'OK';
      ui.mFault.style.color = 'var(--success)';
    } else if (hb.fault) {
      ui.mFault.textContent = `${hb.fault}`;
      ui.mFault.style.color = 'var(--danger)';
    } else {
      ui.mFault.textContent = '--';
      ui.mFault.style.color = '';
    }
  }

  // Chips
  setChip(ui.cWifi, `WiFi: ${wifi.connected ? (wifi.ssid || 'OK') : 'OFF'}`, wifi.connected ? 'success' : 'warn');
  const limO = Boolean(inputs.limitOpen);
  const limC = Boolean(inputs.limitClose);
  setChip(ui.cLimO, `OPEN: ${limO ? 'ON' : 'OFF'}`, limO ? 'success' : '');
  setChip(ui.cLimC, `CLOSE: ${limC ? 'ON' : 'OFF'}`, limC ? 'success' : '');
}

function updateLite(data) {
  if (!data) return;
  const gateState = (data.state || '').toLowerCase();
  if (!gateState) return;
  state.currentState = gateState;

  const label = gateState.toUpperCase();
  if (ui.stateLabel.textContent !== label) ui.stateLabel.textContent = label;
  ui.stateLabel.className = 'state-label ' + gateState;

  const pct = typeof data.positionPercent === 'number' && data.positionPercent >= 0
    ? Math.round(data.positionPercent) : 0;
  state.posPercent = pct;
  const openPct = 100 - pct;
  if (ui.posPct.textContent !== `${openPct}%`) ui.posPct.textContent = `${openPct}%`;
  ui.progressFill.style.width = `${openPct}%`;
  updateGateVisual(pct);
  updateDirectionArrow(gateState);

  ui.toggleBtn.classList.toggle('moving', isMoving(gateState));
  ui.toggleBtn.classList.toggle('error', gateState === 'error');

  if (typeof data.rpm === 'number') ui.mRpm.textContent = `${data.rpm}`;
  if (typeof data.iA === 'number' && data.iA >= 0) ui.mCurrent.textContent = `${data.iA.toFixed(1)}A`;

  const limO = Boolean(data.limitOpen);
  const limC = Boolean(data.limitClose);
  setChip(ui.cLimO, `OPEN: ${limO ? 'ON' : 'OFF'}`, limO ? 'success' : '');
  setChip(ui.cLimC, `CLOSE: ${limC ? 'ON' : 'OFF'}`, limC ? 'success' : '');
}

/* ---- Networking ---- */

async function fetchJson(path, timeoutMs) {
  const ctrl = new AbortController();
  const t = setTimeout(() => ctrl.abort(), timeoutMs);
  try {
    const res = await fetch(path, { signal: ctrl.signal, cache: 'no-store' });
    if (!res.ok) return null;
    return await res.json();
  } catch { return null; }
  finally { clearTimeout(t); }
}

async function fetchLite() {
  if (document.hidden || state.liteInFlight) return;
  state.liteInFlight = true;
  try {
    const d = await fetchJson('/api/status-lite', 2000);
    if (d) updateLite(d);
  } finally { state.liteInFlight = false; }
}

async function fetchFull() {
  if (document.hidden || state.fullInFlight) return;
  state.fullInFlight = true;
  try {
    const d = await fetchJson('/api/status', 2000);
    if (d) updateUI(d);
  } finally { state.fullInFlight = false; }
}

async function sendControl(action) {
  try {
    await apiFetch('/api/control', { method: 'POST', body: JSON.stringify({ action }) });
  } catch {
    showToast('Brak dostepu do sterowania', 'error');
  }
}

function connectWs() {
  if (connectWs._active) return;
  connectWs._active = true;
  const url = `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`;
  const ws = new WebSocket(url);
  ws.onmessage = (evt) => {
    try {
      const msg = JSON.parse(evt.data);
      if (msg.type === 'status') updateUI(msg.data);
    } catch {}
  };
  ws.onclose = () => {
    connectWs._active = false;
    setTimeout(connectWs, 2000);
  };
}

/* ---- Haptic feedback ---- */

function vibrate(ms) {
  if (navigator.vibrate) navigator.vibrate(ms);
}

/* ---- Init ---- */

ui.toggleBtn.addEventListener('click', () => {
  vibrate(30);
  if (isMoving(state.currentState)) {
    sendControl('stop');
  } else {
    sendControl('toggle');
  }
});
ui.openBtn.addEventListener('click', () => { vibrate(20); sendControl('open'); });
ui.stopBtn.addEventListener('click', () => { vibrate(20); sendControl('stop'); });
ui.closeBtn.addEventListener('click', () => { vibrate(20); sendControl('close'); });

document.addEventListener('visibilitychange', () => {
  if (!document.hidden) { fetchLite(); fetchFull(); }
});

fetchFull();
fetchLite();
setInterval(fetchLite, 500);
setInterval(fetchFull, 2000);
connectWs();
