const tokenKey = 'apiToken';
const statusMsg = document.getElementById('statusMsg');
const errorMsg = document.getElementById('errorMsg');
const progressBar = document.getElementById('calibProgress');
const progressPercent = document.getElementById('calibPercent');
const stepEl = document.getElementById('calibStep');
const messageEl = document.getElementById('calibMessage');
const sigLimitOpen = document.getElementById('sigLimitOpen');
const sigLimitClose = document.getElementById('sigLimitClose');
const sigObstacle = document.getElementById('sigObstacle');
const sigHall = document.getElementById('sigHall');
const deltaBox = document.getElementById('deltaBox');
const dirConfirm = document.getElementById('dirConfirm');
const dirOkBtn = document.getElementById('dirOkBtn');
const dirInvertBtn = document.getElementById('dirInvertBtn');

let ws = null;
let lastWsMs = 0;

function getToken() {
  return localStorage.getItem(tokenKey) || '';
}

async function apiRequest(path, options = {}) {
  const headers = options.headers || {};
  const token = getToken();
  if (token) headers['X-Api-Key'] = token;
  if (options.body && !headers['Content-Type']) {
    headers['Content-Type'] = 'application/json';
  }
  const res = await fetch(path, { ...options, headers });
  const text = await res.text();
  let data = null;
  try {
    data = text ? JSON.parse(text) : null;
  } catch {
    data = null;
  }
  if (!res.ok) {
    const err = new Error(`HTTP ${res.status}`);
    err.status = res.status;
    err.data = data;
    err.text = text;
    throw err;
  }
  return data;
}

function setSignal(el, value) {
  el.textContent = value ? 'ON' : 'OFF';
}

function updateUI(data) {
  const running = Boolean(data && data.running);
  statusMsg.textContent = `Status: ${running ? 'running' : 'idle'}`;

  const progress = Number(data && data.progress ? data.progress : 0);
  progressBar.style.width = `${progress}%`;
  progressPercent.textContent = String(progress);

  stepEl.textContent = data && data.step ? data.step : 'idle';
  messageEl.textContent = data && data.message ? data.message : '-';

  const step = data && data.step ? data.step : '';
  if (step === 'confirm_dir') {
    dirConfirm.classList.remove('hidden');
  } else {
    dirConfirm.classList.add('hidden');
  }

  if (data && data.error) {
    errorMsg.textContent = `Error: ${data.error}`;
    errorMsg.classList.remove('hidden');
  } else {
    errorMsg.textContent = '';
    errorMsg.classList.add('hidden');
  }

  const live = data && data.liveSignals ? data.liveSignals : {};
  setSignal(sigLimitOpen, Boolean(live.limitOpen));
  setSignal(sigLimitClose, Boolean(live.limitClose));
  setSignal(sigObstacle, Boolean(live.obstacle));
  sigHall.textContent = live.hallRate !== undefined ? String(live.hallRate) : '-';

  const delta = data && data.proposedConfigDelta ? data.proposedConfigDelta : {};
  deltaBox.textContent = JSON.stringify(delta, null, 2);
}

async function fetchStatus() {
  try {
    const data = await apiRequest('/api/calibration/status');
    updateUI(data);
  } catch {
    statusMsg.textContent = 'Status: offline';
  }
}

function connectWs() {
  if (ws) return;
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.addEventListener('message', (event) => {
    try {
      const msg = JSON.parse(event.data);
      if (msg && msg.type === 'calibration' && msg.data) {
        lastWsMs = Date.now();
        updateUI(msg.data);
      }
    } catch {
      // ignore
    }
  });
  ws.addEventListener('close', () => {
    ws = null;
  });
}

document.getElementById('startBtn').addEventListener('click', async () => {
  try {
    await apiRequest('/api/calibration/start', { method: 'POST' });
  } catch (err) {
    const msg = err && err.data && err.data.error ? err.data.error : 'start_failed';
    errorMsg.textContent = `Error: ${msg}`;
    errorMsg.classList.remove('hidden');
  }
});

document.getElementById('stopBtn').addEventListener('click', async () => {
  try {
    await apiRequest('/api/calibration/stop', { method: 'POST' });
  } catch {
    // ignore
  }
});

dirOkBtn.addEventListener('click', async () => {
  try {
    await apiRequest('/api/calibration/confirm_dir', {
      method: 'POST',
      body: JSON.stringify({ invert: false })
    });
  } catch (err) {
    const msg = err && err.data && err.data.error ? err.data.error : 'confirm_failed';
    errorMsg.textContent = `Error: ${msg}`;
    errorMsg.classList.remove('hidden');
  }
});

dirInvertBtn.addEventListener('click', async () => {
  try {
    await apiRequest('/api/calibration/confirm_dir', {
      method: 'POST',
      body: JSON.stringify({ invert: true })
    });
  } catch (err) {
    const msg = err && err.data && err.data.error ? err.data.error : 'confirm_failed';
    errorMsg.textContent = `Error: ${msg}`;
    errorMsg.classList.remove('hidden');
  }
});

document.getElementById('applyBtn').addEventListener('click', async () => {
  try {
    await apiRequest('/api/calibration/apply', { method: 'POST' });
  } catch (err) {
    const msg = err && err.data && err.data.error ? err.data.error : 'apply_failed';
    errorMsg.textContent = `Error: ${msg}`;
    errorMsg.classList.remove('hidden');
  }
});

window.addEventListener('load', () => {
  connectWs();
  fetchStatus();
  setInterval(() => {
    const now = Date.now();
    if (now - lastWsMs > 1500) {
      fetchStatus();
    }
    if (!ws) connectWs();
  }, 400);
});
