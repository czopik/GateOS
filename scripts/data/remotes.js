const tokenKey = 'apiToken';
const ui = {
  learnToggle: document.getElementById('learnToggle'),
  learnStatus: document.getElementById('learnStatus'),
  listenBtn: document.getElementById('listenBtn'),
  remotesTable: document.getElementById('remotesTable'),
  lastSerial: document.getElementById('lastSerial'),
  lastName: document.getElementById('lastName'),
  lastCounter: document.getElementById('lastCounter'),
  lastAuth: document.getElementById('lastAuth'),
  toast: document.getElementById('toast'),
  addRemoteBtn: document.getElementById('addRemoteBtn'),
  exportRemotesBtn: document.getElementById('exportRemotesBtn'),
  importRemotesBtn: document.getElementById('importRemotesBtn'),
  importRemotesFile: document.getElementById('importRemotesFile'),
  remoteModal: document.getElementById('remoteModal'),
  remoteModalTitle: document.getElementById('remoteModalTitle'),
  remoteSerialInput: document.getElementById('remoteSerialInput'),
  remoteNameInput: document.getElementById('remoteNameInput'),
  remoteEnabledInput: document.getElementById('remoteEnabledInput'),
  remoteSaveBtn: document.getElementById('remoteSaveBtn'),
  remoteCancelBtn: document.getElementById('remoteCancelBtn'),
  confirmModal: document.getElementById('confirmModal'),
  confirmText: document.getElementById('confirmText'),
  confirmYes: document.getElementById('confirmYes'),
  confirmNo: document.getElementById('confirmNo'),
};

const state = {
  remotes: [],
  learnMode: false,
  editMode: 'add',
  editSerial: null,
  pendingConfirm: null,
  listenUntil: 0,
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
  if (!res.ok) {
    const text = await res.text();
    let data = null;
    try { data = text ? JSON.parse(text) : null; } catch { data = null; }
    const err = new Error(`HTTP ${res.status}`);
    err.status = res.status;
    err.data = data;
    err.text = text;
    throw err;
  }
  return res;
}

function showToast(message) {
  ui.toast.textContent = message;
  ui.toast.className = 'toast show';
  setTimeout(() => ui.toast.className = 'toast', 2400);
}

function openModal(mode, remote) {
  state.editMode = mode;
  state.editSerial = remote ? remote.serial : null;
  ui.remoteModalTitle.textContent = mode === 'add' ? 'Dodaj pilota' : 'Edytuj pilota';
  ui.remoteSerialInput.value = remote ? remote.serial : '';
  ui.remoteSerialInput.disabled = mode !== 'add';
  ui.remoteNameInput.value = remote ? (remote.name || '') : '';
  ui.remoteEnabledInput.checked = remote ? remote.enabled : true;
  ui.remoteModal.classList.add('open');
  ui.remoteModal.setAttribute('aria-hidden', 'false');
}

function closeModal() {
  ui.remoteModal.classList.remove('open');
  ui.remoteModal.setAttribute('aria-hidden', 'true');
}

function openConfirm(message, onConfirm) {
  ui.confirmText.textContent = message;
  ui.confirmModal.classList.add('open');
  ui.confirmModal.setAttribute('aria-hidden', 'false');
  state.pendingConfirm = onConfirm;
}

function closeConfirm() {
  ui.confirmModal.classList.remove('open');
  ui.confirmModal.setAttribute('aria-hidden', 'true');
  state.pendingConfirm = null;
}

function renderRemotes() {
  ui.remotesTable.innerHTML = '';
  if (!state.remotes.length) {
    const row = document.createElement('tr');
    const cell = document.createElement('td');
    cell.colSpan = 5;
    cell.textContent = 'Brak zapisanych pilotow';
    row.appendChild(cell);
    ui.remotesTable.appendChild(row);
    return;
  }

  state.remotes.forEach(remote => {
    const row = document.createElement('tr');

    const nameCell = document.createElement('td');
    nameCell.textContent = remote.name || 'Bez nazwy';
    row.appendChild(nameCell);

    const idCell = document.createElement('td');
    idCell.textContent = remote.serial;
    row.appendChild(idCell);

    const statusCell = document.createElement('td');
    statusCell.innerHTML = `<span class="badge">${remote.enabled ? 'Aktywny' : 'Wylaczony'}</span>`;
    row.appendChild(statusCell);

    const counterCell = document.createElement('td');
    counterCell.textContent = remote.lastCounter || '-';
    row.appendChild(counterCell);

    const actionsCell = document.createElement('td');
    const renameBtn = document.createElement('button');
    renameBtn.className = 'btn ghost';
    renameBtn.textContent = 'Edytuj';
    renameBtn.addEventListener('click', () => openModal('edit', remote));

    const toggleBtn = document.createElement('button');
    toggleBtn.className = 'btn ghost';
    toggleBtn.textContent = remote.enabled ? 'Wylacz' : 'Wlacz';
    toggleBtn.addEventListener('click', () => updateRemote(remote.serial, remote.name, !remote.enabled));

    const deleteBtn = document.createElement('button');
    deleteBtn.className = 'btn danger';
    deleteBtn.textContent = 'Usun';
    deleteBtn.addEventListener('click', () => {
      openConfirm(`Usunac pilota ${remote.serial}?`, () => deleteRemote(remote.serial));
    });

    actionsCell.appendChild(renameBtn);
    actionsCell.appendChild(toggleBtn);
    actionsCell.appendChild(deleteBtn);
    row.appendChild(actionsCell);

    ui.remotesTable.appendChild(row);
  });
}

function updateLastRemote(data) {
  ui.lastSerial.textContent = data.serial || '-';
  ui.lastName.textContent = data.name || '-';
  ui.lastCounter.textContent = data.encript || data.lastCounter || '-';
  ui.lastAuth.textContent = data.authorized ? 'Autoryzowany' : 'Nieznany';
}

async function loadRemotes() {
  try {
    const res = await apiFetch('/api/remotes');
    const data = await res.json();
    state.remotes = data.items || [];
    renderRemotes();
  } catch {
    showToast('Brak dostepu do listy pilotow');
  }
}

async function loadLearnStatus() {
  try {
    const res = await apiFetch('/api/learn');
    const data = await res.json();
    state.learnMode = Boolean(data.enabled);
    ui.learnToggle.checked = state.learnMode;
    ui.learnStatus.textContent = state.learnMode ? 'Aktywny' : 'Wylaczony';
  } catch {
    showToast('Brak dostepu do learn mode');
  }
}

async function loadLastRemote() {
  try {
    const res = await apiFetch('/api/test_remote');
    const data = await res.json();
    updateLastRemote(data);
  } catch {
    // ignore
  }
}

async function updateRemote(serial, name, enabled) {
  try {
    await apiFetch('/api/remotes', {
      method: 'POST',
      body: JSON.stringify({ action: 'update', serial, name, enabled })
    });
    await loadRemotes();
    showToast('Zapisano');
  } catch (err) {
    const detail = err && err.data && (err.data.detail || err.data.error) ? (err.data.detail || err.data.error) : '';
    showToast(detail ? `Blad zapisu: ${detail}` : 'Blad zapisu');
  }
}

async function addRemote(serial, name, enabled) {
  try {
    await apiFetch('/api/remotes', {
      method: 'POST',
      body: JSON.stringify({ serial, name, enabled })
    });
    await loadRemotes();
    showToast('Dodano pilota');
  } catch (err) {
    const detail = err && err.data && (err.data.detail || err.data.error) ? (err.data.detail || err.data.error) : '';
    showToast(detail ? `Nie mozna dodac pilota: ${detail}` : 'Nie mozna dodac pilota');
  }
}

async function deleteRemote(serial) {
  try {
    await apiFetch('/api/remotes', {
      method: 'DELETE',
      body: JSON.stringify({ serial })
    });
    await loadRemotes();
    showToast('Usunieto');
  } catch (err) {
    const detail = err && err.data && (err.data.detail || err.data.error) ? (err.data.detail || err.data.error) : '';
    showToast(detail ? `Blad usuwania: ${detail}` : 'Blad usuwania');
  }
}

async function exportRemotes() {
  try {
    const res = await apiFetch('/api/remotes');
    const text = await res.text();
    const blob = new Blob([text], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'remotes.json';
    a.click();
    URL.revokeObjectURL(url);
  } catch {
    showToast('Eksport nieudany');
  }
}

async function importRemotes(file) {
  try {
    const text = await file.text();
    const data = JSON.parse(text);
    const items = data.items || data.remotes || [];
    for (const item of items) {
      const serial = item.serial || item;
      if (!serial) continue;
      await apiFetch('/api/remotes', {
        method: 'POST',
        body: JSON.stringify({ action: 'update', serial, name: item.name || '', enabled: item.enabled !== false, upsert: true })
      });
    }
    await loadRemotes();
    showToast('Import OK');
  } catch (err) {
    const detail = err && err.data && (err.data.detail || err.data.error) ? (err.data.detail || err.data.error) : '';
    showToast(detail ? `Import nieudany: ${detail}` : 'Import nieudany');
  }
}

function connectWs() {
  const url = `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`;
  const ws = new WebSocket(url);
  ws.onmessage = (evt) => {
    try {
      const msg = JSON.parse(evt.data);
      if (msg.type === 'status' && msg.data) {
        if (msg.data.remotes && msg.data.remotes.last) {
          updateLastRemote(msg.data.remotes.last);
        }
        if (typeof msg.data.remotes?.learnMode !== 'undefined') {
          ui.learnToggle.checked = msg.data.remotes.learnMode;
          ui.learnStatus.textContent = msg.data.remotes.learnMode ? 'Aktywny' : 'Wylaczony';
        }
      }
      if (msg.type === 'learn') {
        showToast(`Dodano pilota ${msg.serial}`);
        loadRemotes();
      }
      if (msg.type === 'test_remote') {
        loadLastRemote();
      }
    } catch {
      // ignore
    }
  };
  ws.onclose = () => setTimeout(connectWs, 2000);
}

function setupListeners() {
  ui.learnToggle.addEventListener('change', async () => {
    try {
      await apiFetch('/api/learn', { method: 'POST', body: JSON.stringify({ enable: ui.learnToggle.checked }) });
      await loadLearnStatus();
    } catch {
      showToast('Nie mozna ustawic learn mode');
    }
  });

  ui.listenBtn.addEventListener('click', () => {
    state.listenUntil = Date.now() + 10000;
    ui.learnStatus.textContent = 'Nasluch 10s';
    setTimeout(() => loadLearnStatus(), 10000);
  });

  ui.addRemoteBtn.addEventListener('click', () => openModal('add'));
  ui.remoteCancelBtn.addEventListener('click', closeModal);
  ui.remoteSaveBtn.addEventListener('click', async () => {
    const serial = parseInt(ui.remoteSerialInput.value, 10);
    const name = ui.remoteNameInput.value.trim();
    const enabled = ui.remoteEnabledInput.checked;
    if (!serial) {
      showToast('Podaj ID pilota');
      return;
    }
    if (state.editMode === 'add') {
      await addRemote(serial, name, enabled);
    } else {
      await updateRemote(state.editSerial, name, enabled);
    }
    closeModal();
  });

  ui.exportRemotesBtn.addEventListener('click', exportRemotes);
  ui.importRemotesBtn.addEventListener('click', () => ui.importRemotesFile.click());
  ui.importRemotesFile.addEventListener('change', (e) => {
    const file = e.target.files[0];
    if (file) importRemotes(file);
    e.target.value = '';
  });

  ui.confirmYes.addEventListener('click', async () => {
    if (state.pendingConfirm) await state.pendingConfirm();
    closeConfirm();
  });
  ui.confirmNo.addEventListener('click', closeConfirm);
}

window.addEventListener('load', () => {
  setupListeners();
  loadRemotes();
  loadLearnStatus();
  loadLastRemote();
  connectWs();
});
