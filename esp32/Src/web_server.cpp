#include "web_server.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <map>
#include <Update.h>
#include "mqtt_manager.h"
#include "led_controller.h"
#include "motor_controller.h"
#include <new>

extern void scheduleRestart(uint32_t delayMs);
extern void scheduleFactoryReset(uint32_t delayMs);
extern void scheduleRuntimeConfigApply();

void WebServerManager::setOtaActiveCallback(OtaActiveCb cb) { otaActiveCb = cb; }

namespace {
enum class BodyAppendResult {
  Ok = 0,
  BadOffset,
  BadSize,
  TooLarge
};

static constexpr size_t kMaxJsonBodyBytes = CONFIG_JSON_CAPACITY + 2048;
static constexpr size_t kMaxAuthTokenBytes = 128;
static constexpr size_t kMaxFsListEntries = 32;
static constexpr uint32_t kMaxFsListScanUs = 20000U;

struct BodyBuffer {
  String body;
  size_t total = 0;
  bool badOffset = false;
  // FIX A4: timestamp for stale-buffer cleanup in maintenance().
  unsigned long startMs = 0;
};

struct JsonParseDiag {
  unsigned long lastBadMs = 0;
  size_t lastBadLen = 0;
  char lastBadTag[32] = {0};
  char lastBadErr[48] = {0};
  char lastBadHead[96] = {0};
};

static JsonParseDiag g_jsonParseDiag;
static std::map<AsyncWebServerRequest*, BodyBuffer*> g_bodyBuffers;
static uint32_t g_wsSendOk = 0;
static uint32_t g_wsSendSkippedLowHeap = 0;
static uint32_t g_wsSendSkippedNoClient = 0;
static unsigned long g_lastWsSkipLogMs = 0;
static unsigned long g_lastWsBuildSkipLogMs = 0;
static unsigned long g_lastAuthFailLogMs = 0;
static unsigned long g_lastStatusUsageLogMs = 0;
static uint32_t g_authFailSuppressed = 0;
static uint32_t g_authFailTotal = 0;
static uint32_t g_otaAbortCount = 0;
static portMUX_TYPE g_wsPeersMux = portMUX_INITIALIZER_UNLOCKED;

struct WsPeerInfo {
  uint32_t clientId = 0;
  uint32_t ipKey = 0;
  uint32_t connectedAtMs = 0;
};

static std::map<uint32_t, WsPeerInfo> g_wsActivePeers;
static std::map<uint32_t, uint16_t> g_wsPeerCountByIp;
static uint32_t g_lastWsConnectIpKey = 0;
static uint32_t g_lastWsDisconnectIpKey = 0;
static uint32_t g_lastWsConnectClientId = 0;
static uint32_t g_lastWsDisconnectClientId = 0;

static bool isProtectedStaticPath(const String& url) {
  return url == CONFIG_PATH || url == CONFIG_BAK_PATH || url == CONFIG_TMP_PATH;
}

uint32_t ipAddressToKey(const IPAddress& ip) {
  return ((uint32_t)ip[0] << 24) |
         ((uint32_t)ip[1] << 16) |
         ((uint32_t)ip[2] << 8)  |
         ((uint32_t)ip[3]);
}

String ipKeyToString(uint32_t ipKey) {
  IPAddress ip((uint8_t)(ipKey >> 24),
               (uint8_t)(ipKey >> 16),
               (uint8_t)(ipKey >> 8),
               (uint8_t)ipKey);
  return ip.toString();
}

void noteWsConnect(AsyncWebSocketClient* client) {
  if (!client) return;
  const uint32_t clientId = client->id();
  const uint32_t ipKey = ipAddressToKey(client->remoteIP());
  const uint32_t nowMs = millis();
  uint16_t ipCount = 0;
  uint16_t totalClients = 0;
  portENTER_CRITICAL(&g_wsPeersMux);
  WsPeerInfo& peer = g_wsActivePeers[clientId];
  peer.clientId = clientId;
  peer.ipKey = ipKey;
  peer.connectedAtMs = nowMs;
  uint16_t& count = g_wsPeerCountByIp[ipKey];
  count++;
  ipCount = count;
  g_lastWsConnectIpKey = ipKey;
  g_lastWsConnectClientId = clientId;
  totalClients = (uint16_t)g_wsActivePeers.size();
  portEXIT_CRITICAL(&g_wsPeersMux);
  Serial.printf("[WS] connect id=%lu ip=%s clients=%u ipCount=%u\n",
                (unsigned long)clientId,
                ipKeyToString(ipKey).c_str(),
                (unsigned)totalClients,
                (unsigned)ipCount);
}

void noteWsDisconnect(AsyncWebSocketClient* client) {
  if (!client) return;
  const uint32_t clientId = client->id();
  uint32_t ipKey = ipAddressToKey(client->remoteIP());
  uint16_t ipCount = 0;
  uint16_t totalClients = 0;
  portENTER_CRITICAL(&g_wsPeersMux);
  auto peerIt = g_wsActivePeers.find(clientId);
  if (peerIt != g_wsActivePeers.end()) {
    ipKey = peerIt->second.ipKey;
    g_wsActivePeers.erase(peerIt);
  }
  auto countIt = g_wsPeerCountByIp.find(ipKey);
  if (countIt != g_wsPeerCountByIp.end()) {
    if (countIt->second > 0) countIt->second--;
    ipCount = countIt->second;
    if (countIt->second == 0) g_wsPeerCountByIp.erase(countIt);
  }
  g_lastWsDisconnectIpKey = ipKey;
  g_lastWsDisconnectClientId = clientId;
  totalClients = (uint16_t)g_wsActivePeers.size();
  portEXIT_CRITICAL(&g_wsPeersMux);
  Serial.printf("[WS] disconnect id=%lu ip=%s clients=%u ipCount=%u\n",
                (unsigned long)clientId,
                ipKeyToString(ipKey).c_str(),
                (unsigned)totalClients,
                (unsigned)ipCount);
}

bool tryReadTokenValue(const String& candidate, String& out, bool& tooLong) {
  if (candidate.length() == 0) return false;
  if (candidate.length() > kMaxAuthTokenBytes) {
    tooLong = true;
    return false;
  }
  out = candidate;
  return true;
}

bool extractAuthToken(AsyncWebServerRequest* request, bool allowQueryToken, String& token, bool& tooLong) {
  token = "";
  tooLong = false;
  if (!request) return false;
  if (allowQueryToken && request->hasParam("token")) {
    if (tryReadTokenValue(request->getParam("token")->value(), token, tooLong) || tooLong) return token.length() > 0;
  }
  if (request->hasHeader("X-Api-Key")) {
    if (tryReadTokenValue(request->getHeader("X-Api-Key")->value(), token, tooLong) || tooLong) return token.length() > 0;
  }
  if (request->hasHeader("X-API-Token")) {
    if (tryReadTokenValue(request->getHeader("X-API-Token")->value(), token, tooLong) || tooLong) return token.length() > 0;
  }
  return token.length() > 0;
}

static void copyPrintable(char* dst, size_t dstSize, const char* src, size_t srcLen) {
  if (!dst || dstSize == 0) return;
  size_t out = 0;
  for (size_t i = 0; i < srcLen && out + 1 < dstSize; ++i) {
    char c = src[i];
    if (c >= 0x20 && c <= 0x7E) {
      dst[out++] = c;
    } else {
      dst[out++] = '.';
    }
  }
  dst[out] = '\0';
}

static void setJsonBad(const char* tag, const char* err, size_t len, const String& head) {
  g_jsonParseDiag.lastBadMs = millis();
  g_jsonParseDiag.lastBadLen = len;
  strncpy(g_jsonParseDiag.lastBadTag, tag ? tag : "", sizeof(g_jsonParseDiag.lastBadTag) - 1);
  strncpy(g_jsonParseDiag.lastBadErr, err ? err : "", sizeof(g_jsonParseDiag.lastBadErr) - 1);
  copyPrintable(g_jsonParseDiag.lastBadHead, sizeof(g_jsonParseDiag.lastBadHead), head.c_str(), head.length());
}

void releaseBody(AsyncWebServerRequest* request) {
  auto it = g_bodyBuffers.find(request);
  if (it != g_bodyBuffers.end()) {
    delete it->second;
    g_bodyBuffers.erase(it);
  }
}

BodyBuffer* getBodyBuffer(AsyncWebServerRequest* request, size_t total) {
  auto it = g_bodyBuffers.find(request);
  if (it != g_bodyBuffers.end()) return it->second;
  auto* buf = new BodyBuffer();
  buf->total = total;
  buf->startMs = millis(); // FIX A4: record creation time
  if (total > 0) buf->body.reserve(total + 1);
  g_bodyBuffers[request] = buf;
  return buf;
}

BodyAppendResult appendBody(BodyBuffer* buf, const uint8_t* data, size_t len, size_t index, size_t total) {
  if (!buf) return BodyAppendResult::BadSize;
  if (total > kMaxJsonBodyBytes) return BodyAppendResult::TooLarge;
  if (index == 0) {
    buf->body = "";
    buf->badOffset = false;
    buf->total = total;
    if (total > 0) buf->body.reserve(total + 1);
  }
  if (buf->body.length() != index) {
    buf->badOffset = true;
    return BodyAppendResult::BadOffset;
  }
  if (total > 0 && index + len > total) return BodyAppendResult::BadSize;
  if (buf->body.length() + len > kMaxJsonBodyBytes) return BodyAppendResult::TooLarge;
  // Avoid String::concat(pointer, len) overload ambiguity across cores; append byte-by-byte.
  buf->body.reserve(buf->body.length() + len + 1);
  for (size_t i = 0; i < len; ++i) {
    buf->body += static_cast<char>(data[i]);
  }
  return BodyAppendResult::Ok;
}

BodyBuffer* appendBodyChunk(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
  if (total > kMaxJsonBodyBytes) {
    request->send(413, "application/json", "{\"status\":\"payload_too_large\"}");
    releaseBody(request);
    return nullptr;
  }
  BodyBuffer* buf = getBodyBuffer(request, total);
  BodyAppendResult res = appendBody(buf, data, len, index, total);
  if (res != BodyAppendResult::Ok) {
    if (res == BodyAppendResult::TooLarge) {
      request->send(413, "application/json", "{\"status\":\"payload_too_large\"}");
    } else if (res == BodyAppendResult::BadOffset) {
      request->send(400, "application/json", "{\"status\":\"bad_body_offset\"}");
    } else {
      request->send(400, "application/json", "{\"status\":\"bad_body\"}");
    }
    releaseBody(request);
    return nullptr;
  }
  return buf;
}

bool parseJsonBody(AsyncWebServerRequest* request, JsonDocument& doc, const char* tag) {
  auto it = g_bodyBuffers.find(request);
  if (it == g_bodyBuffers.end()) {
    setJsonBad(tag, "no_body_buffer", 0, "");
    return false;
  }
  BodyBuffer* buf = it->second;
  size_t len = buf->body.length();
  const char* body = buf->body.c_str();
  if (len >= 3 &&
      static_cast<uint8_t>(body[0]) == 0xEF &&
      static_cast<uint8_t>(body[1]) == 0xBB &&
      static_cast<uint8_t>(body[2]) == 0xBF) {
    Serial.printf("JSON parse error (%s): utf8_bom len=%u\n", tag, (unsigned)len);
    setJsonBad(tag, "utf8_bom", len, len > 80 ? buf->body.substring(0, 80) : buf->body);
    releaseBody(request);
    return false;
  }
  // Parse from String directly to preserve length and avoid early-NULL truncation edge-cases.
  DeserializationError err = deserializeJson(doc, buf->body);
  String head = len > 80 ? buf->body.substring(0, 80) : buf->body;
  releaseBody(request);
  if (err) {
    Serial.printf("JSON parse error (%s): %s (len=%u head=%s)\n",
                  tag, err.c_str(), (unsigned)len, head.c_str());
    setJsonBad(tag, err.c_str(), len, head);
    return false;
  }
  return true;
}

void sendJson(AsyncWebServerRequest* request, JsonDocument& doc, int code = 200) {
  AsyncResponseStream* response = request->beginResponseStream("application/json");
  response->setCode(code);
  serializeJson(doc, *response);
  request->send(response);
}

template <typename TDoc>
String serializeJsonString(const TDoc& doc) {
  String payload;
  payload.reserve(measureJson(doc) + 1);
  serializeJson(doc, payload);
  return payload;
}

void logWsBuildSkip(const char* tag, const char* reason, size_t payloadLen) {
  const unsigned long now = millis();
  if (now - g_lastWsBuildSkipLogMs < 5000UL) return;
  g_lastWsBuildSkipLogMs = now;
  Serial.printf("[WS] skip build tag=%s reason=%s len=%u heap=%u maxAlloc=%u\n",
                tag ? tag : "unknown",
                reason ? reason : "unknown",
                (unsigned)payloadLen,
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap());
}

bool hasWsHeapForPayload(size_t payloadLen) {
  uint32_t needed = (uint32_t)payloadLen + 2048U;
  if (needed < 4096U) needed = 4096U;
  return ESP.getMaxAllocHeap() >= needed && ESP.getFreeHeap() >= (needed + 2048U);
}

void logWsSkipLowHeap(const char* tag, size_t payloadLen) {
  const unsigned long now = millis();
  if (now - g_lastWsSkipLogMs < 5000UL) return;
  g_lastWsSkipLogMs = now;
  Serial.printf("[WS] skip low_heap tag=%s len=%u ok=%lu skipLowHeap=%lu skipNoClient=%lu heap=%u maxAlloc=%u\n",
                tag ? tag : "unknown",
                (unsigned)payloadLen,
                (unsigned long)g_wsSendOk,
                (unsigned long)g_wsSendSkippedLowHeap,
                (unsigned long)g_wsSendSkippedNoClient,
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap());
}

bool safeWsTextAll(AsyncWebSocket& socket, const char* payload, size_t payloadLen, const char* tag) {
  if (!payload || payloadLen == 0) return false;
  const size_t clients = socket.count();
  if (clients == 0) {
    g_wsSendSkippedNoClient++;
    return true;
  }
  // textAll() uses a shared buffer (one allocation), but each client needs a
  // queue slot + TCP send buffer. Guard with extra headroom per connected client.
  const uint32_t clientHeadroom = (uint32_t)clients * 512U;
  if (!hasWsHeapForPayload(payloadLen + clientHeadroom)) {
    g_wsSendSkippedLowHeap++;
    logWsSkipLowHeap(tag, payloadLen);
    return false;
  }
  try {
    socket.textAll(payload, payloadLen);
    g_wsSendOk++;
    return true;
  } catch (const std::bad_alloc&) {
    g_wsSendSkippedLowHeap++;
    logWsSkipLowHeap(tag, payloadLen);
  } catch (...) {
    g_wsSendSkippedLowHeap++;
    logWsSkipLowHeap(tag, payloadLen);
  }
  return false;
}

bool safeWsTextAll(AsyncWebSocket& socket, const String& payload, const char* tag) {
  return safeWsTextAll(socket, payload.c_str(), payload.length(), tag);
}

bool safeWsClientText(AsyncWebSocketClient* client, const String& payload, const char* tag) {
  const size_t payloadLen = payload.length();
  if (!client || payloadLen == 0) return false;
  if (!hasWsHeapForPayload(payloadLen)) {
    g_wsSendSkippedLowHeap++;
    logWsSkipLowHeap(tag, payloadLen);
    return false;
  }
  try {
    client->text(payload.c_str(), payloadLen);
    g_wsSendOk++;
    return true;
  } catch (const std::bad_alloc&) {
    g_wsSendSkippedLowHeap++;
    logWsSkipLowHeap(tag, payloadLen);
  } catch (...) {
    g_wsSendSkippedLowHeap++;
    logWsSkipLowHeap(tag, payloadLen);
  }
  return false;
}

template <typename TDoc>
bool buildWsPayloadIfSafe(const TDoc& doc, size_t clientCount, const char* tag, String& payload) {
  if (doc.overflowed()) {
    logWsBuildSkip(tag, "json_overflow", measureJson(doc));
    return false;
  }
  const size_t payloadLen = measureJson(doc);
  if (payloadLen == 0) return false;
  const uint32_t clientHeadroom = clientCount > 0 ? (uint32_t)clientCount * 512U : 0U;
  if (!hasWsHeapForPayload(payloadLen + clientHeadroom)) {
    g_wsSendSkippedLowHeap++;
    logWsSkipLowHeap(tag, payloadLen);
    return false;
  }
  payload = "";
  if (!payload.reserve(payloadLen + 1)) {
    g_wsSendSkippedLowHeap++;
    logWsBuildSkip(tag, "reserve_failed", payloadLen);
    return false;
  }
  serializeJson(doc, payload);
  if (payload.length() == 0) return false;
  return true;
}

template <typename TDoc>
bool safeWsTextAllDoc(AsyncWebSocket& socket, const TDoc& doc, const char* tag) {
  const size_t clients = socket.count();
  if (clients == 0) {
    g_wsSendSkippedNoClient++;
    return true;
  }
  String payload;
  if (!buildWsPayloadIfSafe(doc, clients, tag, payload)) return false;
  try {
    socket.textAll(payload.c_str(), payload.length());
    g_wsSendOk++;
    return true;
  } catch (const std::bad_alloc&) {
    g_wsSendSkippedLowHeap++;
    logWsSkipLowHeap(tag, payload.length());
  } catch (...) {
    g_wsSendSkippedLowHeap++;
    logWsSkipLowHeap(tag, payload.length());
  }
  return false;
}

template <typename TDoc>
bool safeWsClientTextDoc(AsyncWebSocketClient* client, const TDoc& doc, const char* tag) {
  if (!client) return false;
  String payload;
  if (!buildWsPayloadIfSafe(doc, 1, tag, payload)) return false;
  try {
    client->text(payload.c_str(), payload.length());
    g_wsSendOk++;
    return true;
  } catch (const std::bad_alloc&) {
    g_wsSendSkippedLowHeap++;
    logWsSkipLowHeap(tag, payload.length());
  } catch (...) {
    g_wsSendSkippedLowHeap++;
    logWsSkipLowHeap(tag, payload.length());
  }
  return false;
}

void logAuthFailRateLimited(const String& url, IPAddress ip) {
  g_authFailTotal++;
  const unsigned long now = millis();
  if (g_lastAuthFailLogMs != 0 && now - g_lastAuthFailLogMs < 5000UL) {
    g_authFailSuppressed++;
    return;
  }

  const uint32_t suppressed = g_authFailSuppressed;
  g_authFailSuppressed = 0;
  g_lastAuthFailLogMs = now;
  if (suppressed > 0) {
    Serial.printf("AUTH FAIL %s from %s (suppressed %lu)\n",
                  url.c_str(),
                  ip.toString().c_str(),
                  (unsigned long)suppressed);
  } else {
    Serial.printf("AUTH FAIL %s from %s\n",
                  url.c_str(),
                  ip.toString().c_str());
  }
}

void logStatusUsageRateLimited(uint32_t liteCount, uint32_t fullCount, uint16_t wsClients) {
  const unsigned long now = millis();
  if (g_lastStatusUsageLogMs != 0 && now - g_lastStatusUsageLogMs < 10000UL) return;
  g_lastStatusUsageLogMs = now;
  Serial.printf("[STATUS] lite/full usage lite=%lu full=%lu ws=%u\n",
                (unsigned long)liteCount,
                (unsigned long)fullCount,
                (unsigned)wsClients);
}

void sendSchemaError(AsyncWebServerRequest* request, const String& detail) {
  DynamicJsonDocument doc(256);
  doc["status"] = "invalid";
  doc["error"] = "schema_error";
  JsonArray details = doc.createNestedArray("details");
  if (detail.length() > 0) details.add(detail);
  sendJson(request, doc, 422);
}

void sendControlResult(AsyncWebServerRequest* request, const ControlResult& result, const String& action) {
  DynamicJsonDocument doc(256);
  doc["status"] = result.status ? result.status : "error";
  doc["ok"] = result.ok;
  doc["applied"] = result.applied;
  if (action.length() > 0) doc["action"] = action;
  if (result.error && result.error[0] != '\0') doc["error"] = result.error;
  sendJson(request, doc, result.httpCode);
}

String normalizeAction(const String& in) {
  String out = in;
  out.toLowerCase();
  return out;
}

const char* contentTypeForPath(const String& path) {
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".css")) return "text/css";
  if (path.endsWith(".js")) return "application/javascript";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".ico")) return "image/x-icon";
  if (path.endsWith(".png")) return "image/png";
  if (path.endsWith(".svg")) return "image/svg+xml";
  return "text/plain";
}
} // namespace

WebServerManager::WebServerManager(ConfigManager* cfg_) : cfg(cfg_) {}

void WebServerManager::begin() {
  const uint16_t webPort =
    (cfg && cfg->deviceConfig.webPort >= 1 && cfg->deviceConfig.webPort <= 65535)
      ? (uint16_t)cfg->deviceConfig.webPort
      : 80u;
  server.~AsyncWebServer();
  new (&server) AsyncWebServer(webPort);
  fsMounted = LittleFS.begin();
  if (!fsMounted) {
    Serial.println("WebServer: LittleFS not mounted (begin() returned false)");
  } else {
    Serial.println("WebServer: LittleFS is mounted");
  }
  setupRoutes();
  server.begin();
  Serial.printf("Web server started on port %u\n", (unsigned)webPort);
}

bool WebServerManager::isAuthorized(AsyncWebServerRequest* request) const {
  if (!cfg) return true;
  if (!cfg->securityConfig.enabled) return true;

  // Allow configuring security only when no token is set yet (initial setup).
  const String url = request ? request->url() : "";
  if (url == "/api/security" && cfg->securityConfig.apiToken.length() == 0) return true;

  if (cfg->securityConfig.apiToken.length() == 0) return false;

  String token;
  bool tokenTooLong = false;
  extractAuthToken(request, false, token, tokenTooLong);
  bool ok = token.length() > 0 && token == cfg->securityConfig.apiToken;
  if (!ok) {
    IPAddress ip;
    if (request->client()) ip = request->client()->remoteIP();
    logAuthFailRateLimited(request->url(), ip);
  }
  return ok;
}

bool WebServerManager::isWebSocketAuthorized(AsyncWebServerRequest* request) const {
  if (!cfg) return true;
  if (!cfg->securityConfig.enabled) return true;
  if (!request) return false;
  if (cfg->securityConfig.apiToken.length() == 0) return false;

  String token;
  bool tokenTooLong = false;
  extractAuthToken(request, true, token, tokenTooLong);

  bool ok = token.length() > 0 && token == cfg->securityConfig.apiToken;
  if (!ok) {
    IPAddress ip;
    if (request->client()) ip = request->client()->remoteIP();
    logAuthFailRateLimited(request->url(), ip);
  }
  return ok;
}

void WebServerManager::sendUnauthorized(AsyncWebServerRequest* request) const {
  request->send(401, "application/json", "{\"status\":\"unauthorized\"}");
}

void WebServerManager::setupRoutes() {
  server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    const uint32_t t0Us = micros();
    stats.apiReqCount++;
    stats.statusReqCount++;
    stats.lastApiReqMs = millis();
    stats.lastStatusReqMs = millis();
    logStatusUsageRateLimited(stats.statusLiteReqCount, stats.statusReqCount, (uint16_t)ws.count());
    StaticJsonDocument<5120> doc;
    JsonObject root = doc.to<JsonObject>();
    if (statusCb) {
      statusCb(root);
    } else {
      stats.statusErrors++;
      root["ok"] = false;
      root["error"] = "no_status_provider";
    }
    const uint32_t dtUs = micros() - t0Us;
    stats.lastStatusDurationUs = dtUs;
    if (dtUs > stats.maxStatusDurationUs) stats.maxStatusDurationUs = dtUs;
    if (dtUs > 15000) {
      stats.statusSlowCount++;
      Serial.printf("[HTTP] /api/status slow dt=%luus heap=%u minHeap=%u ws=%u req=%lu\n",
                    (unsigned long)dtUs,
                    (unsigned)ESP.getFreeHeap(),
                    (unsigned)ESP.getMinFreeHeap(),
                    (unsigned)ws.count(),
                    (unsigned long)stats.statusReqCount);
    }
    {
      // Use String (single malloc) instead of AsyncResponseStream (cbuf+realloc loop)
      // to avoid abort() from CONFIG_HEAP_ABORT_WHEN_ALLOCATION_FAILS during heap pressure.
      const size_t jsonLen = measureJson(doc);
      if (ESP.getMaxAllocHeap() < (uint32_t)jsonLen * 2 + 4096U) {
        Serial.printf("[HTTP] /api/status heap_guard jsonLen=%u maxAlloc=%u\n",
                      (unsigned)jsonLen, (unsigned)ESP.getMaxAllocHeap());
        request->send(503, "application/json", "{\"ok\":false,\"error\":\"low_heap\"}");
        return;
      }
      request->send(200, "application/json", serializeJsonString(doc));
    }
  });

  server.on("/api/status-lite", HTTP_GET, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    stats.apiReqCount++;
    stats.statusLiteReqCount++;
    stats.lastApiReqMs = millis();
    logStatusUsageRateLimited(stats.statusLiteReqCount, stats.statusReqCount, (uint16_t)ws.count());
    StaticJsonDocument<768> doc;
    JsonObject root = doc.to<JsonObject>();
    if (statusLiteCb) {
      statusLiteCb(root);
    } else if (statusCb) {
      statusCb(root);
    } else {
      stats.statusErrors++;
      root["ok"] = false;
      root["error"] = "no_status_provider";
    }
    {
      const size_t jsonLen = measureJson(doc);
      if (ESP.getMaxAllocHeap() < (uint32_t)jsonLen * 2 + 4096U) {
        Serial.printf("[HTTP] /api/status-lite heap_guard jsonLen=%u maxAlloc=%u\n",
                      (unsigned)jsonLen, (unsigned)ESP.getMaxAllocHeap());
        request->send(503, "application/json", "{\"ok\":false,\"error\":\"low_heap\"}");
        return;
      }
      request->send(200, "application/json", serializeJsonString(doc));
    }
  });

  server.on("/api/config", HTTP_GET, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    String err;
    if (!cfg->ensureDefaultConfigExists(&err)) {
      request->send(500, "application/json", String("{\"status\":\"error\",\"error\":\"") + err + "\"}");
      return;
    }
    if (!LittleFS.exists(CONFIG_PATH)) {
      request->send(500, "application/json", "{\"status\":\"error\",\"error\":\"config_missing\"}");
      return;
    }
    request->send(200, "application/json", cfg->toApiJson(true));
  });

  server.on("/api/config", HTTP_POST, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
  }, NULL, [this](AsyncWebServerRequest* request, uint8_t *data, size_t len, size_t index, size_t total){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    BodyBuffer* buf = appendBodyChunk(request, data, len, index, total);
    if (!buf) return;
    if (index + len != total) return;

    DynamicJsonDocument doc(CONFIG_JSON_CAPACITY);
    if (!parseJsonBody(request, doc, "api_config")) {
      request->send(400, "application/json", "{\"status\":\"bad_json\"}");
      return;
    }
    JsonVariantConst root = doc.as<JsonVariantConst>();
    JsonVariantConst wrapped = doc["config"];
    if (wrapped.is<JsonObjectConst>()) {
      root = wrapped;
    }

    const String requestUrl = request ? request->url() : "";
    if (requestUrl == "/api/config/validate") {
      String err;
      if (!cfg->validate(root, err)) {
        sendSchemaError(request, err);
        return;
      }
      request->send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }

    int requestedBrakingForce = -1;
    if (root.is<JsonObjectConst>()) {
      JsonObjectConst rootObj = root.as<JsonObjectConst>();
      JsonObjectConst motion = rootObj["motion"];
      if (!motion.isNull()) {
        JsonObjectConst advanced = motion["advanced"];
        if (!advanced.isNull()) {
          JsonObjectConst braking = advanced["braking"];
          if (!braking.isNull() && braking.containsKey("force")) {
            requestedBrakingForce = braking["force"] | -1;
          }
        }
      }
    }
    if (requestedBrakingForce >= 0) {
#if defined(GATE_DEBUG_CONFIG)
      Serial.printf("[config] motion.advanced.braking.force=%d\n", requestedBrakingForce);
#endif
    }

    const int previousWebPort = cfg ? cfg->deviceConfig.webPort : 80;
    ConfigManager updated = *cfg;
    // FIX: Clear saveAllowedCb on the copy so the web API save always
    // writes immediately.  The deferred-save mechanism is meant for
    // runtime position saves, not for user-initiated config changes.
    // Without this, if the gate happens to be moving the save is
    // silently deferred on this LOCAL copy (which is destroyed after
    // the request), causing the new config to be lost.
    updated.setSaveAllowedCallback(nullptr);
    String err;
    if (!updated.validate(root, err)) {
      sendSchemaError(request, err);
      return;
    }
    if (!updated.fromJsonVariant(root)) {
      request->send(400, "application/json", "{\"status\":\"bad_payload\"}");
      return;
    }
#if defined(GATE_DEBUG_CONFIG)
    Serial.printf("[CFG_SAVE] web POST: gate.maxDistance=%.3f\n", updated.gateConfig.maxDistance);
#endif
    if (!updated.save(nullptr)) {
      request->send(500, "application/json", "{\"status\":\"error\",\"error\":\"fs_write_failed\"}");
      return;
    }
#if defined(GATE_DEBUG_CONFIG)
    Serial.printf("[CFG_SAVE] web POST: save OK, scheduling apply\n");
#endif
    cfg->adoptPersistenceMetaFrom(updated);
    if (updated.deviceConfig.webPort != previousWebPort) {
      char response[128];
      snprintf(response,
               sizeof(response),
               "{\"status\":\"ok\",\"apply\":\"restart\",\"restartMs\":1500,\"redirectPort\":%d}",
               updated.deviceConfig.webPort);
      scheduleRestart(1500);
      request->send(200, "application/json", response);
      return;
    }
    scheduleRuntimeConfigApply();
    request->send(200, "application/json", "{\"status\":\"ok\",\"apply\":\"scheduled\"}");
  });

  server.on("/api/config/validate", HTTP_POST, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
  }, NULL, [this](AsyncWebServerRequest* request, uint8_t *data, size_t len, size_t index, size_t total){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    BodyBuffer* buf = appendBodyChunk(request, data, len, index, total);
    if (!buf) return;
    if (index + len != total) return;

    String head = buf->body.length() > 80 ? buf->body.substring(0, 80) : buf->body;
#if defined(GATE_DEBUG_CONFIG)
    Serial.printf("[validate] total=%u head=%s\n", (unsigned)total, head.c_str());
#endif

    DynamicJsonDocument doc(CONFIG_JSON_CAPACITY);
    if (!parseJsonBody(request, doc, "api_config_validate")) {
      request->send(400, "application/json", "{\"status\":\"bad_json\"}");
      return;
    }
    JsonVariantConst root = doc.as<JsonVariantConst>();
    JsonVariantConst wrapped = doc["config"];
    if (wrapped.is<JsonObjectConst>()) {
      root = wrapped;
    }
    String err;
    if (!cfg->validate(root, err)) {
      sendSchemaError(request, err);
      return;
    }
    request->send(200, "application/json", "{\"status\":\"ok\"}");
  });

  server.on("/api/motion/profile", HTTP_GET, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    StaticJsonDocument<768> doc;
    JsonObject motion = doc.createNestedObject("motion");
    motion["profile"] = cfg->motionConfig.profile;
    motion["expert"] = cfg->motionConfig.expert;
    JsonObject ui = motion.createNestedObject("ui");
    ui["speedOpen"] = cfg->motionConfig.ui.speedOpen;
    ui["speedClose"] = cfg->motionConfig.ui.speedClose;
    ui["accelSmoothness"] = cfg->motionConfig.ui.accelSmoothness;
    ui["decelSmoothness"] = cfg->motionConfig.ui.decelSmoothness;
    ui["slowdownDistance"] = cfg->motionConfig.ui.slowdownDistance;
    ui["brakingFeel"] = cfg->motionConfig.ui.brakingFeel;
    const MotionAdvancedConfig& advCfg = cfg->motionConfig.advanced;
    JsonObject advanced = motion.createNestedObject("advanced");
    advanced["maxSpeedOpen"] = advCfg.maxSpeedOpen;
    advanced["maxSpeedClose"] = advCfg.maxSpeedClose;
    advanced["minSpeed"] = advCfg.minSpeed;
    JsonObject rampOpen = advanced.createNestedObject("rampOpen");
    rampOpen["mode"] = advCfg.rampOpen.mode;
    rampOpen["value"] = advCfg.rampOpen.value;
    JsonObject rampClose = advanced.createNestedObject("rampClose");
    rampClose["mode"] = advCfg.rampClose.mode;
    rampClose["value"] = advCfg.rampClose.value;
    JsonObject braking = advanced.createNestedObject("braking");
    braking["startDistanceOpen"] = advCfg.braking.startDistanceOpen;
    braking["startDistanceClose"] = advCfg.braking.startDistanceClose;
    braking["force"] = advCfg.braking.force;
    braking["mode"] = advCfg.braking.mode;
    sendJson(request, doc);
  });

  server.on("/api/motion/profile", HTTP_POST, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
  }, NULL, [this](AsyncWebServerRequest* request, uint8_t *data, size_t len, size_t index, size_t total){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    BodyBuffer* buf = appendBodyChunk(request, data, len, index, total);
    if (!buf) return;
    if (index + len != total) return;

    DynamicJsonDocument payload(CONFIG_JSON_CAPACITY);
    if (!parseJsonBody(request, payload, "api_motion_profile")) {
      request->send(400, "application/json", "{\"status\":\"bad_json\"}");
      return;
    }

    DynamicJsonDocument wrapper(CONFIG_JSON_CAPACITY);
    wrapper["version"] = CONFIG_VERSION;
    JsonObject obj = wrapper.createNestedObject("motion");
    JsonVariantConst payloadRoot = payload.as<JsonVariantConst>();
    if (!payloadRoot.is<JsonObjectConst>()) {
      request->send(400, "application/json", "{\"status\":\"bad_payload\",\"error\":\"root_not_object\"}");
      return;
    }
    JsonObjectConst incoming = payloadRoot.as<JsonObjectConst>();
    // ArduinoJson v6: iterate const pairs and clone values into our mutable document via .set().
    for (JsonPairConst kv : incoming) {
      obj[kv.key().c_str()].set(kv.value());
    }

    String err;
    ConfigManager updated = *cfg;
    if (!updated.validate(wrapper.as<JsonVariantConst>(), err)) {
      sendSchemaError(request, err);
      return;
    }
    if (!updated.fromJsonVariant(wrapper.as<JsonVariantConst>())) {
      request->send(400, "application/json", "{\"status\":\"bad_payload\"}");
      return;
    }
    if (!updated.save(nullptr)) {
      request->send(500, "application/json", "{\"status\":\"error\",\"error\":\"fs_write_failed\"}");
      return;
    }
    cfg->adoptPersistenceMetaFrom(updated);
    scheduleRuntimeConfigApply();
    request->send(200, "application/json", "{\"status\":\"ok\",\"apply\":\"scheduled\"}");
  });

  server.on("/api/motion/test", HTTP_POST, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
  }, NULL, [this](AsyncWebServerRequest* request, uint8_t *data, size_t len, size_t index, size_t total){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    BodyBuffer* buf = appendBodyChunk(request, data, len, index, total);
    if (!buf) return;
    if (index + len != total) return;

    StaticJsonDocument<128> doc;
    if (!parseJsonBody(request, doc, "api_motion_test")) {
      request->send(400, "application/json", "{\"status\":\"bad_json\"}");
      return;
    }
    const char* action = doc["action"] | "";
    if (!action || action[0] == '\0') {
      request->send(400, "application/json", "{\"status\":\"missing_action\"}");
      return;
    }
    if (!controlCb) {
      request->send(500, "application/json", "{\"status\":\"error\",\"error\":\"control_unavailable\"}");
      return;
    }
    controlCb(String(action));
    request->send(200, "application/json", "{\"status\":\"ok\"}");
  });

  server.on("/api/mqtt/status", HTTP_GET, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    StaticJsonDocument<256> doc;
    bool enabled = mqtt ? mqtt->enabled() : false;
    doc["enabled"] = enabled;
    doc["connected"] = mqtt ? mqtt->connected() : false;
    doc["state"] = mqtt ? mqtt->state() : 0;
    doc["error"] = mqtt ? mqtt->lastError() : "mqtt_not_ready";
    sendJson(request, doc);
  });

  server.on("/api/mqtt/test", HTTP_POST, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    StaticJsonDocument<256> doc;
    if (!mqtt) {
      doc["ok"] = false;
      doc["connected"] = false;
      doc["state"] = 0;
      doc["error"] = "mqtt_not_ready";
      sendJson(request, doc);
      return;
    }
    String topic;
    String msg;
    bool ok = mqtt->testPublish(topic, msg);
    doc["ok"] = ok;
    doc["topic"] = topic;
    doc["msg"] = msg;
    doc["connected"] = mqtt->connected();
    doc["state"] = mqtt->state();
    doc["error"] = mqtt->lastError();
    sendJson(request, doc);
  });

  server.on("/api/led", HTTP_GET, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    StaticJsonDocument<256> doc;
    JsonObject root = doc.to<JsonObject>();
    if (led) {
      led->fillStatus(root);
    } else {
      root["enabled"] = false;
      root["error"] = "led_not_ready";
    }
    sendJson(request, doc);
  });

  server.on("/api/led", HTTP_POST, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
  }, NULL, [this](AsyncWebServerRequest* request, uint8_t *data, size_t len, size_t index, size_t total){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    if (!led) {
      request->send(500, "application/json", "{\"status\":\"error\",\"error\":\"led_not_ready\"}");
      return;
    }
    BodyBuffer* buf = appendBodyChunk(request, data, len, index, total);
    if (!buf) return;
    if (index + len != total) return;

    StaticJsonDocument<256> doc;
    if (!parseJsonBody(request, doc, "api_led_post")) {
      request->send(400, "application/json", "{\"status\":\"bad_json\"}");
      return;
    }

    if (doc.containsKey("enabled")) {
      led->setEnabled(doc["enabled"] | false);
    }
    if (doc.containsKey("brightness")) {
      int value = doc["brightness"] | led->getBrightness();
      if (value < 0 || value > 100) {
        request->send(422, "application/json", "{\"status\":\"invalid\",\"error\":\"brightness_out_of_range\"}");
        return;
      }
      led->setBrightness(value);
    }
    if (doc.containsKey("ringStartIndex") || doc.containsKey("ringReverse")) {
      int startIndex = doc["ringStartIndex"] | led->getRingStartIndex();
      bool reverse = doc["ringReverse"] | led->getRingReverse();
      if (startIndex < -4096 || startIndex > 4096) {
        request->send(422, "application/json", "{\"status\":\"invalid\",\"error\":\"ringStartIndex_out_of_range\"}");
        return;
      }
      led->setRingOrientation(startIndex, reverse);
    }
    if (doc.containsKey("mode")) {
      const char* mode = doc["mode"] | "";
      led->setMode(mode);
    }
    if (doc.containsKey("pattern")) {
      const char* pattern = doc["pattern"] | "flash";
      unsigned long duration = doc["overrideMs"] | doc["duration"] | 800;
      led->setOverride(pattern, duration);
    }
    if (doc["test"] | false) {
      led->startTest();
    }

    StaticJsonDocument<256> out;
    out["status"] = "ok";
    JsonObject ledObj = out.createNestedObject("led");
    led->fillStatus(ledObj);
    sendJson(request, out);
  });

  server.on("/api/led/test", HTTP_POST, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    if (led) {
      led->startTest();
      request->send(200, "application/json", "{\"status\":\"ok\"}");
    } else {
      request->send(500, "application/json", "{\"status\":\"error\",\"error\":\"led_not_ready\"}");
    }
  });

  server.on("/api/remotes", HTTP_GET, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    StaticJsonDocument<1536> doc;
    doc["antiRepeatMs"] = cfg->remoteConfig.antiRepeatMs;
    doc["antiReplay"] = cfg->remoteConfig.antiReplay;
    doc["replayWindow"] = cfg->remoteConfig.replayWindow;
    JsonArray legacy = doc.createNestedArray("remotes");
    JsonArray items = doc.createNestedArray("items");
    for (const auto& r : cfg->getRemotes()) {
      legacy.add(r.serial);
      JsonObject item = items.createNestedObject();
      item["serial"] = r.serial;
      item["name"] = r.name;
      item["enabled"] = r.enabled;
      item["lastCounter"] = r.lastCounter;
      item["lastSeenMs"] = r.lastSeenMs;
    }
    sendJson(request, doc);
  });

  server.on("/api/remotes", HTTP_POST, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
  }, NULL, [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    BodyBuffer* buf = appendBodyChunk(request, data, len, index, total);
    if (!buf) return;
    if (index + len != total) return;

    StaticJsonDocument<512> doc;
    if (!parseJsonBody(request, doc, "api_remotes_post")) { request->send(400, "application/json", "{\"status\":\"bad_json\"}"); return; }
    unsigned long serial = doc["serial"] | 0;
    if (serial == 0) { request->send(400, "application/json", "{\"status\":\"missing_serial\"}"); return; }

    String name = String((const char*)(doc["name"] | ""));
    bool enabled = doc["enabled"] | true;
    String action = String((const char*)(doc["action"] | ""));
    bool upsert = doc["upsert"] | false;

     RemoteEntry existing;
     bool exists = cfg->getRemote(serial, existing);

    if (action == "update") {
      if (!exists && !upsert) {
        request->send(404, "application/json", "{\"status\":\"not_found\"}");
        return;
      }
      if (!exists && upsert) {
        if (!cfg->addRemote(serial, name)) {
          request->send(500, "application/json", "{\"status\":\"error\",\"error\":\"fs_write_failed\",\"detail\":\"add_remote_failed\"}");
          return;
        }
      }
      if (cfg->updateRemote(serial, name, enabled)) {
        request->send(200, "application/json", "{\"status\":\"ok\"}");
      } else {
        String detail = cfg->getLastRemotesSaveError();
        if (detail.length() == 0) detail = "save_failed";
        request->send(500, "application/json",
                      String("{\"status\":\"error\",\"error\":\"fs_write_failed\",\"detail\":\"") + detail + "\"}");
      }
      return;
    }

    if (action.length() > 0 && action != "add") {
      request->send(400, "application/json", "{\"status\":\"bad_payload\",\"error\":\"bad_action\"}");
      return;
    }

    if (exists) {
      request->send(409, "application/json", "{\"status\":\"exists\"}");
      return;
    }

    if (cfg->addRemote(serial, name)) {
      request->send(200, "application/json", "{\"status\":\"ok\"}");
    } else {
      String detail = cfg->getLastRemotesSaveError();
      if (detail.length() == 0) detail = "save_failed";
      request->send(500, "application/json",
                    String("{\"status\":\"error\",\"error\":\"fs_write_failed\",\"detail\":\"") + detail + "\"}");
    }
  });

  server.on("/api/remotes", HTTP_DELETE, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
  }, NULL, [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    BodyBuffer* buf = appendBodyChunk(request, data, len, index, total);
    if (!buf) return;
    if (index + len != total) return;

    StaticJsonDocument<256> doc;
    if (!parseJsonBody(request, doc, "api_remotes_delete")) { request->send(400, "application/json", "{\"status\":\"bad_json\"}"); return; }
    unsigned long serial = doc["serial"] | 0;
    if (serial == 0) { request->send(400, "application/json", "{\"status\":\"missing_serial\"}"); return; }
    RemoteEntry existing;
    bool exists = cfg->getRemote(serial, existing);
    if (!exists) {
      request->send(404, "application/json", "{\"status\":\"not_found\"}");
      return;
    }
    if (cfg->removeRemote(serial)) {
      request->send(200, "application/json", "{\"status\":\"ok\"}");
    } else {
      String detail = cfg->getLastRemotesSaveError();
      if (detail.length() == 0) detail = "save_failed";
      request->send(500, "application/json",
                    String("{\"status\":\"error\",\"error\":\"fs_write_failed\",\"detail\":\"") + detail + "\"}");
    }
  });

  server.on("/api/learn", HTTP_GET, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    StaticJsonDocument<128> doc;
    doc["enabled"] = learnMode;
    sendJson(request, doc);
  });

  server.on("/api/learn", HTTP_POST, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
  }, NULL, [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    BodyBuffer* buf = appendBodyChunk(request, data, len, index, total);
    if (!buf) return;
    if (index + len != total) return;

    StaticJsonDocument<128> doc;
    if (!parseJsonBody(request, doc, "api_learn_post")) { request->send(400, "application/json", "{\"status\":\"bad_json\"}"); return; }
    bool enable = doc["enable"] | false;
    learnMode = enable;
    if (learnCb) learnCb(enable);
    request->send(200, "application/json", "{\"status\":\"ok\"}");
  });

  server.on("/api/test_remote", HTTP_GET, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    StaticJsonDocument<256> doc;
    JsonObject root = doc.to<JsonObject>();
    if (remoteStateCb) remoteStateCb(root);
    sendJson(request, doc);
  });

  server.on("/api/test_remote", HTTP_POST, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
  }, NULL, [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    BodyBuffer* buf = appendBodyChunk(request, data, len, index, total);
    if (!buf) return;
    if (index + len != total) return;

    StaticJsonDocument<256> doc;
    if (!parseJsonBody(request, doc, "api_test_remote_post")) { request->send(400, "application/json", "{\"status\":\"bad_json\"}"); return; }
    unsigned long serial = doc["serial"] | 0;
    unsigned long encript = doc["encript"] | 0;
    bool btnT = doc["btnToggle"] | false;
    bool btnG = doc["btnGreen"] | false;
    StaticJsonDocument<256> ev;
    ev["type"] = "test_remote";
    ev["serial"] = serial;
    ev["encript"] = encript;
    ev["btnToggle"] = btnT;
    ev["btnGreen"] = btnG;
    String payload = serializeJsonString(ev);
    safeWsTextAll(ws, payload, "test_remote");

    if (testCb) testCb(serial, encript, btnT, btnG, false);
    request->send(200, "application/json", "{\"status\":\"ok\"}");
  });

  server.on("/api/fs_status", HTTP_GET, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    StaticJsonDocument<512> doc;
    bool exists = LittleFS.exists(CONFIG_PATH);
    size_t size = 0;
    if (exists) {
      File f = LittleFS.open(CONFIG_PATH, "r");
      if (f) {
        size = f.size();
        f.close();
      }
    }
    doc["mounted"] = fsMounted;
    doc["totalBytes"] = LittleFS.totalBytes();
    doc["usedBytes"] = LittleFS.usedBytes();
    doc["freeBytes"] = LittleFS.totalBytes() - LittleFS.usedBytes();
    doc["exists_config"] = exists;
    doc["config_size"] = (unsigned)size;
    doc["last_save_ms"] = cfg ? cfg->getLastSaveMs() : 0;
    doc["last_save_error"] = cfg ? cfg->getLastSaveError() : "";
    sendJson(request, doc);
  });

  server.on("/api/diagnostics", HTTP_GET, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    stats.apiReqCount++;
    stats.lastApiReqMs = millis();
    // Diagnostics can grow over time; keep generous headroom to avoid truncation.
    StaticJsonDocument<4096> doc;
    JsonObject root = doc.to<JsonObject>();
    root["uptimeMs"] = millis();
    JsonObject jsonObj = root.createNestedObject("json");
    jsonObj["lastBadMs"] = g_jsonParseDiag.lastBadMs;
    jsonObj["lastBadLen"] = (unsigned)g_jsonParseDiag.lastBadLen;
    jsonObj["lastBadTag"] = g_jsonParseDiag.lastBadTag;
    jsonObj["lastBadErr"] = g_jsonParseDiag.lastBadErr;
    jsonObj["lastBadHead"] = g_jsonParseDiag.lastBadHead;
    JsonObject ledObj = root.createNestedObject("led");
    if (led) {
      led->fillStatus(ledObj);
    } else {
      ledObj["enabled"] = false;
      ledObj["error"] = "led_not_ready";
    }
    if (diagnosticsCb) {
      diagnosticsCb(root);
    }
    sendJson(request, doc);
  });

  server.on("/api/fslist", HTTP_GET, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    StaticJsonDocument<512> doc;
    JsonArray arr = doc.createNestedArray("files");
    File root = LittleFS.open("/");
    if (!root) {
      doc["error"] = "fs_open_failed";
      String out;
      serializeJson(doc, out);
      request->send(500, "application/json", out);
      return;
    }
    const uint32_t scanStartUs = micros();
    size_t fileCount = 0;
    bool truncated = false;
    File file = root.openNextFile();
    while (file) {
      arr.add(String(file.name()));
      file.close();
       fileCount++;
       if (fileCount >= kMaxFsListEntries || (micros() - scanStartUs) > kMaxFsListScanUs) {
        truncated = true;
        break;
      }
      file = root.openNextFile();
    }
    root.close();
    if (truncated) {
      Serial.printf("[FS] /api/fslist truncated files=%u dt=%luus\n",
                    (unsigned)fileCount,
                    (unsigned long)(micros() - scanStartUs));
    }
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server.on("/api/wifi", HTTP_POST, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    request->send(501, "application/json", "{\"status\":\"not_implemented\"}");
  });

  server.on("/api/reboot", HTTP_POST, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    request->send(200, "application/json", "{\"status\":\"ok\"}");
    scheduleRestart(100);
  });

  // HTTP OTA upload — prześlij skompilowany .bin przez przeglądarkę.
  // Autoryzacja jak pozostałe endpointy API.
  server.on("/api/ota/upload", HTTP_POST,
    [this](AsyncWebServerRequest *request) {
      if (!isAuthorized(request)) { sendUnauthorized(request); return; }
      if (!cfg || !cfg->otaConfig.enabled) {
        request->send(423, "application/json", "{\"ok\":false,\"error\":\"ota_disabled\"}");
        return;
      }
      // Odpowiedź wysyłana po zakończeniu przesyłania pliku.
      // ok = true tylko gdy upload ukończony bez błędu i bez flagi failed.
      const bool ok = !otaHttpUploadFailed && !Update.hasError();
      const bool hadError = Update.hasError();
      otaHttpUploadFailed = false;  // reset po odpowiedzi
      String resp = ok
        ? "{\"ok\":true}"
        : String("{\"ok\":false,\"error\":\"") +
            (hadError ? Update.errorString() : "upload_failed") + "\"}";
      request->send(200, "application/json", resp);
      if (ok) {
        // Restart planowany poza callbackiem HTTP, żeby odpowiedź zdążyła wrócić do klienta.
        scheduleRestart(300);
      }
    },
    [this](AsyncWebServerRequest *request, const String &filename,
           size_t index, uint8_t *data, size_t len, bool final) {
      if (index == 0) {
        // Blokuj równoczesne uploady — nie dopuść do dwóch Update.begin().
        if (otaHttpUploadStarted) {
          Serial.printf("[OTA-HTTP] reject: already_in_progress aborts=%lu\n",
                        (unsigned long)(g_otaAbortCount + 1));
          g_otaAbortCount++;
          otaHttpUploadFailed = true;
          return;
        }
        // Sprawdź autoryzację przy pierwszym chunku.
        if (!isAuthorized(request)) {
          Serial.printf("[OTA-HTTP] abort: unauthorized\n");
          g_otaAbortCount++;
          otaHttpUploadFailed = true;
          otaHttpUploadStarted = false;
          Update.abort();
          return;
        }
        if (!cfg || !cfg->otaConfig.enabled) {
          Serial.printf("[OTA-HTTP] abort: ota_disabled\n");
          g_otaAbortCount++;
          otaHttpUploadFailed = true;
          otaHttpUploadStarted = false;
          Update.abort();
          return;
        }
        // Nowy upload — reset stanu.
        otaHttpUploadFailed = false;
        Serial.printf("[OTA-HTTP] Start: %s size=%u\n",
                      filename.c_str(), request->contentLength());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
          Serial.printf("[OTA-HTTP] begin error: %s\n", Update.errorString());
          otaHttpUploadFailed = true;
          return;
        }
        otaHttpUploadStarted = true;
      }
      // Pomiń dalsze przetwarzanie jeśli ten upload jest zablokowany lub nieudany.
      if (otaHttpUploadFailed) return;
      if (Update.isRunning()) {
        if (Update.write(data, len) != len) {
          // Błąd zapisu — natychmiast przerwij, nie kontynuuj i nie restartuj.
          Serial.printf("[OTA-HTTP] write error: %s – aborting\n", Update.errorString());
          g_otaAbortCount++;
          Update.abort();
          otaHttpUploadFailed = true;
          otaHttpUploadStarted = false;
          return;
        }
      }
      if (final) {
        // Ten upload jest właścicielem — resetuj stan przed odpowiedzią.
        otaHttpUploadStarted = false;
        otaHttpUploadFailed = false;
        if (Update.isRunning()) {
          if (Update.end(true)) {
            Serial.printf("[OTA-HTTP] Done: %u B\n", index + len);
          } else {
            Serial.printf("[OTA-HTTP] end error: %s\n", Update.errorString());
            otaHttpUploadFailed = true;
            g_otaAbortCount++;
          }
        }
      }
    }
  );


  server.on("/api/factory_reset", HTTP_POST, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    if (led) led->setFactoryCountdown(3);
    request->send(200, "application/json", "{\"status\":\"ok\"}");
    scheduleFactoryReset(3200);
  });

  server.on("/api/gate/calibrate", HTTP_POST, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
  }, NULL, [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    BodyBuffer* buf = appendBodyChunk(request, data, len, index, total);
    if (!buf) return;
    if (index + len != total) return;

    StaticJsonDocument<128> doc;
    if (!parseJsonBody(request, doc, "api_gate_calibrate")) {
      request->send(400, "application/json", "{\"status\":\"bad_json\"}");
      return;
    }
    const char* mode = doc["set"] | "";
    if (!mode || mode[0] == '\0') {
      request->send(400, "application/json", "{\"status\":\"missing_set\"}");
      return;
    }
    if (!gateCalibrateCb) {
      request->send(500, "application/json", "{\"status\":\"error\",\"error\":\"calibrate_not_ready\"}");
      return;
    }
    if (!gateCalibrateCb(mode)) {
      request->send(422, "application/json", "{\"status\":\"invalid\",\"error\":\"calibrate_invalid\"}");
      return;
    }
    request->send(200, "application/json", "{\"status\":\"ok\"}");
  });

  // Convenience endpoint for bench testing (no limit switches yet):
  // sets current position as ZERO using the same logic as /api/gate/calibrate {set:"zero"}.
  server.on("/api/zero", HTTP_POST, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    if (!gateCalibrateCb) {
      request->send(500, "application/json", "{\"status\":\"error\",\"error\":\"calibrate_not_ready\"}");
      return;
    }
    if (!gateCalibrateCb("zero")) {
      request->send(422, "application/json", "{\"status\":\"invalid\",\"error\":\"calibrate_failed\"}");
      return;
    }
    request->send(200, "application/json", "{\"status\":\"ok\"}");
  });

  server.on("/api/control", HTTP_POST, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
  }, NULL, [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    if (otaActiveCb && otaActiveCb()) {
      request->send(423, "application/json", "{\"status\":\"blocked\",\"error\":\"ota_active\"}");
      return;
    }
    BodyBuffer* buf = appendBodyChunk(request, data, len, index, total);
    if (!buf) return;
    if (index + len != total) return;

    StaticJsonDocument<128> doc;
    if (!parseJsonBody(request, doc, "api_control_post")) { request->send(400, "application/json", "{\"status\":\"bad_json\"}"); return; }
    String action = normalizeAction(String((const char*)(doc["action"] | doc["cmd"] | "")));
    if (action.length() == 0) {
      if (doc.containsKey("position")) {
        float pos = doc["position"] | NAN;
        if (isfinite(pos)) {
          action = String("goto:") + String(pos, 3);
        }
      } else if (doc.containsKey("target")) {
        float pos = doc["target"] | NAN;
        if (isfinite(pos)) {
          action = String("goto:") + String(pos, 3);
        }
      } else if (doc.containsKey("positionMm")) {
        long posMm = doc["positionMm"] | LONG_MIN;
        if (posMm != LONG_MIN) {
          action = String("goto_mm:") + String(posMm);
        }
      } else if (doc.containsKey("targetMm")) {
        long posMm = doc["targetMm"] | LONG_MIN;
        if (posMm != LONG_MIN) {
          action = String("goto_mm:") + String(posMm);
        }
      }
    }
    if (!controlCb || action.length() == 0) {
      request->send(422, "application/json", "{\"status\":\"invalid\",\"error\":\"missing_action_or_position\"}");
      return;
    }
    sendControlResult(request, controlCb(action), action);
  });

  server.on("/api/move", HTTP_POST, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
  }, NULL, [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    if (otaActiveCb && otaActiveCb()) {
      request->send(423, "application/json", "{\"status\":\"blocked\",\"error\":\"ota_active\"}");
      return;
    }
    BodyBuffer* buf = appendBodyChunk(request, data, len, index, total);
    if (!buf) return;
    if (index + len != total) return;

    StaticJsonDocument<160> doc;
    if (!parseJsonBody(request, doc, "api_move_post")) {
      request->send(400, "application/json", "{\"status\":\"bad_json\"}");
      return;
    }

    String action;
    if (doc.containsKey("position")) {
      float pos = doc["position"] | NAN;
      if (isfinite(pos)) action = String("goto:") + String(pos, 3);
    } else if (doc.containsKey("target")) {
      float pos = doc["target"] | NAN;
      if (isfinite(pos)) action = String("goto:") + String(pos, 3);
    } else if (doc.containsKey("positionMm")) {
      long posMm = doc["positionMm"] | LONG_MIN;
      if (posMm != LONG_MIN) action = String("goto_mm:") + String(posMm);
    } else if (doc.containsKey("targetMm")) {
      long posMm = doc["targetMm"] | LONG_MIN;
      if (posMm != LONG_MIN) action = String("goto_mm:") + String(posMm);
    }

    if (!controlCb || action.length() == 0) {
      request->send(422, "application/json", "{\"status\":\"invalid\",\"error\":\"missing_position\"}");
      return;
    }

    sendControlResult(request, controlCb(action), action);
  });

  server.on("/control", HTTP_GET, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    if (request->hasParam("command")) {
      String action = normalizeAction(request->getParam("command")->value());
      if (controlCb && action.length() > 0) {
        sendControlResult(request, controlCb(action), action);
        return;
      }
    }
    request->send(422, "application/json", "{\"status\":\"invalid\",\"error\":\"missing_action\"}");
  });

  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request){
    if (LittleFS.exists("/favicon.ico")) {
      request->send(LittleFS, "/favicon.ico", "image/x-icon");
      return;
    }
    if (LittleFS.exists("/favicon.ico.gz")) {
      AsyncWebServerResponse* response = request->beginResponse(LittleFS, "/favicon.ico.gz", "image/x-icon");
      response->addHeader("Content-Encoding", "gzip");
      request->send(response);
      return;
    }
    request->send(404, "text/plain", "Not found");
  });

  server.onNotFound([](AsyncWebServerRequest *request){
    String url = request->url();
    if (url.startsWith("/api") || url.startsWith("/ws") || isProtectedStaticPath(url)) {
      request->send(404, "text/plain", "Not found");
      return;
    }
    if (url.endsWith("/")) {
      url += "index.html";
    }
    if (LittleFS.exists(url)) {
      request->send(LittleFS, url, contentTypeForPath(url));
      return;
    }
    String gz = url + ".gz";
    if (LittleFS.exists(gz)) {
      AsyncWebServerResponse* response = request->beginResponse(LittleFS, gz, contentTypeForPath(url));
      response->addHeader("Content-Encoding", "gzip");
      request->send(response);
      return;
    }
    request->send(404, "text/plain", "Not found");
  });

  ws.onEvent([this](AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len){
    (void)server;
    (void)data;
    (void)len;
    if (type == WS_EVT_CONNECT) {
      AsyncWebServerRequest* request = reinterpret_cast<AsyncWebServerRequest*>(arg);
      if (!isWebSocketAuthorized(request)) {
        client->close();
        return;
      }
      stats.lastWsConnectMs = millis();
      noteWsConnect(client);
      if (!statusLiteCb && !statusCb) return;
      StaticJsonDocument<1792> doc;
      doc["type"] = statusLiteCb ? "status_lite" : "status";
      JsonObject dataObj = doc.createNestedObject("data");
      if (statusLiteCb) statusLiteCb(dataObj);
      else statusCb(dataObj);
      safeWsClientTextDoc(client, doc, "connect_status");
      return;
    }
    if (type == WS_EVT_DISCONNECT) {
      stats.lastWsDisconnectMs = millis();
      noteWsDisconnect(client);
    }
  });

  // Reject new WS connections at HTTP level when at client limit (count >= 4).
  // HTTP 503 is cheaper than accepting the WS handshake and then sending close(1008),
  // which requires a full TCP round-trip and burdens async_tcp.
  // Browser WebSocket sees a non-101 response → onclose(1006, wasClean=false) → rapidFailure backoff.
  ws.setFilter([this](AsyncWebServerRequest* request) -> bool {
    (void)request;
    return ws.count() < 4;
  });
  server.addHandler(&ws);

  // Fallback: catches WS upgrade requests that the WS handler's filter rejected (count >= 4).
  server.on("/ws", HTTP_GET, [this](AsyncWebServerRequest* request) {
    stats.wsRejected++;
    static uint32_t lastRejectLogMs = 0;
    static uint32_t suppressedRejects = 0;
    const uint32_t nowMs = (uint32_t)millis();
    if (nowMs - lastRejectLogMs >= 5000U) {
      Serial.printf("[WS] reject HTTP-503 count=%u total=%lu suppressed=%u\n",
                    (unsigned)ws.count(), (unsigned long)stats.wsRejected, (unsigned)suppressedRejects);
      lastRejectLogMs = nowMs;
      suppressedRejects = 0;
    } else {
      suppressedRejects++;
      stats.wsRejectLogSuppressed++;
    }
    request->send(503, "application/json", "{\"ok\":false,\"error\":\"max_clients\"}");
  });

  server.serveStatic("/", LittleFS, "/")
    .setDefaultFile("index.html")
    .setCacheControl("max-age=300, public")
    .setFilter([](AsyncWebServerRequest* request) {
      String url = request->url();
      return !url.startsWith("/api") && !url.startsWith("/ws") && !isProtectedStaticPath(url);
    });
}

void WebServerManager::setLearnCallback(LearnCb cb) {
  learnCb = cb;
}

void WebServerManager::setTestCallback(TestCb cb) {
  testCb = cb;
}

void WebServerManager::setControlCallback(ControlCb cb) {
  controlCb = cb;
}

void WebServerManager::setGateCalibrateCallback(GateCalibrateCb cb) {
  gateCalibrateCb = cb;
}

void WebServerManager::setStatusCallback(StatusCb cb) {
  statusCb = cb;
}

void WebServerManager::setStatusLiteCallback(StatusLiteCb cb) {
  statusLiteCb = cb;
}

void WebServerManager::setDiagnosticsCallback(DiagnosticsCb cb) {
  diagnosticsCb = cb;
}

void WebServerManager::setRemoteStateCallback(RemoteStateCb cb) {
  remoteStateCb = cb;
}

void WebServerManager::setMqttManager(MqttManager* mqtt_) {
  mqtt = mqtt_;
}

void WebServerManager::setLedController(LedController* led_) {
  led = led_;
}

void WebServerManager::setMotorController(MotorController* motor_) {
  motor = motor_;
}

void WebServerManager::setLearnState(bool enabled) {
  learnMode = enabled;
}

void WebServerManager::broadcastJson(const String &json) {
  safeWsTextAll(ws, json, "json");
}

void WebServerManager::broadcastJson(const char* json) {
  if (!json) return;
  safeWsTextAll(ws, json, strlen(json), "json");
}

void WebServerManager::broadcastStatus() {
  if (!statusLiteCb && !statusCb) return;
  if (ws.count() == 0) return;
  StaticJsonDocument<1792> doc;
  doc["type"] = statusLiteCb ? "status_lite" : "status";
  JsonObject data = doc.createNestedObject("data");
  if (statusLiteCb) statusLiteCb(data);
  else statusCb(data);
  if (data.containsKey("uptimeMs")) {
    unsigned long uptime = data["uptimeMs"] | 0UL;
    data["uptimeMs"] = (uptime / 1000UL) * 1000UL;
  }
  safeWsTextAllDoc(ws, doc, "status");
}

void WebServerManager::broadcastEvent(const char* level, const char* message) {
  if (ws.count() == 0) return;
  StaticJsonDocument<256> doc;
  doc["type"] = "event";
  doc["level"] = level;
  doc["message"] = message;
  safeWsTextAllDoc(ws, doc, "event");
}

void WebServerManager::maintenance() {
  stats.lastMaintenanceMs = millis();
  // Explicit limit: ESP32 default is 8, which allows too many concurrent clients.
  // >4 clients causes heap exhaustion + async_tcp WDT stalls under load.
  ws.cleanupClients(4);
  // Jeśli klient zerwał połączenie w trakcie uploadu (brak final chunk),
  // Update.isRunning() wróci do false – czyść wtedy flagę, żeby nie blokować kolejnych OTA.
  if (otaHttpUploadStarted && !Update.isRunning()) {
    Serial.printf("[OTA-HTTP] reset: abandoned upload detected\n");
    otaHttpUploadStarted = false;
    otaHttpUploadFailed  = false;
  }

  // FIX A4: Remove stale BodyBuffers whose HTTP connections were dropped
  // before the body handler completed (no onDisconnect for raw HTTP in
  // ESPAsyncWebServer).  Buffers older than 8 s or when the map exceeds 8
  // entries are freed unconditionally to prevent heap exhaustion under load.
  const unsigned long now = millis();
  constexpr unsigned long kBodyBufStaleMs = 8000;
  constexpr size_t kBodyBufMaxEntries = 8;
  for (auto it = g_bodyBuffers.begin(); it != g_bodyBuffers.end(); ) {
    const unsigned long age = (it->second && it->second->startMs)
                                ? (now - it->second->startMs)
                                : ULONG_MAX;
    if (age > kBodyBufStaleMs || g_bodyBuffers.size() > kBodyBufMaxEntries) {
      delete it->second;
      it = g_bodyBuffers.erase(it);
    } else {
      ++it;
    }
  }
}

WebRuntimeStats WebServerManager::runtimeStats() const {
  WebRuntimeStats out = stats;
  out.wsClients = (uint16_t)ws.count();
  out.wsSendOk = g_wsSendOk;
  out.wsSendSkipped = g_wsSendSkippedLowHeap;
  out.wsSendSkippedNoClient = g_wsSendSkippedNoClient;
  out.authFails = g_authFailTotal;
  out.bodyBufActive = (uint16_t)g_bodyBuffers.size();
  out.otaAborts = g_otaAbortCount;
  return out;
}

void WebServerManager::appendWsClientDiagnostics(JsonObject& out) const {
  struct PeerSnapshot {
    uint32_t clientId = 0;
    uint32_t ipKey = 0;
    uint32_t connectedAtMs = 0;
  };
  struct IpCountSnapshot {
    uint32_t ipKey = 0;
    uint16_t count = 0;
  };

  PeerSnapshot peerSnapshot[6];
  IpCountSnapshot ipSnapshot[6];
  size_t peerCount = 0;
  size_t ipCount = 0;
  uint32_t lastConnectIpKey = 0;
  uint32_t lastDisconnectIpKey = 0;
  uint32_t lastConnectClientId = 0;
  uint32_t lastDisconnectClientId = 0;

  portENTER_CRITICAL(&g_wsPeersMux);
  lastConnectIpKey = g_lastWsConnectIpKey;
  lastDisconnectIpKey = g_lastWsDisconnectIpKey;
  lastConnectClientId = g_lastWsConnectClientId;
  lastDisconnectClientId = g_lastWsDisconnectClientId;
  for (auto it = g_wsActivePeers.begin(); it != g_wsActivePeers.end() && peerCount < 6; ++it) {
    peerSnapshot[peerCount].clientId = it->second.clientId;
    peerSnapshot[peerCount].ipKey = it->second.ipKey;
    peerSnapshot[peerCount].connectedAtMs = it->second.connectedAtMs;
    peerCount++;
  }
  for (auto it = g_wsPeerCountByIp.begin(); it != g_wsPeerCountByIp.end() && ipCount < 6; ++it) {
    ipSnapshot[ipCount].ipKey = it->first;
    ipSnapshot[ipCount].count = it->second;
    ipCount++;
  }
  portEXIT_CRITICAL(&g_wsPeersMux);

  const uint32_t nowMs = millis();
  out["clients"] = (uint16_t)ws.count();
  out["distinctIpCount"] = (uint16_t)ipCount;
  out["lastConnectClientId"] = lastConnectClientId;
  out["lastDisconnectClientId"] = lastDisconnectClientId;
  out["lastConnectIp"] = lastConnectIpKey ? ipKeyToString(lastConnectIpKey) : "";
  out["lastDisconnectIp"] = lastDisconnectIpKey ? ipKeyToString(lastDisconnectIpKey) : "";

  JsonArray byIp = out.createNestedArray("perIp");
  for (size_t i = 0; i < ipCount; ++i) {
    JsonObject item = byIp.createNestedObject();
    item["ip"] = ipKeyToString(ipSnapshot[i].ipKey);
    item["count"] = ipSnapshot[i].count;
  }

  JsonArray active = out.createNestedArray("activePeers");
  for (size_t i = 0; i < peerCount; ++i) {
    JsonObject item = active.createNestedObject();
    item["clientId"] = peerSnapshot[i].clientId;
    item["ip"] = ipKeyToString(peerSnapshot[i].ipKey);
    item["connectedAtMs"] = peerSnapshot[i].connectedAtMs;
    item["ageMs"] = (peerSnapshot[i].connectedAtMs == 0 || nowMs < peerSnapshot[i].connectedAtMs)
                      ? -1L
                      : (long)(nowMs - peerSnapshot[i].connectedAtMs);
  }
}
