const els = {
  limitsEnabled: document.getElementById('limitsEnabled'),
  limitOpenEnabled: document.getElementById('limitOpenEnabled'),
  limitCloseEnabled: document.getElementById('limitCloseEnabled'),
  limitOpen: document.getElementById('limitOpen'),
  limitClose: document.getElementById('limitClose'),
  limitOpenRaw: document.getElementById('limitOpenRaw'),
  limitCloseRaw: document.getElementById('limitCloseRaw'),
  stopInput: document.getElementById('stopInput'),
  obstacleInput: document.getElementById('obstacleInput'),
  buttonInput: document.getElementById('buttonInput'),
  photocellEnabled: document.getElementById('photocellEnabled'),
  photocellBlocked: document.getElementById('photocellBlocked'),
  photocellRaw: document.getElementById('photocellRaw'),
  fsTotal: document.getElementById('fsTotal'),
  fsUsed: document.getElementById('fsUsed'),
  fsFree: document.getElementById('fsFree'),
  fsUsedPct: document.getElementById('fsUsedPct')
};

const state = {
  intervalStarted: false,
  inFlight: false,
  abort: null,
};

function setText(el, value) {
  if (!el) return;
  el.textContent = value;
}

function boolLabel(value) {
  return value ? 'YES' : 'NO';
}

function boolLabelUnknown(value) {
  if (typeof value !== 'boolean') return 'NO DATA';
  return value ? 'YES' : 'NO';
}

function fmtNum(value) {
  if (typeof value !== 'number' || Number.isNaN(value)) return 'NO DATA';
  return `${value}`;
}

function fmtMb(bytes) {
  if (typeof bytes !== 'number' || Number.isNaN(bytes) || bytes < 0) return 'NO DATA';
  return (bytes / (1024 * 1024)).toFixed(2);
}

async function loadStatus() {
  if (document.hidden || state.inFlight) return;
  state.inFlight = true;
  const controller = new AbortController();
  state.abort = controller;
  const timeout = setTimeout(() => controller.abort(), 2000);
  try {
    const res = await fetch('/api/status', { signal: controller.signal, cache: 'no-store' });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const data = await res.json();

    const limits = data?.limits || {};
    const inputs = data?.inputs || {};
    const io = data?.io || {};
    setText(els.limitsEnabled, boolLabelUnknown(limits.enabled));
    setText(els.limitOpenEnabled, boolLabelUnknown(limits.openEnabled));
    setText(els.limitCloseEnabled, boolLabelUnknown(limits.closeEnabled));
    setText(els.limitOpen, boolLabelUnknown(inputs.limitOpen));
    setText(els.limitClose, boolLabelUnknown(inputs.limitClose));
    setText(els.limitOpenRaw, boolLabelUnknown(inputs.limitOpenRaw));
    setText(els.limitCloseRaw, boolLabelUnknown(inputs.limitCloseRaw));
    setText(els.stopInput, boolLabelUnknown(io.stop));
    setText(els.obstacleInput, boolLabelUnknown(io.obstacle));
    setText(els.buttonInput, boolLabelUnknown(io.button));
    setText(els.photocellEnabled, boolLabelUnknown(inputs.photocellEnabled));
    setText(els.photocellBlocked, boolLabelUnknown(inputs.photocellBlocked));
    setText(els.photocellRaw, boolLabelUnknown(inputs.photocellRaw));

    const fs = data?.fs || {};
    const total = typeof fs.totalBytes === 'number' ? fs.totalBytes : -1;
    const used = typeof fs.usedBytes === 'number' ? fs.usedBytes : -1;
    const free = total >= 0 && used >= 0 ? Math.max(0, total - used) : -1;
    const usedPct = total > 0 && used >= 0 ? ((used / total) * 100).toFixed(1) : 'NO DATA';
    setText(els.fsTotal, fmtMb(total));
    setText(els.fsUsed, fmtMb(used));
    setText(els.fsFree, fmtMb(free));
    setText(els.fsUsedPct, usedPct === 'NO DATA' ? usedPct : `${usedPct}%`);
  } catch {
    setText(els.limitsEnabled, 'NO DATA');
    setText(els.limitOpenEnabled, 'NO DATA');
    setText(els.limitCloseEnabled, 'NO DATA');
    setText(els.limitOpen, 'NO DATA');
    setText(els.limitClose, 'NO DATA');
    setText(els.limitOpenRaw, 'NO DATA');
    setText(els.limitCloseRaw, 'NO DATA');
    setText(els.stopInput, 'NO DATA');
    setText(els.obstacleInput, 'NO DATA');
    setText(els.buttonInput, 'NO DATA');
    setText(els.photocellEnabled, 'NO DATA');
    setText(els.photocellBlocked, 'NO DATA');
    setText(els.photocellRaw, 'NO DATA');
    setText(els.fsTotal, 'NO DATA');
    setText(els.fsUsed, 'NO DATA');
    setText(els.fsFree, 'NO DATA');
    setText(els.fsUsedPct, 'NO DATA');
  } finally {
    clearTimeout(timeout);
    if (state.abort === controller) state.abort = null;
    state.inFlight = false;
  }
}

function startPollingOnce() {
  if (state.intervalStarted) return;
  state.intervalStarted = true;
  setInterval(loadStatus, 5000);
}

document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    if (state.abort) state.abort.abort();
    return;
  }
  loadStatus();
});

window.addEventListener('load', () => {
  loadStatus();
  startPollingOnce();
});
