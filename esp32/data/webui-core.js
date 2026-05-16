(function bootstrapGateOSWeb(global) {
  'use strict';

  if (global.GateOSWeb) return;

  const LEVELS = { silent: 0, error: 1, warn: 2, info: 3, debug: 4 };
  const DEFAULT_LEVEL = 'warn';
  const LOG_LEVEL_KEY = 'gateos.logLevel';
  const DEBUG_FLAG_KEY = 'gateos.debug';
  const LOCAL_BASE_URL_KEY = 'gateos.localBaseUrl';
  const LOCAL_BASE_URL_SOURCE_KEY = 'gateos.localBaseUrlSource';
  const registry = new Map();
  let configBaseUrl = '';
  let configBaseUrlLoaded = false;
  let configBaseUrlLoadPromise = null;
  let redirectingToPreferredBase = false;

  function safeStorageGet(key) {
    try {
      return global.localStorage ? global.localStorage.getItem(key) : '';
    } catch {
      return '';
    }
  }

  function safeStorageSet(key, value) {
    try {
      if (!global.localStorage) return;
      if (value === undefined || value === null || value === '') {
        global.localStorage.removeItem(key);
      } else {
        global.localStorage.setItem(key, value);
      }
    } catch {}
  }

  function nowMs() {
    return Date.now();
  }

  function resolveLevel(level) {
    if (typeof level === 'string' && LEVELS[level] !== undefined) return level;
    try {
      const params = new URLSearchParams(global.location.search || '');
      if (params.get('debug') === '1' || params.get('gateosDebug') === '1') return 'debug';
      const levelParam = params.get('logLevel');
      if (levelParam && LEVELS[levelParam] !== undefined) return levelParam;
    } catch {}
    if (safeStorageGet(DEBUG_FLAG_KEY) === '1') return 'debug';
    const stored = safeStorageGet(LOG_LEVEL_KEY);
    return LEVELS[stored] !== undefined ? stored : DEFAULT_LEVEL;
  }

  function createLogger(namespace, options = {}) {
    const fixedLevel = options.level;

    function canLog(level) {
      const current = LEVELS[resolveLevel(fixedLevel)] ?? LEVELS[DEFAULT_LEVEL];
      return current >= (LEVELS[level] ?? LEVELS.warn);
    }

    function emit(level, args) {
      if (!canLog(level)) return;
      const prefix = `[GateOS][${namespace}]`;
      const printer = level === 'debug'
        ? console.debug
        : level === 'info'
          ? console.info
          : level === 'warn'
            ? console.warn
            : console.error;
      printer(prefix, ...args);
    }

    return {
      error(...args) { emit('error', args); },
      warn(...args) { emit('warn', args); },
      info(...args) { emit('info', args); },
      debug(...args) { emit('debug', args); },
      setLevel(level) { safeStorageSet(LOG_LEVEL_KEY, level); },
      enableDebug() { safeStorageSet(DEBUG_FLAG_KEY, '1'); },
      disableDebug() { safeStorageSet(DEBUG_FLAG_KEY, ''); },
      getLevel() { return resolveLevel(fixedLevel); }
    };
  }

  function getToken(tokenKey = 'apiToken') {
    return safeStorageGet(tokenKey) || '';
  }

  function getCurrentOrigin() {
    if (global.location && global.location.origin) return global.location.origin;
    const protocol = global.location && global.location.protocol ? global.location.protocol : 'http:';
    const host = global.location && global.location.host ? global.location.host : '';
    return host ? `${protocol}//${host}` : '';
  }

  function isAbsoluteUrl(value) {
    return /^(?:https?|wss?):\/\//i.test(value || '') || `${value || ''}`.startsWith('//');
  }

  function normalizeBaseUrl(value) {
    const raw = typeof value === 'string' ? value.trim() : '';
    if (!raw) return '';
    try {
      const parsed = new URL(raw);
      if (!(parsed.protocol === 'http:' || parsed.protocol === 'https:')) return '';
      if (parsed.username || parsed.password || parsed.search || parsed.hash) return '';
      if (parsed.pathname && parsed.pathname !== '/') return '';
      return `${parsed.protocol}//${parsed.host}`;
    } catch {
      return '';
    }
  }

  function joinUrl(baseUrl, path) {
    if (!path) return baseUrl || '';
    if (isAbsoluteUrl(path)) return path;
    const base = baseUrl || getCurrentOrigin();
    try {
      return new URL(path, `${base.replace(/\/+$/, '')}/`).toString();
    } catch {
      const prefix = base.replace(/\/+$/, '');
      return `${prefix}${path.startsWith('/') ? '' : '/'}${path}`;
    }
  }

  function getStoredBaseUrlState() {
    return {
      value: normalizeBaseUrl(safeStorageGet(LOCAL_BASE_URL_KEY)),
      source: safeStorageGet(LOCAL_BASE_URL_SOURCE_KEY) || ''
    };
  }

  function getPreferredBaseUrlState() {
    if (configBaseUrl) return { value: configBaseUrl, source: 'config' };
    const stored = getStoredBaseUrlState();
    if (stored.value) return { value: stored.value, source: stored.source || 'storage' };
    return { value: '', source: 'origin' };
  }

  function syncStoredBaseUrl(value, source) {
    if (value) {
      safeStorageSet(LOCAL_BASE_URL_KEY, value);
      safeStorageSet(LOCAL_BASE_URL_SOURCE_KEY, source || 'storage');
      return;
    }
    const stored = getStoredBaseUrlState();
    if (source === 'config' && stored.source !== 'config') return;
    safeStorageSet(LOCAL_BASE_URL_KEY, '');
    safeStorageSet(LOCAL_BASE_URL_SOURCE_KEY, '');
  }

  function maybeRedirectToPreferredBaseUrl(baseUrl) {
    const normalized = normalizeBaseUrl(baseUrl);
    const currentOrigin = getCurrentOrigin();
    if (!normalized || !currentOrigin || normalized === currentOrigin) return false;
    const targetUrl = `${normalized}${global.location.pathname || '/'}${global.location.search || ''}${global.location.hash || ''}`;
    if (targetUrl === global.location.href) return false;
    redirectingToPreferredBase = true;
    global.location.replace(targetUrl);
    return true;
  }

  function applyConfigBaseUrl(config, options = {}) {
    const normalized = normalizeBaseUrl(config && config.device ? config.device.localBaseUrl : '');
    configBaseUrl = normalized;
    configBaseUrlLoaded = true;
    syncStoredBaseUrl(normalized, 'config');
    if (options.navigate) maybeRedirectToPreferredBaseUrl(normalized);
    return normalized;
  }

  async function ensurePreferredBaseUrlLoaded(options = {}) {
    if (configBaseUrlLoaded) {
      if (options.navigate) maybeRedirectToPreferredBaseUrl(getPreferredBaseUrlState().value);
      return getPreferredBaseUrlState();
    }
    if (configBaseUrlLoadPromise) return configBaseUrlLoadPromise;

    configBaseUrlLoadPromise = (async () => {
      const headers = {};
      const token = getToken(options.tokenKey || 'apiToken');
      if (token) headers['X-Api-Key'] = token;

      try {
        const response = await fetch(joinUrl(getCurrentOrigin(), '/api/config'), {
          headers,
          cache: 'no-store'
        });
        if (response.ok) {
          const text = await response.text();
          let data = null;
          try {
            data = text ? JSON.parse(text) : null;
          } catch {}
          if (data && typeof data === 'object') {
            applyConfigBaseUrl(data, { navigate: Boolean(options.navigate) });
          } else {
            configBaseUrlLoaded = true;
          }
        } else {
          configBaseUrlLoaded = true;
        }
      } catch {
        configBaseUrlLoaded = true;
      }

      if (options.navigate) maybeRedirectToPreferredBaseUrl(getPreferredBaseUrlState().value);
      return getPreferredBaseUrlState();
    })().finally(() => {
      configBaseUrlLoadPromise = null;
    });

    return configBaseUrlLoadPromise;
  }

  function getApiBaseUrl() {
    const currentOrigin = getCurrentOrigin();
    const preferred = getPreferredBaseUrlState().value;
    return preferred && preferred !== currentOrigin ? preferred : '';
  }

  function getWsBaseUrl() {
    const apiBaseUrl = getApiBaseUrl();
    return apiBaseUrl ? apiBaseUrl.replace(/^http/i, 'ws') : '';
  }

  function resolveApiUrl(path) {
    if (isAbsoluteUrl(path)) return path;
    const apiBaseUrl = getApiBaseUrl();
    return apiBaseUrl ? joinUrl(apiBaseUrl, path) : path;
  }

  function resolveHttpUrl(pathOrUrl) {
    if (!pathOrUrl) return '';
    if (isAbsoluteUrl(pathOrUrl)) return pathOrUrl;
    const preferred = getPreferredBaseUrlState().value || getCurrentOrigin();
    return joinUrl(preferred, pathOrUrl);
  }

  function appendTokenToWebSocketUrl(url, tokenKey = 'apiToken') {
    const token = getToken(tokenKey);
    if (!token || !url) return url;
    try {
      const parsed = new URL(url, getCurrentOrigin());
      parsed.searchParams.set('token', token);
      return parsed.toString();
    } catch {
      const separator = url.includes('?') ? '&' : '?';
      return `${url}${separator}token=${encodeURIComponent(token)}`;
    }
  }

  function resolveWebSocketUrl(path = '/ws', tokenKey = 'apiToken') {
    if (isAbsoluteUrl(path)) return appendTokenToWebSocketUrl(path, tokenKey);
    const wsBaseUrl = getWsBaseUrl();
    if (wsBaseUrl) return appendTokenToWebSocketUrl(joinUrl(wsBaseUrl, path), tokenKey);
    return appendTokenToWebSocketUrl(`${global.location.protocol === 'https:' ? 'wss' : 'ws'}://${global.location.host}${path}`, tokenKey);
  }

  function isRedirectingToPreferredBase() {
    return redirectingToPreferredBase;
  }

  function createApiClient(options = {}) {
    const logger = options.logger || createLogger('api');
    const tokenKey = options.tokenKey || 'apiToken';
    const defaultTimeoutMs = Number.isFinite(options.defaultTimeoutMs) ? options.defaultTimeoutMs : 2000;
    const controllers = new Map();

    function abort(requestKey, reason = 'manual_abort') {
      if (!requestKey || !controllers.has(requestKey)) return;
      const controller = controllers.get(requestKey);
      controllers.delete(requestKey);
      try {
        controller.abort(reason);
      } catch {}
    }

    function abortAll(reason = 'abort_all') {
      Array.from(controllers.keys()).forEach((key) => abort(key, reason));
    }

    async function request(path, reqOptions = {}) {
      await ensurePreferredBaseUrlLoaded({ navigate: false, tokenKey });
      const requestKey = reqOptions.requestKey || '';
      if (requestKey && reqOptions.cancelPrevious !== false) abort(requestKey, 'replaced_request');

      const controller = new AbortController();
      if (requestKey) controllers.set(requestKey, controller);

      const externalSignal = reqOptions.signal;
      let removeExternalAbort = null;
      if (externalSignal) {
        if (externalSignal.aborted) controller.abort(externalSignal.reason || 'external_abort');
        const onAbort = () => controller.abort(externalSignal.reason || 'external_abort');
        externalSignal.addEventListener('abort', onAbort, { once: true });
        removeExternalAbort = () => externalSignal.removeEventListener('abort', onAbort);
      }

      const headers = { ...(reqOptions.headers || {}) };
      const token = getToken(tokenKey);
      if (token) headers['X-Api-Key'] = token;
      if (reqOptions.body && !(reqOptions.body instanceof FormData) && !headers['Content-Type']) {
        headers['Content-Type'] = 'application/json';
      }

      const timeoutMs = reqOptions.timeoutMs === undefined ? defaultTimeoutMs : reqOptions.timeoutMs;
      const timeoutId = timeoutMs > 0
        ? setTimeout(() => controller.abort('timeout'), timeoutMs)
        : null;

      try {
        const response = await fetch(reqOptions.resolveBaseUrl === false ? path : resolveApiUrl(path), {
          method: reqOptions.method || 'GET',
          body: reqOptions.body,
          headers,
          signal: controller.signal,
          cache: reqOptions.cache || 'no-store'
        });

        if (reqOptions.responseType === 'raw') {
          if (!response.ok) {
            const error = new Error(`HTTP ${response.status}`);
            error.status = response.status;
            error.response = response;
            throw error;
          }
          return { res: response, data: null, text: '' };
        }

        const text = await response.text();
        const data = reqOptions.responseType === 'text'
          ? text
          : (() => {
              try {
                return text ? JSON.parse(text) : null;
              } catch {
                return null;
              }
            })();

        if (!response.ok) {
          const error = new Error(`HTTP ${response.status}`);
          error.status = response.status;
          error.data = data;
          error.text = text;
          error.response = response;
          throw error;
        }

        return { res: response, data, text };
      } catch (error) {
        if (error && error.name === 'AbortError') {
          logger.debug('request aborted', reqOptions.method || 'GET', path, requestKey || '(no-key)');
        }
        throw error;
      } finally {
        if (timeoutId) clearTimeout(timeoutId);
        if (removeExternalAbort) removeExternalAbort();
        if (requestKey && controllers.get(requestKey) === controller) controllers.delete(requestKey);
      }
    }

    return {
      request,
      getToken: () => getToken(tokenKey),
      abort,
      abortAll
    };
  }

  function createScheduler() {
    const intervals = new Set();
    const timeouts = new Set();
    const cleanups = new Set();

    function every(fn, ms) {
      const id = setInterval(fn, ms);
      intervals.add(id);
      return () => {
        clearInterval(id);
        intervals.delete(id);
      };
    }

    function after(fn, ms) {
      const id = setTimeout(() => {
        timeouts.delete(id);
        fn();
      }, ms);
      timeouts.add(id);
      return () => {
        clearTimeout(id);
        timeouts.delete(id);
      };
    }

    function addCleanup(fn) {
      if (typeof fn !== 'function') return () => {};
      cleanups.add(fn);
      return () => cleanups.delete(fn);
    }

    function clearAll() {
      intervals.forEach((id) => clearInterval(id));
      timeouts.forEach((id) => clearTimeout(id));
      intervals.clear();
      timeouts.clear();
      cleanups.forEach((fn) => {
        try {
          fn();
        } catch {}
      });
      cleanups.clear();
    }

    return {
      every,
      after,
      addCleanup,
      clearAll
    };
  }

  function createRafBatcher(renderer) {
    let frameId = 0;
    let payload = null;

    return function schedule(nextPayload) {
      payload = nextPayload;
      if (frameId) return;
      frameId = requestAnimationFrame(() => {
        frameId = 0;
        const current = payload;
        payload = null;
        renderer(current);
      });
    };
  }

  function createStateStore(initialState = {}) {
    let current = initialState;
    const listeners = new Set();

    function getState() {
      return current;
    }

    function setState(update) {
      current = typeof update === 'function' ? update(current) : { ...current, ...update };
      listeners.forEach((listener) => {
        try {
          listener(current);
        } catch {}
      });
      return current;
    }

    function subscribe(listener) {
      if (typeof listener !== 'function') return () => {};
      listeners.add(listener);
      return () => listeners.delete(listener);
    }

    return {
      getState,
      setState,
      subscribe
    };
  }

  function bindPageLifecycle(handlers = {}) {
    const scheduler = createScheduler();
    let lastHiddenAt = document.hidden ? nowMs() : 0;

    function onHide(reason) {
      lastHiddenAt = nowMs();
      if (typeof handlers.onHide === 'function') handlers.onHide({ reason, hiddenAt: lastHiddenAt });
    }

    function onShow(reason) {
      const shownAt = nowMs();
      const sleptMs = lastHiddenAt ? shownAt - lastHiddenAt : 0;
      if (typeof handlers.onShow === 'function') handlers.onShow({ reason, shownAt, sleptMs });
      if (sleptMs > 1500 && typeof handlers.onWake === 'function') handlers.onWake({ reason, shownAt, sleptMs });
    }

    function listen(target, eventName, handler, options) {
      target.addEventListener(eventName, handler, options);
      scheduler.addCleanup(() => target.removeEventListener(eventName, handler, options));
    }

    listen(document, 'visibilitychange', () => {
      if (document.hidden) onHide('visibilitychange');
      else onShow('visibilitychange');
    });

    listen(global, 'pagehide', () => onHide('pagehide'));
    listen(global, 'pageshow', (event) => {
      onShow('pageshow');
      if (event.persisted && typeof handlers.onWake === 'function') {
        handlers.onWake({ reason: 'pageshow', persisted: true, shownAt: nowMs(), sleptMs: lastHiddenAt ? nowMs() - lastHiddenAt : 0 });
      }
    });
    listen(global, 'focus', () => onShow('focus'));
    listen(global, 'online', () => {
      if (typeof handlers.onOnline === 'function') handlers.onOnline({ reason: 'online' });
    });
    listen(global, 'offline', () => {
      if (typeof handlers.onOffline === 'function') handlers.onOffline({ reason: 'offline' });
    });

    return {
      cleanup: () => scheduler.clearAll()
    };
  }

  function createWebSocketManager(options = {}) {
    const path = options.path || '/ws';
    const key = options.key || `${global.location.origin}${path}`;
    if (registry.has(key)) return registry.get(key);

    const logger = options.logger || createLogger(`ws:${path}`);
    const tokenKey = options.tokenKey || 'apiToken';
    const baseDelayMs = Number.isFinite(options.baseDelayMs) ? options.baseDelayMs : 2000;
    const maxDelayMs = Number.isFinite(options.maxDelayMs) ? options.maxDelayMs : 30000;
    const cooldownMs = Number.isFinite(options.cooldownMs) ? options.cooldownMs : 30000;
    const maxRapidFailures = Number.isFinite(options.maxRapidFailures) ? options.maxRapidFailures : 3;
    const heartbeatIntervalMs = Number.isFinite(options.heartbeatIntervalMs) ? options.heartbeatIntervalMs : 15000;
    const staleTimeoutMs = Number.isFinite(options.staleTimeoutMs) ? options.staleTimeoutMs : 45000;
    const subscribers = new Set();

    const state = {
      socket: null,
      desired: false,
      visible: !document.hidden,
      online: global.navigator ? global.navigator.onLine !== false : true,
      connecting: false,
      reconnectTimer: null,
      heartbeatTimer: null,
      lastMessageAt: 0,
      backoffMs: baseDelayMs,
      rapidFailures: 0,
      openedAt: 0,
      stableConnected: false,
    };

    function snapshot() {
      return {
        connected: Boolean(state.socket && state.socket.readyState === WebSocket.OPEN),
        connecting: state.connecting,
        visible: state.visible,
        online: state.online,
        lastMessageAt: state.lastMessageAt,
        backoffMs: state.backoffMs,
        rapidFailures: state.rapidFailures
      };
    }

    function emit(name, payload) {
      subscribers.forEach((subscriber) => {
        const handler = subscriber && subscriber[name];
        if (typeof handler !== 'function') return;
        try {
          handler(payload, snapshot());
        } catch (error) {
          logger.warn('subscriber handler failed', name, error && error.message ? error.message : error);
        }
      });
    }

    function clearReconnectTimer() {
      if (!state.reconnectTimer) return;
      clearTimeout(state.reconnectTimer);
      state.reconnectTimer = null;
    }

    function clearHeartbeatTimer() {
      if (!state.heartbeatTimer) return;
      clearInterval(state.heartbeatTimer);
      state.heartbeatTimer = null;
    }

    function closeSocket(code = 1000, reason = 'manual') {
      clearReconnectTimer();
      clearHeartbeatTimer();
      const socket = state.socket;
      state.socket = null;
      state.connecting = false;
      state.openedAt = 0;
      state.stableConnected = false;
      if (!socket) return;
      socket.onopen = null;
      socket.onmessage = null;
      socket.onerror = null;
      socket.onclose = null;
      try {
        if (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CONNECTING) {
          socket.close(code, reason);
        }
      } catch {}
    }

    function scheduleReconnect(delayMs) {
      if (!state.desired || !state.visible || !state.online) return;
      clearReconnectTimer();
      emit('scheduledReconnect', { delayMs });
      state.reconnectTimer = setTimeout(() => {
        state.reconnectTimer = null;
        openSocket('reconnect');
      }, delayMs);
    }

    function startHeartbeatChecks() {
      clearHeartbeatTimer();
      state.heartbeatTimer = setInterval(() => {
        if (!state.socket || state.socket.readyState !== WebSocket.OPEN) return;
        const ageMs = state.lastMessageAt ? nowMs() - state.lastMessageAt : staleTimeoutMs + 1;
        emit('heartbeat', { ageMs });
        if (ageMs < staleTimeoutMs) return;
        emit('stale', { ageMs });
        reconnect('stale');
      }, heartbeatIntervalMs);
    }

    function openSocket(reason) {
      if (!state.desired || !state.visible || !state.online) return;
      if (state.connecting) return;
      if (state.socket && (state.socket.readyState === WebSocket.OPEN || state.socket.readyState === WebSocket.CONNECTING)) return;

      clearReconnectTimer();
      state.connecting = true;
      emit('connecting', { reason });

      ensurePreferredBaseUrlLoaded({ navigate: false, tokenKey }).then(() => {
        if (!state.desired || !state.visible || !state.online) {
          state.connecting = false;
          return;
        }

        const url = resolveWebSocketUrl(path, tokenKey);
        const socket = new WebSocket(url);
        state.socket = socket;

        socket.onopen = () => {
          if (state.socket !== socket) return;
          state.connecting = false;
          state.openedAt = nowMs();
          state.lastMessageAt = nowMs();
          emit('open', { reason });
          startHeartbeatChecks();
        };

        socket.onmessage = (event) => {
          if (state.socket !== socket) return;
          state.lastMessageAt = nowMs();
          if (!state.stableConnected) {
            state.stableConnected = true;
            state.rapidFailures = 0;
            state.backoffMs = baseDelayMs;
          }
          emit('message', event);
        };

        socket.onerror = (event) => {
          if (state.socket !== socket) return;
          emit('error', event);
        };

        socket.onclose = (event) => {
          if (state.socket !== socket) return;
          const openedAt = state.openedAt;
          state.socket = null;
          state.connecting = false;
          state.openedAt = 0;
          state.stableConnected = false;
          clearHeartbeatTimer();
          emit('close', event);
          if (!state.desired || !state.visible || !state.online) return;

          const shortLived = openedAt > 0 && (nowMs() - openedAt) < 500;
          const abnormal = event.code === 1006 || event.code === 1007 || event.code === 1002 ||
                           event.code === 1008 || event.wasClean === false || shortLived;
          if (abnormal) state.rapidFailures += 1;
          else state.rapidFailures = 0;

          if (state.rapidFailures >= maxRapidFailures) {
            state.rapidFailures = 0;
            state.backoffMs = baseDelayMs;
            scheduleReconnect(cooldownMs);
            return;
          }

          const delayMs = state.backoffMs;
          state.backoffMs = Math.min(maxDelayMs, Math.round(state.backoffMs * 1.8));
          scheduleReconnect(delayMs);
        };
      }).catch((error) => {
        state.connecting = false;
        emit('error', error);
        if (!state.desired || !state.visible || !state.online) return;
        const delayMs = state.backoffMs;
        state.backoffMs = Math.min(maxDelayMs, Math.round(state.backoffMs * 1.8));
        scheduleReconnect(delayMs);
      });
    }

    function connect() {
      state.desired = true;
      openSocket('connect');
    }

    function disconnect() {
      state.desired = false;
      closeSocket(1000, 'disconnect');
    }

    function reconnect(reason = 'manual') {
      closeSocket(1000, reason);
      if (!state.desired || !state.visible || !state.online) return;
      scheduleReconnect(baseDelayMs);
    }

    function setVisibility(visible) {
      state.visible = Boolean(visible);
      if (!state.visible) {
        closeSocket(1000, 'hidden');
        return;
      }
      if (state.desired) openSocket('visible');
    }

    function setOnline(online) {
      state.online = Boolean(online);
      if (!state.online) {
        closeSocket(1000, 'offline');
        return;
      }
      if (state.desired && state.visible) openSocket('online');
    }

    function subscribe(handlers) {
      subscribers.add(handlers || {});
      return () => subscribers.delete(handlers || {});
    }

    const api = {
      connect,
      disconnect,
      reconnect,
      subscribe,
      setVisibility,
      setOnline,
      isConnected: () => Boolean(state.socket && state.socket.readyState === WebSocket.OPEN),
      isConnecting: () => state.connecting,
      snapshot,
      destroy() {
        disconnect();
        subscribers.clear();
        registry.delete(key);
      }
    };

    registry.set(key, api);
    return api;
  }

  global.GateOSWeb = {
    createLogger,
    createApiClient,
    createScheduler,
    createRafBatcher,
    createStateStore,
    createWebSocketManager,
    bindPageLifecycle,
    getToken,
    getCurrentOrigin,
    normalizeBaseUrl,
    ensurePreferredBaseUrlLoaded,
    applyConfigBaseUrl,
    getApiBaseUrl,
    getWsBaseUrl,
    resolveApiUrl,
    resolveHttpUrl,
    resolveWebSocketUrl,
    isRedirectingToPreferredBase,
    safeStorageGet,
    safeStorageSet,
    nowMs
  };
})(window);