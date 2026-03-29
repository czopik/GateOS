#include "web_server.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <map>
#include "mqtt_manager.h"
#include "calibration_manager.h"
#include "led_controller.h"
#include "motor_controller.h"

extern void scheduleRestart(uint32_t delayMs);
extern void scheduleFactoryReset(uint32_t delayMs);

void WebServerManager::setOtaActiveCallback(OtaActiveCb cb) { otaActiveCb = cb; }

namespace {
enum class BodyAppendResult {
  Ok = 0,
  BadOffset,
  BadSize
};

struct BodyBuffer {
  String body;
  size_t total = 0;
  bool badOffset = false;
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
  if (total > 0) buf->body.reserve(total + 1);
  g_bodyBuffers[request] = buf;
  return buf;
}

BodyAppendResult appendBody(BodyBuffer* buf, const uint8_t* data, size_t len, size_t index, size_t total) {
  if (!buf) return BodyAppendResult::BadSize;
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
  // Avoid String::concat(pointer, len) overload ambiguity across cores; append byte-by-byte.
  buf->body.reserve(buf->body.length() + len + 1);
  for (size_t i = 0; i < len; ++i) {
    buf->body += static_cast<char>(data[i]);
  }
  return BodyAppendResult::Ok;
}

BodyBuffer* appendBodyChunk(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
  BodyBuffer* buf = getBodyBuffer(request, total);
  BodyAppendResult res = appendBody(buf, data, len, index, total);
  if (res != BodyAppendResult::Ok) {
    if (res == BodyAppendResult::BadOffset) {
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

void sendSchemaError(AsyncWebServerRequest* request, const String& detail) {
  DynamicJsonDocument doc(256);
  doc["status"] = "invalid";
  doc["error"] = "schema_error";
  JsonArray details = doc.createNestedArray("details");
  if (detail.length() > 0) details.add(detail);
  sendJson(request, doc, 422);
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
  fsMounted = LittleFS.begin();
  if (!fsMounted) {
    Serial.println("WebServer: LittleFS not mounted (begin() returned false)");
  } else {
    Serial.println("WebServer: LittleFS is mounted");
  }
  setupRoutes();
  server.begin();
  Serial.println("Web server started");
}

bool WebServerManager::isAuthorized(AsyncWebServerRequest* request) const {
  if (!cfg) return true;
  if (!cfg->securityConfig.enabled) return true;

  // Allow configuring security even when token is empty.
  const String url = request ? request->url() : "";
  if (url == "/api/security") return true;

  if (cfg->securityConfig.apiToken.length() == 0) return false;

  String token;
  if (request->hasHeader("X-Api-Key")) {
    token = request->getHeader("X-Api-Key")->value();
  }
  if (token.length() == 0 && request->hasHeader("X-API-Token")) {
    token = request->getHeader("X-API-Token")->value();
  }
  if (token.length() == 0 && request->hasParam("token")) {
    token = request->getParam("token")->value();
  }
  bool ok = token.length() > 0 && token == cfg->securityConfig.apiToken;
  if (!ok) {
    IPAddress ip;
    if (request->client()) ip = request->client()->remoteIP();
    Serial.printf("AUTH FAIL %s from %s\n",
                  request->url().c_str(),
                  ip.toString().c_str());
  }
  return ok;
}

void WebServerManager::sendUnauthorized(AsyncWebServerRequest* request) const {
  request->send(401, "application/json", "{\"status\":\"unauthorized\"}");
}

void WebServerManager::setupRoutes() {
  server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest *request){
    const uint32_t t0Us = micros();
    stats.statusReqCount++;
    stats.lastStatusReqMs = millis();
    StaticJsonDocument<4096> doc;
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
      Serial.printf("[HTTP] /api/status slow dt=%luus heap=%u minHeap=%u ws=%u req=%lu\n",
                    (unsigned long)dtUs,
                    (unsigned)ESP.getFreeHeap(),
                    (unsigned)ESP.getMinFreeHeap(),
                    (unsigned)ws.count(),
                    (unsigned long)stats.statusReqCount);
    }
    sendJson(request, doc);
  });

  server.on("/api/status-lite", HTTP_GET, [this](AsyncWebServerRequest *request){
    stats.statusLiteReqCount++;
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
    sendJson(request, doc);
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
    request->send(200, "application/json", cfg->toJson());
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
      Serial.printf("[config] motion.advanced.braking.force=%d\n", requestedBrakingForce);
    }

    ConfigManager updated = *cfg;
    String err;
    if (!updated.validate(root, err)) {
      sendSchemaError(request, err);
      return;
    }
    if (!updated.fromJsonVariant(root)) {
      request->send(400, "application/json", "{\"status\":\"bad_payload\"}");
      return;
    }
    if (!updated.save(nullptr)) {
      request->send(500, "application/json", "{\"status\":\"error\",\"error\":\"fs_write_failed\"}");
      return;
    }
    *cfg = updated;
    if (mqtt) {
      mqtt->applyConfig(cfg->mqttConfig);
    }
    if (led) {
      led->applyConfig(cfg->ledConfig);
      led->setMqttEnabled(cfg->mqttConfig.enabled);
    }
    if (motor) {
      motor->setMotionProfile(cfg->motionProfile());
    }
    request->send(200, "application/json", "{\"status\":\"ok\"}");
  });

  server.on("/api/config/validate", HTTP_POST, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
  }, NULL, [this](AsyncWebServerRequest* request, uint8_t *data, size_t len, size_t index, size_t total){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    BodyBuffer* buf = appendBodyChunk(request, data, len, index, total);
    if (!buf) return;
    if (index + len != total) return;

    String head = buf->body.length() > 80 ? buf->body.substring(0, 80) : buf->body;
    Serial.printf("[validate] total=%u head=%s\n", (unsigned)total, head.c_str());

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
    *cfg = updated;
    if (motor) {
      motor->setMotionProfile(cfg->motionProfile());
    }
    request->send(200, "application/json", "{\"status\":\"ok\"}");
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

  server.on("/api/calibration/status", HTTP_GET, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    StaticJsonDocument<768> doc;
    if (calibration) {
      JsonObject root = doc.to<JsonObject>();
      calibration->fillStatus(root);
    } else {
      doc["running"] = false;
      doc["step"] = "unavailable";
      doc["progress"] = 0;
      doc["message"] = "";
      doc["error"] = "calibration_not_ready";
    }
    sendJson(request, doc);
  });

  server.on("/api/calibration/manual/status", HTTP_GET, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    // CALIB_FIX: manual API alias for calibration status contract.
    StaticJsonDocument<768> doc;
    if (calibration) {
      JsonObject root = doc.to<JsonObject>();
      calibration->fillStatus(root);
    } else {
      doc["enabled"] = false;
      doc["active"] = false;
      doc["step"] = "unavailable";
      doc["error"] = "calibration_not_ready";
    }
    sendJson(request, doc);
  });

  server.on("/api/calibration/start", HTTP_POST, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    StaticJsonDocument<256> doc;
    if (!calibration) {
      doc["status"] = "error";
      doc["error"] = "calibration_not_ready";
      sendJson(request, doc, 500);
      return;
    }
    if (!calibration->start()) {
      doc["status"] = "error";
      doc["error"] = "already_running";
      sendJson(request, doc, 409);
      return;
    }
    doc["status"] = "ok";
    sendJson(request, doc);
  });

  server.on("/api/calibration/manual/start", HTTP_POST, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    // CALIB_FIX: manual API alias for start.
    StaticJsonDocument<256> doc;
    if (!calibration) {
      doc["status"] = "error";
      doc["error"] = "calibration_not_ready";
      sendJson(request, doc, 500);
      return;
    }
    if (!calibration->start()) {
      doc["status"] = "error";
      doc["error"] = "already_running";
      sendJson(request, doc, 409);
      return;
    }
    doc["status"] = "ok";
    sendJson(request, doc);
  });

  server.on("/api/calibration/stop", HTTP_POST, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    if (calibration) {
      calibration->stop();
    }
    request->send(200, "application/json", "{\"status\":\"ok\"}");
  });

  server.on("/api/calibration/manual/cancel", HTTP_POST, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    // CALIB_FIX: manual API alias for cancel.
    if (calibration) {
      calibration->stop();
    }
    request->send(200, "application/json", "{\"status\":\"ok\"}");
  });

  server.on("/api/calibration/confirm_dir", HTTP_POST, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    if (!calibration) {
      request->send(500, "application/json", "{\"status\":\"error\",\"error\":\"calibration_not_ready\"}");
      return;
    }
  }, NULL, [this](AsyncWebServerRequest* request, uint8_t *data, size_t len, size_t index, size_t total){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    bool invert = false;
    if (total > 0) {
      BodyBuffer* buf = appendBodyChunk(request, data, len, index, total);
      if (!buf) return;
      if (index + len != total) return;

      StaticJsonDocument<128> doc;
      if (!parseJsonBody(request, doc, "api_calibration_confirm_dir")) {
        request->send(400, "application/json", "{\"status\":\"bad_json\"}");
        return;
      }
      invert = doc["invert"] | false;
    } else if (request->hasParam("invert")) {
      invert = request->getParam("invert")->value().toInt() != 0;
    }

    if (!calibration->confirmDirection(invert)) {
      request->send(409, "application/json", "{\"status\":\"error\",\"error\":\"not_waiting\"}");
      return;
    }
    request->send(200, "application/json", "{\"status\":\"ok\"}");
  });

  server.on("/api/calibration/apply", HTTP_POST, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    StaticJsonDocument<256> doc;
    if (!calibration) {
      doc["status"] = "error";
      doc["error"] = "calibration_not_ready";
      sendJson(request, doc, 500);
      return;
    }
    String err;
    if (!calibration->apply(&err)) {
      doc["status"] = "error";
      doc["error"] = err.length() ? err : "apply_failed";
      sendJson(request, doc, 500);
      return;
    }
    doc["status"] = "ok";
    sendJson(request, doc);
    scheduleRestart(200);
  });

  server.on("/api/calibration/manual/apply", HTTP_POST, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    // CALIB_FIX: manual API alias for apply.
    StaticJsonDocument<256> doc;
    if (!calibration) {
      doc["status"] = "error";
      doc["error"] = "calibration_not_ready";
      sendJson(request, doc, 500);
      return;
    }
    String err;
    if (!calibration->apply(&err)) {
      doc["status"] = "error";
      doc["error"] = err.length() ? err : "apply_failed";
      sendJson(request, doc, 409);
      return;
    }
    doc["status"] = "ok";
    sendJson(request, doc);
    scheduleRestart(200);
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
    char payload[256];
    serializeJson(ev, payload, sizeof(payload));
    ws.textAll(payload);

    if (testCb) testCb(serial, encript, btnT, btnG, false);
    request->send(200, "application/json", "{\"status\":\"ok\"}");
  });

  server.on("/api/fs_status", HTTP_GET, [this](AsyncWebServerRequest *request){
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

  server.on("/api/fslist", HTTP_GET, [](AsyncWebServerRequest *request){
    StaticJsonDocument<512> doc;
    JsonArray arr = doc.createNestedArray("files");
    File root = LittleFS.open("/");
    File file = root.openNextFile();
    while (file) {
      arr.add(String(file.name()));
      file = root.openNextFile();
    }
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server.on("/api/wifi", HTTP_POST, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", "OK");
  });

  server.on("/api/reboot", HTTP_POST, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    request->send(200, "application/json", "{\"status\":\"ok\"}");
    scheduleRestart(100);
  });

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
    if (controlCb && action.length() > 0) controlCb(action);
    request->send(200, "application/json", "{\"status\":\"ok\"}");
  });

  server.on("/control", HTTP_GET, [this](AsyncWebServerRequest *request){
    if (!isAuthorized(request)) { sendUnauthorized(request); return; }
    if (request->hasParam("command")) {
      String action = normalizeAction(request->getParam("command")->value());
      if (controlCb && action.length() > 0) controlCb(action);
    }
    request->send(200, "application/json", "{\"status\":\"ok\"}");
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
    if (url.startsWith("/api") || url.startsWith("/ws")) {
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
    if (type == WS_EVT_CONNECT) {
      if (!statusCb) return;
      StaticJsonDocument<2048> doc;
      doc["type"] = "status";
      JsonObject dataObj = doc.createNestedObject("data");
      statusCb(dataObj);
      char payload[2048];
      serializeJson(doc, payload, sizeof(payload));
      client->text(payload);
    }
  });

  server.addHandler(&ws);

  server.serveStatic("/", LittleFS, "/")
    .setDefaultFile("index.html")
    .setCacheControl("max-age=300, public")
    .setFilter([](AsyncWebServerRequest* request) {
      String url = request->url();
      return !url.startsWith("/api") && !url.startsWith("/ws");
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

void WebServerManager::setCalibrationManager(CalibrationManager* calibration_) {
  calibration = calibration_;
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
  ws.textAll(json);
}

void WebServerManager::broadcastJson(const char* json) {
  if (!json) return;
  ws.textAll(json);
}

void WebServerManager::broadcastStatus() {
  if (!statusCb) return;
  if (ws.count() == 0) return;
  StaticJsonDocument<2048> doc;
  doc["type"] = "status";
  JsonObject data = doc.createNestedObject("data");
  statusCb(data);
  if (data.containsKey("uptimeMs")) {
    unsigned long uptime = data["uptimeMs"] | 0UL;
    data["uptimeMs"] = (uptime / 1000UL) * 1000UL;
  }
  char payload[2048];
  serializeJson(doc, payload, sizeof(payload));
  ws.textAll(payload);
}

void WebServerManager::broadcastEvent(const char* level, const char* message) {
  if (ws.count() == 0) return;
  StaticJsonDocument<256> doc;
  doc["type"] = "event";
  doc["level"] = level;
  doc["message"] = message;
  char payload[256];
  serializeJson(doc, payload, sizeof(payload));
  ws.textAll(payload);
}

void WebServerManager::maintenance() {
  ws.cleanupClients();
}

WebRuntimeStats WebServerManager::runtimeStats() const {
  WebRuntimeStats out = stats;
  out.wsClients = (uint16_t)ws.count();
  return out;
}
