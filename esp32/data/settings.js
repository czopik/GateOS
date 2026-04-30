const tokenKey = 'apiToken';
const inputs = Array.from(document.querySelectorAll('[data-path]'));
const dirtyBar = document.getElementById('dirtyBar');
const toast = document.getElementById('toast');
const confirmModal = document.getElementById('confirmModal');
const confirmText = document.getElementById('confirmText');
const confirmYes = document.getElementById('confirmYes');
const confirmNo = document.getElementById('confirmNo');
const ledTestBtn = document.getElementById('ledTestBtn');
const ledStealthBtn = document.getElementById('ledStealthBtn');
const ledSegmentsInput = document.getElementById('ledSegments');
const gateZeroBtn = document.getElementById('gateZeroBtn');
const gateMaxBtn = document.getElementById('gateMaxBtn');
const motionAdvancedPanel = document.getElementById('motionAdvancedPanel');
const motionExpertCheckbox = document.querySelector('[data-path="motion.expert"]');
const motionDerived = document.getElementById('motionDerived');
const motionOpenBtn = document.getElementById('motionOpenBtn');
const motionStopBtn = document.getElementById('motionStopBtn');
const motionCloseBtn = document.getElementById('motionCloseBtn');
const statusEls = {
  limitOpenStatus: document.getElementById('limitOpenStatus'),
  limitCloseStatus: document.getElementById('limitCloseStatus'),
  photocellStatus: document.getElementById('photocellStatus'),
  otaBanner: document.getElementById('otaEnabledBanner'),
  otaStatus: document.getElementById('otaStatus'),
  otaProgress: document.getElementById('otaProgress'),
  otaFwVersion: document.getElementById('otaFwVersion'),
  otaUptime: document.getElementById('otaUptime'),
  otaIp: document.getElementById('otaIp'),
  otaFreeSketch: document.getElementById('otaFreeSketch'),
  otaErrorLine: document.getElementById('otaErrorLine')
};

let originalConfig = null;
let dirty = false;
let pendingConfirm = null;
let ledStealth = false;
let statusIntervalStarted = false;
let statusInFlight = false;
let statusAbort = null;

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
  const method = (options.method || 'GET').toUpperCase();
  const body = options.body || null;
  console.debug('[api request]', method, path, body);
  const res = await fetch(path, { ...options, headers });
  const text = await res.text();
  let data = null;
  try {
    data = text ? JSON.parse(text) : null;
  } catch {
    data = null;
  }
  console.debug('[api response]', method, path, res.status, data || text);
  if (!res.ok) {
    const err = new Error(`HTTP ${res.status}`);
    err.status = res.status;
    err.data = data;
    err.text = text;
    throw err;
  }
  return { res, data, text };
}

function normalizePayload(obj) {
  const type = typeof obj;
  console.debug('[cfg typeof]', type);
  if (type === 'string') {
    const parsed = JSON.parse(obj);
    if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
      throw new Error('invalid_payload_type');
    }
    return parsed;
  }
  if (obj === null || type !== 'object' || Array.isArray(obj)) {
    throw new Error('invalid_payload_type');
  }
  return obj;
}

async function postJson(path, obj) {
  const payload = normalizePayload(obj);
  console.debug('[postJson payload]', path, payload);
  return apiRequest(path, {
    method: 'POST',
    body: JSON.stringify(payload)
  });
}

function showToast(message) {
  toast.textContent = message;
  toast.className = 'toast show';
  setTimeout(() => toast.className = 'toast', 2400);
}

function redirectToPort(newPort) {
  const port = Number(newPort);
  if (!Number.isFinite(port) || port < 1 || port > 65535) return;
  const protocol = window.location.protocol || 'http:';
  const hostname = window.location.hostname || window.location.host;
  const defaultPort = protocol === 'https:' ? 443 : 80;
  const suffix = port === defaultPort ? '' : `:${port}`;
  window.location.href = `${protocol}//${hostname}${suffix}/settings.html`;
}

function setLedStealthState(value) {
  ledStealth = value;
  if (!ledStealthBtn) return;
  ledStealthBtn.textContent = ledStealth ? 'Wylacz stealth' : 'Stealth mode';
}

function setDirty(value) {
  dirty = value;
  dirtyBar.classList.toggle('hidden', !dirty);
}

function toggleMotionAdvanced() {
  if (!motionAdvancedPanel || !motionExpertCheckbox) return;
  const expert = motionExpertCheckbox.checked;
  motionAdvancedPanel.classList.toggle('hidden', !expert);
}

function displayMotionDerived(cfg) {
  if (!motionDerived) return;
  const adv = cfg?.motion?.advanced;
  if (!adv) {
    motionDerived.textContent = '';
    return;
  }
  const mode = adv.braking?.mode || 'active';
  const force = adv.braking?.force ?? 0;
  motionDerived.textContent = `Configured: open ${adv.maxSpeedOpen}, close ${adv.maxSpeedClose}, braking ${mode} @ ${force}%`;
}

function setStatusText(el, value) {
  if (!el) return;
  el.textContent = value;
}

function fmtUptime(ms) {
  if (typeof ms !== 'number' || ms < 0) return '-';
  const sec = Math.floor(ms / 1000);
  const h = Math.floor(sec / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = sec % 60;
  return `${h}h ${m}m ${s}s`;
}

function setInputValue(path, value) {
  const input = inputs.find(i => i.dataset.path === path);
  if (!input) return;
  if (input.type === 'checkbox') {
    input.checked = Boolean(value);
  } else if (input.type === 'number') {
    input.value = value ?? '';
  } else {
    input.value = value ?? '';
  }
}

function updateSensorStatus(data) {
  const inputs = data?.inputs || {};
  const limitOpenRaw = Boolean(inputs.limitOpenRaw);
  const limitCloseRaw = Boolean(inputs.limitCloseRaw);
  const limitOpen = Boolean(inputs.limitOpen);
  const limitClose = Boolean(inputs.limitClose);
  const limitsEnabled = Boolean(inputs.limitsEnabled);
  const limitOpenEnabled = Boolean(inputs.limitOpenEnabled);
  const limitCloseEnabled = Boolean(inputs.limitCloseEnabled);
  const photocellBlocked = Boolean(inputs.photocellBlocked);
  const photocellRaw = Boolean(inputs.photocellRaw);
  const photocellEnabled = Boolean(inputs.photocellEnabled);

  if (!limitsEnabled || !limitOpenEnabled) {
    setStatusText(statusEls.limitOpenStatus, 'inactive');
  } else {
    setStatusText(statusEls.limitOpenStatus, limitOpen ? 'ACTIVE' : (limitOpenRaw ? 'RAW' : 'inactive'));
  }
  if (!limitsEnabled || !limitCloseEnabled) {
    setStatusText(statusEls.limitCloseStatus, 'inactive');
  } else {
    setStatusText(statusEls.limitCloseStatus, limitClose ? 'ACTIVE' : (limitCloseRaw ? 'RAW' : 'inactive'));
  }
  if (!photocellEnabled) {
    setStatusText(statusEls.photocellStatus, 'inactive');
  } else {
    setStatusText(statusEls.photocellStatus, photocellBlocked ? 'BLOCKED' : (photocellRaw ? 'RAW' : 'CLEAR'));
  }

  const ota = data?.ota || {};
  const otaEnabled = Boolean(ota.enabled);
  const otaActive = Boolean(ota.active);
  const otaReady = Boolean(ota.ready);
  if (statusEls.otaBanner) {
    statusEls.otaBanner.style.display = otaEnabled ? 'block' : 'none';
  }
  let otaState = 'OFF';
  if (otaEnabled) otaState = otaReady ? 'ON' : 'INIT';
  if (otaActive) otaState = 'UPDATING';
  setStatusText(statusEls.otaStatus, otaState);
  const progress = typeof ota.progress === 'number' && ota.progress >= 0 ? `${ota.progress}%` : '-';
  setStatusText(statusEls.otaProgress, progress);
  const fw = data?.build || data?.version || data?.firmware || '-';
  setStatusText(statusEls.otaFwVersion, fw);
  setStatusText(statusEls.otaUptime, fmtUptime(data?.uptimeMs));
  const ip = data?.wifi?.ip || '';
  setStatusText(statusEls.otaIp, ip || '-');
  const freeSketch = typeof ota.freeSketchSpace === 'number' ? `${ota.freeSketchSpace}` : '-';
  setStatusText(statusEls.otaFreeSketch, freeSketch);
  if (statusEls.otaErrorLine) {
    const err = ota?.error || '';
    statusEls.otaErrorLine.textContent = err ? `OTA error: ${err}` : '';
  }
}

async function loadStatus() {
  if (document.hidden || statusInFlight) return;
  statusInFlight = true;
  const controller = new AbortController();
  statusAbort = controller;
  const timeout = setTimeout(() => controller.abort(), 2000);
  try {
    const result = await apiRequest('/api/status', { signal: controller.signal, cache: 'no-store' });
    const data = result.data || (result.text ? JSON.parse(result.text) : {});
    updateSensorStatus(data);
  } catch {
    updateSensorStatus({});
  } finally {
    clearTimeout(timeout);
    if (statusAbort === controller) statusAbort = null;
    statusInFlight = false;
  }
}

function startStatusPollingOnce() {
  if (statusIntervalStarted) return;
  statusIntervalStarted = true;
  setInterval(loadStatus, 5000);
}

document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    if (statusAbort) statusAbort.abort();
    return;
  }
  loadStatus();
});

async function runMotionTest(action) {
  try {
    await postJson('/api/motion/test', { action });
    showToast(`Test ${action} wydany`);
  } catch (err) {
    if (err && err.status === 401) {
      showToast('Brak uprawnien');
      return;
    }
    const backend = err && err.data && err.data.error ? err.data.error : '';
    showToast(backend ? `Test motion fail: ${backend}` : 'Test motion fail');
  }
}

function formatSegments(segments) {
  if (!Array.isArray(segments) || !segments.length) return '';
  return segments.map(seg => `${seg.start}:${seg.len}`).join(',');
}

function parseSegments(value) {
  const out = [];
  const text = (value || '').trim();
  if (!text) return out;
  const parts = text.split(',');
  for (const part of parts) {
    const trimmed = part.trim();
    if (!trimmed) continue;
    const match = trimmed.split(':');
    if (match.length !== 2) return null;
    const start = parseInt(match[0], 10);
    const len = parseInt(match[1], 10);
    if (Number.isNaN(start) || Number.isNaN(len)) return null;
    out.push({ start, len });
  }
  return out;
}

function getByPath(obj, path) {
  return path.split('.').reduce((acc, key) => (acc ? acc[key] : undefined), obj);
}

function setByPath(obj, path, value) {
  const parts = path.split('.');
  let cur = obj;
  for (let i = 0; i < parts.length - 1; i++) {
    if (!cur[parts[i]]) cur[parts[i]] = {};
    cur = cur[parts[i]];
  }
  cur[parts[parts.length - 1]] = value;
}

function readValue(input) {
  if (input.type === 'checkbox') return input.checked;
  const castType = input.dataset.type;
  if (input.value === '') return null;
  if (castType === 'int') {
    const n = parseInt(input.value, 10);
    return Number.isNaN(n) ? null : n;
  }
  if (castType === 'float') {
    const n = parseFloat(input.value);
    return Number.isNaN(n) ? null : n;
  }
  return input.value;
}

function bindConfig(cfg) {
  inputs.forEach(input => {
    let val = getByPath(cfg, input.dataset.path);
    if ((val === undefined || val === null) && input.dataset.path === 'gate.maxDistance') {
      val = cfg?.gate?.totalDistance;
    }
    if (input.type === 'checkbox') {
      input.checked = Boolean(val);
    } else if (input.type === 'number') {
      input.value = val ?? '';
    } else {
      input.value = val ?? '';
    }
  });
  if (ledSegmentsInput) {
    ledSegmentsInput.value = formatSegments(cfg?.led?.segments);
  }
  if (motionExpertCheckbox && motionAdvancedPanel) {
    motionAdvancedPanel.classList.toggle('hidden', !motionExpertCheckbox.checked);
  }
  displayMotionDerived(cfg);
  const tokenInput = document.getElementById('apiToken');
  if (tokenInput && tokenInput.value) {
    localStorage.setItem(tokenKey, tokenInput.value);
  }
}

function clone(obj) {
  return JSON.parse(JSON.stringify(obj));
}

function collectConfig() {
  const cfg = clone(originalConfig || {});
  inputs.forEach(input => {
    const val = readValue(input);
    if (val === null) return;
    setByPath(cfg, input.dataset.path, val);
  });
  if (ledSegmentsInput) {
    const parsed = parseSegments(ledSegmentsInput.value);
    if (parsed && parsed.length) {
      if (!cfg.led) cfg.led = {};
      cfg.led.segments = parsed;
    } else if (parsed && cfg.led && cfg.led.segments) {
      delete cfg.led.segments;
    }
  }
  return cfg;
}

function setError(path, msg) {
  const field = inputs.find(i => i.dataset.path === path);
  if (!field) return;
  const wrapper = field.closest('.field');
  const msgEl = wrapper ? wrapper.querySelector('.error-msg') : null;
  if (wrapper) wrapper.classList.toggle('error', Boolean(msg));
  if (msgEl) msgEl.textContent = msg || '';
}

function clearErrors() {
  inputs.forEach(input => {
    const wrapper = input.closest('.field');
    const msgEl = wrapper ? wrapper.querySelector('.error-msg') : null;
    if (wrapper) wrapper.classList.remove('error');
    if (msgEl) msgEl.textContent = '';
  });
}

function validateLocal(cfg) {
  clearErrors();
  let ok = true;
  if (!cfg.wifi || !cfg.wifi.ssid) {
    setError('wifi.ssid', 'SSID nie moze byc puste');
    ok = false;
  }
  const pwmMin = cfg.motor?.pwmMin ?? 0;
  const pwmMax = cfg.motor?.pwmMax ?? 0;
  if (pwmMin < 0 || pwmMin > 255 || pwmMax < pwmMin || pwmMax > 255) {
    setError('motor.pwmMin', 'Zakres PWM jest nieprawidlowy');
    ok = false;
  }
  const maxDistance = cfg.gate?.maxDistance ?? cfg.gate?.totalDistance ?? 0;
  if (maxDistance <= 0 || maxDistance > 100) {
    setError('gate.maxDistance', 'Max dystans 0-100 m');
    ok = false;
  }
  const position = cfg.gate?.position;
  if (typeof position === 'number' && maxDistance > 0) {
    if (position < 0 || position > maxDistance) {
      showToast('Gate position poza zakresem maxDistance');
      ok = false;
    }
  }
  const startupHomingTimeoutMs = cfg.gate?.startupHomingTimeoutMs;
  if (startupHomingTimeoutMs !== undefined && (startupHomingTimeoutMs < 5000 || startupHomingTimeoutMs > 300000)) {
    setError('gate.startupHomingTimeoutMs', 'Timeout homingu 5000-300000 ms');
    ok = false;
  }
  const homingScalePercent = cfg.gate?.homingScalePercent;
  if (homingScalePercent !== undefined && (homingScalePercent < 5 || homingScalePercent > 100)) {
    setError('gate.homingScalePercent', 'Skala homingu 5-100%');
    ok = false;
  }
  const qos = cfg.mqtt?.qos ?? 0;
  if (qos < 0 || qos > 2) {
    setError('mqtt.qos', 'QoS musi byc 0-2');
    ok = false;
  }
  const brightness = cfg.led?.brightness ?? 0;
  if (brightness < 0 || brightness > 100) {
    setError('led.brightness', 'Jasnosc 0-100');
    ok = false;
  }
  const ledCount = cfg.led?.count ?? 0;
  if (ledCount < 0 || ledCount > 300) {
    setError('led.count', 'Liczba diod 0-300');
    ok = false;
  }
  const animSpeed = cfg.led?.animSpeed ?? 50;
  if (animSpeed < 1 || animSpeed > 100) {
    setError('led.animSpeed', 'Animacja 1-100');
    ok = false;
  }
  const nightBrightness = cfg.led?.nightMode?.brightness ?? 0;
  if (nightBrightness < 0 || nightBrightness > 100) {
    setError('led.nightMode.brightness', 'Jasnosc nocna 0-100');
    ok = false;
  }
  const webPort = cfg.device?.webPort ?? 80;
  if (webPort < 1 || webPort > 65535) {
    setError('device.webPort', 'Port WWW 1-65535');
    ok = false;
  }
  const segParsed = ledSegmentsInput ? parseSegments(ledSegmentsInput.value) : [];
  if (segParsed === null) {
    showToast('Segmenty LED: format start:len, start:len');
    ok = false;
  } else if (segParsed.length > 8) {
    showToast('Segmenty LED: max 8');
    ok = false;
  } else {
    for (const seg of segParsed) {
      if (seg.start < 0 || seg.len < 0) {
        showToast('Segmenty LED: start/len >= 0');
        ok = false;
        break;
      }
    }
  }

  const hover = cfg.hoverUart || {};
  if (hover.baud !== undefined && (hover.baud < 1200 || hover.baud > 1000000)) {
    showToast('Hover UART: baud 1200-1000000');
    ok = false;
  }
  if (hover.maxSpeed !== undefined && (hover.maxSpeed < 0 || hover.maxSpeed > 2000)) {
    showToast('Hover UART: maxSpeed 0-2000');
    ok = false;
  }
  if (hover.rampStep !== undefined && (hover.rampStep < 1 || hover.rampStep > 200)) {
    showToast('Hover UART: rampStep 1-200');
    ok = false;
  }

  const strapping = [0, 2, 4, 5, 12, 15];
  const pins = {
    pwm: cfg.gpio?.pwm,
    dir: cfg.gpio?.dir,
    en: cfg.gpio?.en,
    limitOpen: cfg.gpio?.limitOpen,
    limitClose: cfg.gpio?.limitClose,
    button: cfg.gpio?.button,
    stop: cfg.gpio?.stop,
    obstacle: cfg.gpio?.obstacle,
    hcs: cfg.gpio?.hcs,
    led: cfg.led?.pin ?? cfg.gpio?.led,
    hoverRx: cfg.hoverUart?.rx,
    hoverTx: cfg.hoverUart?.tx
  };
  const used = {};
  const conflicts = [];
  Object.entries(pins).forEach(([key, value]) => {
    if (value === null || value === undefined) return;
    if (value < 0) return;
    if (strapping.includes(value)) {
      showToast(`Uwaga: GPIO ${value} to pin strapping (${key})`);
    }
    if (used[value]) {
      conflicts.push(`${used[value]} i ${key} na GPIO ${value}`);
    } else {
      used[value] = key;
    }
  });
  if (conflicts.length) {
    showToast(`Konflikt GPIO: ${conflicts[0]}`);
  }

  return ok;
}

async function loadConfig() {
  try {
    const result = await apiRequest('/api/config');
    const cfg = result.data || (result.text ? JSON.parse(result.text) : {});
    originalConfig = cfg;
    bindConfig(cfg);
    setLedStealthState(cfg && cfg.led && (cfg.led.mode === 'stealth' || cfg.led.defaultMode === 'stealth'));
    setDirty(false);
  } catch (err) {
    if (err && err.status === 401) {
      showToast('Brak uprawnien. Podaj token API.');
      return;
    }
    showToast('Nie mozna pobrac konfiguracji');
  }
}

async function saveConfig() {
  const cfg = collectConfig();
  console.debug('[save] typeof cfg', typeof cfg);
  if (!validateLocal(cfg)) return;
  try {
    await postJson('/api/config/validate', cfg);
  } catch (err) {
    if (err && (err.message === 'invalid_payload_type' || err.name === 'SyntaxError')) {
      showToast('Validation error: payload');
      return;
    }
    if (err && err.status === 401) {
      showToast('Brak uprawnien');
      return;
    }
    if (err && err.data && err.data.status === 'invalid') {
      const details = Array.isArray(err.data.details) ? err.data.details.join(', ') : '';
      showToast(details ? `Validation error: ${details}` : 'Validation error');
      return;
    }
    const backend = err && err.data && err.data.error ? err.data.error : '';
    showToast(backend ? `Blad zapisu: ${backend}` : 'Blad zapisu');
    return;
  }

  try {
    const result = await postJson('/api/config', cfg);
    if (result && result.apply === 'restart' && result.redirectPort) {
      showToast(`Zapisano, restart i zmiana na port ${result.redirectPort}...`);
      setTimeout(() => redirectToPort(result.redirectPort), 2500);
      return;
    }
    showToast('Zapisano');
    await loadConfig();
  } catch (err) {
    if (err && (err.message === 'invalid_payload_type' || err.name === 'SyntaxError')) {
      showToast('Validation error: payload');
      return;
    }
    if (err && err.status === 401) {
      showToast('Brak uprawnien');
      return;
    }
    if (err && err.data && err.data.status === 'invalid') {
      const details = Array.isArray(err.data.details) ? err.data.details.join(', ') : '';
      showToast(details ? `Validation error: ${details}` : 'Validation error');
      return;
    }
    const backend = err && err.data && err.data.error ? err.data.error : '';
    showToast(backend ? `Blad zapisu: ${backend}` : 'Blad zapisu');
  }
}

function openConfirm(message, onConfirm) {
  confirmText.textContent = message;
  confirmModal.classList.add('open');
  confirmModal.setAttribute('aria-hidden', 'false');
  pendingConfirm = onConfirm;
}

function closeConfirm() {
  confirmModal.classList.remove('open');
  confirmModal.setAttribute('aria-hidden', 'true');
  pendingConfirm = null;
}

async function exportConfig() {
  try {
    const result = await apiRequest('/api/config');
    const text = result.text || JSON.stringify(result.data || {});
    const blob = new Blob([text], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'config.json';
    a.click();
    URL.revokeObjectURL(url);
  } catch {
    showToast('Eksport nieudany');
  }
}

async function importConfig(file) {
  try {
    const text = await file.text();
    const cfg = JSON.parse(text);
    await postJson('/api/config/validate', cfg);
    await postJson('/api/config', cfg);
    await loadConfig();
    showToast('Import OK');
  } catch {
    showToast('Import nieudany');
  }
}

function setupAccordion() {
  document.querySelectorAll('.accordion-header').forEach(header => {
    header.addEventListener('click', () => {
      const item = header.closest('.accordion-item');
      item.classList.toggle('open');
    });
  });
}

function setupListeners() {
  inputs.forEach(input => {
    input.addEventListener('input', () => setDirty(true));
    input.addEventListener('change', () => setDirty(true));
  });
  if (ledSegmentsInput) {
    ledSegmentsInput.addEventListener('input', () => setDirty(true));
    ledSegmentsInput.addEventListener('change', () => setDirty(true));
  }

  if (motionExpertCheckbox) {
    motionExpertCheckbox.addEventListener('change', () => {
      toggleMotionAdvanced();
      setDirty(true);
    });
  }

  if (motionOpenBtn) {
    motionOpenBtn.addEventListener('click', () => runMotionTest('open'));
  }
  if (motionStopBtn) {
    motionStopBtn.addEventListener('click', () => runMotionTest('stop'));
  }
  if (motionCloseBtn) {
    motionCloseBtn.addEventListener('click', () => runMotionTest('close'));
  }

  const apiTokenInput = document.getElementById('apiToken');
  if (apiTokenInput) {
    apiTokenInput.addEventListener('input', () => {
      localStorage.setItem(tokenKey, apiTokenInput.value);
    });
  }

  document.getElementById('saveBtn').addEventListener('click', saveConfig);
  document.getElementById('discardBtn').addEventListener('click', () => {
    bindConfig(originalConfig || {});
    setDirty(false);
  });

  document.getElementById('mqttTestBtn').addEventListener('click', async () => {
    try {
      const result = await apiRequest('/api/mqtt/test', { method: 'POST' });
      const data = result.data || {};
      if (data.ok) {
        const topic = data.topic || '';
        showToast(`MQTT OK -> ${topic}`);
        return;
      }
      const err = data.error || (data.state !== undefined ? `state ${data.state}` : 'unknown');
      showToast(`MQTT FAIL: ${err}`);
    } catch (err) {
      if (err && err.status === 401) {
        showToast('Brak uprawnien');
        return;
      }
      const backend = err && err.data && err.data.error ? err.data.error : '';
      showToast(backend ? `MQTT FAIL: ${backend}` : 'MQTT FAIL');
    }
  });

  if (ledTestBtn) {
    ledTestBtn.addEventListener('click', async () => {
      try {
        await apiRequest('/api/led/test', { method: 'POST' });
        showToast('Test LED uruchomiony');
      } catch (err) {
        if (err && err.status === 401) {
          showToast('Brak uprawnien');
          return;
        }
        showToast('Test LED nieudany');
      }
    });
  }

  if (ledStealthBtn) {
    ledStealthBtn.addEventListener('click', async () => {
      const next = !ledStealth;
      try {
        await postJson('/api/led', { mode: next ? 'stealth' : 'status' });
        setLedStealthState(next);
        showToast(next ? 'Stealth ON' : 'Stealth OFF');
      } catch (err) {
        if (err && err.status === 401) {
          showToast('Brak uprawnien');
          return;
        }
        const backend = err && err.data && err.data.error ? err.data.error : '';
        showToast(backend ? `Blad LED: ${backend}` : 'Blad LED');
      }
    });
  }

  if (gateZeroBtn) {
    gateZeroBtn.addEventListener('click', async () => {
      try {
        await postJson('/api/gate/calibrate', { set: 'zero' });
        showToast('Pozycja ustawiona na 0');
        await loadConfig();
      } catch (err) {
        if (err && err.status === 401) {
          showToast('Brak uprawnien');
          return;
        }
        const backend = err && err.data && err.data.error ? err.data.error : '';
        showToast(backend ? `Blad kalibracji: ${backend}` : 'Blad kalibracji');
      }
    });
  }

  if (gateMaxBtn) {
    gateMaxBtn.addEventListener('click', async () => {
      try {
        await postJson('/api/gate/calibrate', { set: 'max' });
        showToast('Pozycja ustawiona na MAX');
        await loadConfig();
      } catch (err) {
        if (err && err.status === 401) {
          showToast('Brak uprawnien');
          return;
        }
        const backend = err && err.data && err.data.error ? err.data.error : '';
        showToast(backend ? `Blad kalibracji: ${backend}` : 'Blad kalibracji');
      }
    });
  }

  document.getElementById('exportBtn').addEventListener('click', exportConfig);
  document.getElementById('importBtn').addEventListener('click', () => {
    document.getElementById('importFile').click();
  });
  document.getElementById('importFile').addEventListener('change', (e) => {
    const file = e.target.files[0];
    if (file) importConfig(file);
    e.target.value = '';
  });

  document.getElementById('rebootBtn').addEventListener('click', () => {
    openConfirm('Restartowac urzadzenie?', async () => {
      await apiRequest('/api/reboot', { method: 'POST' });
      showToast('Restart...');
    });
  });
  document.getElementById('factoryBtn').addEventListener('click', () => {
    openConfirm('Factory reset? Operacja nieodwracalna.', async () => {
      await apiRequest('/api/factory_reset', { method: 'POST' });
      showToast('Reset OK');
    });
  });

  // --- HTTP OTA upload przez przeglądarkę ---
  const otaUploadBtn  = document.getElementById('otaUploadBtn');
  const otaUploadFile = document.getElementById('otaUploadFile');
  const otaUploadFileName = document.getElementById('otaUploadFileName');
  const otaUploadProgress = document.getElementById('otaUploadProgress');
  const otaUploadBar    = document.getElementById('otaUploadBar');
  const otaUploadStatus = document.getElementById('otaUploadStatus');

  otaUploadBtn.addEventListener('click', () => otaUploadFile.click());

  otaUploadFile.addEventListener('change', (e) => {
    const file = e.target.files[0];
    if (!file) return;
    otaUploadFileName.textContent = file.name;
    openConfirm(`Wgrać firmware: ${file.name} (${(file.size/1024).toFixed(1)} KB)?`, () => {
      uploadFirmware(file);
    });
    e.target.value = '';
  });

  async function uploadFirmware(file) {
    otaUploadProgress.style.display = 'block';
    otaUploadBar.style.width = '0%';
    otaUploadStatus.textContent = 'Przesyłanie...';
    otaUploadBtn.disabled = true;

    const formData = new FormData();
    formData.append('firmware', file, file.name);

    const xhr = new XMLHttpRequest();
    const token = getToken();

    xhr.upload.addEventListener('progress', (ev) => {
      if (ev.lengthComputable) {
        const pct = Math.round(ev.loaded / ev.total * 100);
        otaUploadBar.style.width = pct + '%';
        otaUploadStatus.textContent = `Przesyłanie: ${pct}% (${(ev.loaded/1024).toFixed(0)} / ${(ev.total/1024).toFixed(0)} KB)`;
      }
    });

    xhr.addEventListener('load', () => {
      let ok = false;
      try { ok = JSON.parse(xhr.responseText)?.ok === true; } catch {}
      if (ok) {
        otaUploadBar.style.width = '100%';
        otaUploadBar.style.background = '#4caf50';
        otaUploadStatus.textContent = 'Wgrano pomyślnie! Urządzenie restartuje się...';
        showToast('Firmware wgrane — restart urządzenia');
      } else {
        otaUploadBar.style.background = '#f44336';
        let errMsg = '-';
        try { errMsg = JSON.parse(xhr.responseText)?.error || xhr.responseText; } catch {}
        otaUploadStatus.textContent = `Błąd: ${errMsg}`;
        showToast('Błąd wgrywania firmware');
        otaUploadBtn.disabled = false;
      }
    });

    xhr.addEventListener('error', () => {
      otaUploadBar.style.background = '#f44336';
      otaUploadStatus.textContent = 'Błąd sieci — sprawdź połączenie';
      showToast('Błąd sieci');
      otaUploadBtn.disabled = false;
    });

    xhr.open('POST', '/api/ota/upload');
    if (token) xhr.setRequestHeader('X-Api-Key', token);
    xhr.send(formData);
  }

  confirmYes.addEventListener('click', async () => {
    if (pendingConfirm) await pendingConfirm();
    closeConfirm();
  });
  confirmNo.addEventListener('click', closeConfirm);
}

window.addEventListener('load', () => {
  setupAccordion();
  setupListeners();
  loadConfig();
  loadStatus();
  startStatusPollingOnce();
});
