#include "config_manager.h"
#include <esp_system.h>
#include <errno.h>
#include <algorithm>

namespace {
void setError(String* out, const char* value) {
  if (out) *out = value;
}

bool writeJsonFile(const char* path, JsonDocument& doc, size_t* writtenOut = nullptr) {
  File f = LittleFS.open(path, "w");
  if (!f) return false;
  const size_t written = serializeJson(doc, f);
  f.flush();
  f.close();
  if (writtenOut) *writtenOut = written;
  return written != 0;
}

bool readJsonFile(const char* path, DynamicJsonDocument& doc, String& error) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    setError(&error, "open_failed");
    return false;
  }
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    setError(&error, err.c_str());
    return false;
  }
  return true;
}

String normalizeLabel(const String& value) {
  String tmp = value;
  tmp.toLowerCase();
  tmp.trim();
  return tmp;
}

float ensurePositive(float value, float fallback) {
  if (value <= 0.0f) return fallback;
  return value;
}

int mapSpeedLabel(const String& label, int fallback) {
  String value = normalizeLabel(label);
  if (value == "slow") return std::max(20, std::min(fallback, 160));
  if (value == "normal") return std::max(40, std::min(fallback, 240));
  if (value == "fast") return std::max(60, std::min(fallback, 320));
  if (value == "dynamic") return std::max(80, std::min(fallback, 360));
  return fallback;
}

int mapMinSpeed(const String& feel) {
  String value = normalizeLabel(feel);
  if (value == "soft") return 40;
  if (value == "normal") return 60;
  if (value == "strong") return 80;
  return 50;
}

MotorConfig::RampConfig deriveRamp(const String& label, float maxDistance) {
  MotorConfig::RampConfig out;
  String value = normalizeLabel(label);
  float safeDistance = ensurePositive(maxDistance, 12.0f);
  if (value == "soft") {
    out.mode = "distance";
    out.value = safeDistance * 0.2f;
  } else if (value == "dynamic") {
    out.mode = "time";
    out.value = 420.0f;
  } else {
    out.mode = "distance";
    out.value = safeDistance * 0.12f;
  }
  if (out.mode == "distance" && out.value > safeDistance) {
    out.value = safeDistance;
  }
  if (out.value < 50.0f && out.mode == "time") {
    out.value = 50.0f;
  }
  if (out.value < 0.1f && out.mode == "distance") {
    out.value = 0.1f;
  }
  return out;
}

MotorConfig::BrakingConfig deriveBraking(const String& label, float maxDistance) {
  MotorConfig::BrakingConfig out;
  String value = normalizeLabel(label);
  float safeDistance = ensurePositive(maxDistance, 12.0f);
  if (value == "close") {
    out.startDistanceOpen = safeDistance * 0.05f;
    out.startDistanceClose = safeDistance * 0.05f;
  } else if (value == "early") {
    out.startDistanceOpen = safeDistance * 0.2f;
    out.startDistanceClose = safeDistance * 0.2f;
  } else {
    out.startDistanceOpen = safeDistance * 0.1f;
    out.startDistanceClose = safeDistance * 0.1f;
  }
  out.force = 0;
  if (value == "soft") {
    out.force = 30;
    out.mode = "coast";
  } else if (value == "strong") {
    out.force = 80;
    out.mode = "hold";
  } else {
    out.force = 55;
    out.mode = "active";
  }
  float limit = safeDistance * 0.5f;
  if (out.startDistanceOpen > limit) {
    out.startDistanceOpen = limit;
  }
  if (out.startDistanceClose > limit) {
    out.startDistanceClose = limit;
  }
  return out;
}

bool isTimeMode(const String& mode) {
  String value = normalizeLabel(mode);
  return value == "time";
}
} // namespace

ConfigManager::ConfigManager() {}

void ConfigManager::begin() {
  lastSaveMs = 0;
  lastSaveError = "";
  lastRemotesSaveOk = true;
  lastRemotesSaveError = "";
  lastRemotesSaveMs = 0;
  ensureDefaultConfigExists();
}

void ConfigManager::resetToDefaults() {
  gateConfig = GateConfig();
  limitsConfig = LimitsConfig();
  wifiConfig = WiFiConfig();
  mqttConfig = MQTTConfig();
  otaConfig = OTAConfig();
  motorConfig = MotorConfig();
  hoverUartConfig = HoverUartConfig();
  gpioConfig = GpioConfig();
  sensorsConfig = SensorsConfig();
  safetyConfig = SafetyConfig();
  remoteConfig = RemoteConfig();
  ledConfig = LedConfig();
  motionConfig = MotionConfig();
  securityConfig = SecurityConfig();
  deviceConfig = DeviceConfig();
  limitsConfig.open.pin = gpioConfig.limitOpenPin;
  limitsConfig.open.invert = gpioConfig.limitOpenInvert;
  limitsConfig.open.enabled = limitsConfig.enabled;
  limitsConfig.close.pin = gpioConfig.limitClosePin;
  limitsConfig.close.invert = gpioConfig.limitCloseInvert;
  limitsConfig.close.enabled = limitsConfig.enabled;
  remotes.clear();
  lastRemotesSaveOk = true;
  lastRemotesSaveError = "";
  lastRemotesSaveMs = 0;
}

bool ConfigManager::readConfigFileToDoc(DynamicJsonDocument& doc, String& error) {
  if (!readJsonFile(CONFIG_PATH, doc, error)) return false;
  revision++;
  return true;
}

void ConfigManager::buildJson(JsonDocument& doc) const {
  doc.clear();
  doc["version"] = CONFIG_VERSION;

  JsonObject device = doc.createNestedObject("device");
  device["name"] = deviceConfig.name;
  device["hostname"] = deviceConfig.hostname;
  device["webPort"] = deviceConfig.webPort;

  JsonObject wifi = doc.createNestedObject("wifi");
  wifi["ssid"] = wifiConfig.ssid;
  wifi["password"] = wifiConfig.password;

  JsonObject ap = wifi.createNestedObject("apFallback");
  ap["ssid"] = wifiConfig.apFallback.ssid;
  ap["password"] = wifiConfig.apFallback.password;
  ap["timeoutMs"] = wifiConfig.apFallback.fallbackTimeoutMs;

  JsonObject ip = wifi.createNestedObject("staticIp");
  ip["enabled"] = wifiConfig.staticIp.enabled;
  ip["ip"] = wifiConfig.staticIp.ip;
  ip["gateway"] = wifiConfig.staticIp.gateway;
  ip["netmask"] = wifiConfig.staticIp.netmask;
  ip["dns1"] = wifiConfig.staticIp.dns1;
  ip["dns2"] = wifiConfig.staticIp.dns2;

  JsonObject mqtt = doc.createNestedObject("mqtt");
  mqtt["enabled"] = mqttConfig.enabled;
  mqtt["server"] = mqttConfig.server;
  mqtt["port"] = mqttConfig.port;
  mqtt["user"] = mqttConfig.user;
  mqtt["password"] = mqttConfig.password;
  mqtt["topicBase"] = mqttConfig.topicBase;
  mqtt["retain"] = mqttConfig.retain;
  mqtt["qos"] = mqttConfig.qos;
  mqtt["lwtTopic"] = mqttConfig.lwtTopic;
  mqtt["lwtPayload"] = mqttConfig.lwtPayload;

  JsonObject ota = doc.createNestedObject("ota");
  ota["enabled"] = otaConfig.enabled;
  ota["port"] = otaConfig.port;
  ota["password"] = otaConfig.password;

  JsonObject gate = doc.createNestedObject("gate");
  float maxDistance = gateConfig.maxDistance > 0.0f ? gateConfig.maxDistance : gateConfig.totalDistance;
  gate["totalDistance"] = maxDistance;
  gate["maxDistance"] = maxDistance;
  gate["position"] = gateConfig.position;
  gate["hbOriginDistMm"] = gateConfig.hbOriginDistMm;
  gate["wheelCircumference"] = gateConfig.wheelCircumference;
  gate["pulsesPerRevolution"] = gateConfig.pulsesPerRevolution;
  gate["movementTimeout"] = gateConfig.movementTimeout;
  gate["stallTimeoutMs"] = gateConfig.stallTimeoutMs;
  gate["telemetryTimeoutMs"] = gateConfig.telemetryTimeoutMs;
  gate["telemetryGraceMs"] = gateConfig.telemetryGraceMs;
  gate["softLimitsEnabled"] = gateConfig.softLimitsEnabled;
  gate["currentLimitA"] = gateConfig.currentLimitA;
  gate["overCurrentTripMs"] = gateConfig.overCurrentTripMs;
  gate["overCurrentCooldownMs"] = gateConfig.overCurrentCooldownMs;
  gate["overCurrentMaxAutoRearm"] = gateConfig.overCurrentMaxAutoRearm;
  gate["overCurrentAutoRearm"] = gateConfig.overCurrentAutoRearm;
  gate["overCurrentAction"] = gateConfig.overCurrentAction;
  gate["autoCloseDelay"] = gateConfig.autoCloseDelay;
  gate["toggleDirection"] = gateConfig.toggleDirection;
  gate["positionSource"] = gateConfig.positionSource;
  gate["allowMoveWithoutLimits"] = gateConfig.allowMoveWithoutLimits;

  JsonObject limits = doc.createNestedObject("limits");
  limits["enabled"] = limitsConfig.enabled;
  JsonObject limOpen = limits.createNestedObject("open");
  limOpen["enabled"] = limitsConfig.open.enabled;
  limOpen["pin"] = limitsConfig.open.pin;
  limOpen["invert"] = limitsConfig.open.invert;
  limOpen["pullMode"] = limitsConfig.open.pullMode;
  limOpen["debounceMs"] = limitsConfig.open.debounceMs;
  JsonObject limClose = limits.createNestedObject("close");
  limClose["enabled"] = limitsConfig.close.enabled;
  limClose["pin"] = limitsConfig.close.pin;
  limClose["invert"] = limitsConfig.close.invert;
  limClose["pullMode"] = limitsConfig.close.pullMode;
  limClose["debounceMs"] = limitsConfig.close.debounceMs;

  JsonObject motor = doc.createNestedObject("motor");
  motor["driverType"] = motorConfig.driverType;
  motor["pwmMin"] = motorConfig.pwmMin;
  motor["pwmMax"] = motorConfig.pwmMax;
  motor["pwmFreq"] = motorConfig.pwmFreq;
  motor["pwmResolution"] = motorConfig.pwmResolution;
  motor["softStartMs"] = motorConfig.softStartMs;
  motor["softStopMs"] = motorConfig.softStopMs;
  motor["rampCurve"] = motorConfig.rampCurve;
  motor["invertDir"] = motorConfig.invertDir;

  JsonObject hover = doc.createNestedObject("hoverUart");
  hover["rx"] = hoverUartConfig.rxPin;
  hover["tx"] = hoverUartConfig.txPin;
  hover["baud"] = hoverUartConfig.baud;
  hover["maxSpeed"] = hoverUartConfig.maxSpeed;
  hover["rampStep"] = hoverUartConfig.rampStep;
  hover["keepArmed"] = hoverUartConfig.keepArmed;
  hover["faultCooldownMs"] = hoverUartConfig.faultCooldownMs;
  hover["maxAutoRearm"] = hoverUartConfig.maxAutoRearm;

  JsonObject gpio = doc.createNestedObject("gpio");
  gpio["pwm"] = gpioConfig.pwmPin;
  gpio["dir"] = gpioConfig.dirPin;
  gpio["en"] = gpioConfig.enPin;
  gpio["limitOpen"] = gpioConfig.limitOpenPin;
  gpio["limitClose"] = gpioConfig.limitClosePin;
  gpio["button"] = gpioConfig.buttonPin;
  gpio["stop"] = gpioConfig.stopPin;
  gpio["obstacle"] = gpioConfig.obstaclePin;
  gpio["hcs"] = gpioConfig.hcsPin;
  gpio["led"] = gpioConfig.ledPin;
  gpio["limitOpenInvert"] = gpioConfig.limitOpenInvert;
  gpio["limitCloseInvert"] = gpioConfig.limitCloseInvert;
  gpio["buttonInvert"] = gpioConfig.buttonInvert;
  gpio["obstacleInvert"] = gpioConfig.obstacleInvert;
  gpio["dirInvert"] = gpioConfig.dirInvert;

  JsonObject sensors = doc.createNestedObject("sensors");
  JsonObject photo = sensors.createNestedObject("photocell");
  photo["enabled"] = sensorsConfig.photocell.enabled;
  photo["pin"] = sensorsConfig.photocell.pin;
  photo["invert"] = sensorsConfig.photocell.invert;
  photo["pullMode"] = sensorsConfig.photocell.pullMode;
  photo["debounceMs"] = sensorsConfig.photocell.debounceMs;

  JsonObject safety = doc.createNestedObject("safety");
  safety["obstacleAction"] = safetyConfig.obstacleAction;
  safety["obstacleReverseCm"] = safetyConfig.obstacleReverseCm;
  safety["watchdogEnabled"] = safetyConfig.watchdogEnabled;

  JsonObject rem = doc.createNestedObject("remotes");
  rem["antiRepeatMs"] = remoteConfig.antiRepeatMs;
  rem["antiReplay"] = remoteConfig.antiReplay;
  rem["replayWindow"] = remoteConfig.replayWindow;
  JsonArray items = rem.createNestedArray("items");
  for (const auto& r : remotes) {
    JsonObject item = items.createNestedObject();
    item["serial"] = r.serial;
    item["name"] = r.name;
    item["enabled"] = r.enabled;
    item["lastCounter"] = r.lastCounter;
    item["lastSeenMs"] = r.lastSeenMs;
  }

  JsonObject led = doc.createNestedObject("led");
  led["enabled"] = ledConfig.enabled;
  led["type"] = ledConfig.type;
  led["pin"] = ledConfig.pin;
  led["count"] = ledConfig.count;
  led["brightness"] = ledConfig.brightness;
  led["ringStartIndex"] = ledConfig.ringStartIndex;
  led["ringReverse"] = ledConfig.ringReverse;
  led["colorOrder"] = ledConfig.colorOrder;
  led["defaultMode"] = ledConfig.defaultMode;
  led["mode"] = ledConfig.mode;
  led["animSpeed"] = ledConfig.animSpeed;
  JsonObject night = led.createNestedObject("nightMode");
  night["enabled"] = ledConfig.nightMode.enabled;
  night["brightness"] = ledConfig.nightMode.brightness;
  night["from"] = ledConfig.nightMode.from;
  night["to"] = ledConfig.nightMode.to;
  if (ledConfig.segmentCount > 0) {
    JsonArray segments = led.createNestedArray("segments");
    for (int i = 0; i < ledConfig.segmentCount && i < LedConfig::kMaxSegments; ++i) {
      JsonObject seg = segments.createNestedObject();
      seg["start"] = ledConfig.segments[i].start;
      seg["len"] = ledConfig.segments[i].length;
    }
  }

  JsonObject security = doc.createNestedObject("security");
  security["enabled"] = securityConfig.enabled;
  security["apiToken"] = securityConfig.apiToken;

  JsonObject motion = doc.createNestedObject("motion");
  motion["profile"] = motionConfig.profile;
  motion["expert"] = motionConfig.expert;
  JsonObject ui = motion.createNestedObject("ui");
  ui["speedOpen"] = motionConfig.ui.speedOpen;
  ui["speedClose"] = motionConfig.ui.speedClose;
  ui["accelSmoothness"] = motionConfig.ui.accelSmoothness;
  ui["decelSmoothness"] = motionConfig.ui.decelSmoothness;
  ui["slowdownDistance"] = motionConfig.ui.slowdownDistance;
  ui["brakingFeel"] = motionConfig.ui.brakingFeel;

  MotionAdvancedConfig profile = motionConfig.advanced;
  JsonObject adv = motion.createNestedObject("advanced");
  adv["maxSpeedOpen"] = profile.maxSpeedOpen;
  adv["maxSpeedClose"] = profile.maxSpeedClose;
  adv["minSpeed"] = profile.minSpeed;
  JsonObject rampOpen = adv.createNestedObject("rampOpen");
  rampOpen["mode"] = profile.rampOpen.mode;
  rampOpen["value"] = profile.rampOpen.value;
  JsonObject rampClose = adv.createNestedObject("rampClose");
  rampClose["mode"] = profile.rampClose.mode;
  rampClose["value"] = profile.rampClose.value;
  JsonObject braking = adv.createNestedObject("braking");
  braking["startDistanceOpen"] = profile.braking.startDistanceOpen;
  braking["startDistanceClose"] = profile.braking.startDistanceClose;
  braking["force"] = profile.braking.force;
  braking["mode"] = profile.braking.mode;
}

bool ConfigManager::ensureDefaultConfigExists(String* error) {
  if (LittleFS.exists(CONFIG_PATH)) return true;
  resetToDefaults();
  if (!save(error)) {
    setError(error, "default_save_failed");
    return false;
  }
  return true;
}

void ConfigManager::load() {
  String err;
  if (!ensureDefaultConfigExists(&err)) {
    Serial.printf("CONFIG LOAD: default ensure failed (%s)\n", err.c_str());
    return;
  }

  resetToDefaults();

  DynamicJsonDocument doc(CONFIG_JSON_CAPACITY);
  if (!readConfigFileToDoc(doc, err)) {
    Serial.printf("CONFIG LOAD FAIL: %s\n", err.c_str());
    save(nullptr);
    return;
  }

  JsonVariantConst root = doc.as<JsonVariantConst>();
  if (!root.is<JsonObjectConst>()) {
    Serial.println("CONFIG LOAD FAIL: root_not_object");
    resetToDefaults();
    save(nullptr);
    return;
  }

  int version = root["version"] | CONFIG_VERSION;
  if (version != CONFIG_VERSION) {
    Serial.printf("CONFIG LOAD FAIL: version_mismatch %d != %d\n", version, CONFIG_VERSION);
    resetToDefaults();
    save(nullptr);
    return;
  }

  if (!fromJsonVariant(root)) {
    Serial.println("CONFIG LOAD FAIL: parse_apply");
    resetToDefaults();
    save(nullptr);
    return;
  }

  Serial.printf("[CFG_LOAD] gate.maxDistance=%.3f gate.totalDistance=%.3f gate.position=%.3f\n",
                gateConfig.maxDistance, gateConfig.totalDistance, gateConfig.position);

  // If API security is enabled but no token is set, generate a token and persist it.
  if (securityConfig.enabled && securityConfig.apiToken.length() == 0) {
    const char* hex = "0123456789abcdef";
    String t;
    t.reserve(32);
    for (int i = 0; i < 32; ++i) {
      uint32_t r = esp_random();
      t += hex[r & 0xF];
    }
    securityConfig.apiToken = t;
    Serial.printf("[security] generated apiToken=%s\n", securityConfig.apiToken.c_str());
    save(nullptr);
  }

}

bool ConfigManager::save(String* error) {
  return saveInternal(error, false);
}

bool ConfigManager::processDeferredSave() {
  if (!pendingSave) return false;
  if (saveAllowedCb && !saveAllowedCb()) return false;
  unsigned long now = millis();
  if (lastDeferredAttemptMs != 0 && now - lastDeferredAttemptMs < 2000) return false;
  lastDeferredAttemptMs = now;
  return saveInternal(nullptr, true);
}

bool ConfigManager::saveInternal(String* error, bool force) {
  if (!force && saveAllowedCb && !saveAllowedCb()) {
    pendingSave = true;
    pendingSaveAtMs = millis();
    return true;
  }
  static volatile bool saving = false;
  unsigned long waitStart = millis();
  while (saving && millis() - waitStart < 1000) {
    delay(5);
  }
  if (saving) {
    lastSaveError = "save_busy";
    setError(error, lastSaveError.c_str());
    pendingSave = true;
    pendingSaveAtMs = millis();
    return false;
  }
  saving = true;
  struct SaveGuard {
    volatile bool* flag;
    explicit SaveGuard(volatile bool* f) : flag(f) {}
    ~SaveGuard() { *flag = false; }
  } guard(&saving);
  lastSaveMs = millis();
  lastSaveError = "";
  size_t written = 0;

  DynamicJsonDocument doc(CONFIG_JSON_CAPACITY);
  buildJson(doc);

  if (!writeJsonFile(CONFIG_TMP_PATH, doc, &written)) {
    lastSaveError = "open_tmp_failed";
    Serial.printf("[save] bytes=%u fail errno=%d\n", (unsigned)written, errno);
    setError(error, lastSaveError.c_str());
    pendingSave = true;
    pendingSaveAtMs = millis();
    return false;
  }
  DynamicJsonDocument verifyTmp(CONFIG_JSON_CAPACITY);
  String verifyTmpErr;
  if (!readJsonFile(CONFIG_TMP_PATH, verifyTmp, verifyTmpErr)) {
    LittleFS.remove(CONFIG_TMP_PATH);
    lastSaveError = "tmp_readback_failed";
    Serial.printf("[save] bytes=%u fail errno=%d\n", (unsigned)written, errno);
    setError(error, verifyTmpErr.c_str());
    pendingSave = true;
    pendingSaveAtMs = millis();
    return false;
  }

  bool committed = false;
  if (!LittleFS.exists(CONFIG_PATH)) {
    committed = LittleFS.rename(CONFIG_TMP_PATH, CONFIG_PATH);
  } else {
    committed = LittleFS.rename(CONFIG_TMP_PATH, CONFIG_PATH);
    if (!committed) {
      // LittleFS may reject unlink/replace when another request still has CONFIG_PATH open.
      // Fallback to an in-place rewrite to avoid "Has open FD" on remove(CONFIG_PATH).
      Serial.printf("[save] rename fallback path errno=%d\n", errno);
      committed = writeJsonFile(CONFIG_PATH, doc, &written);
      LittleFS.remove(CONFIG_TMP_PATH);
    }
  }
  if (!committed) {
    lastSaveError = "commit_failed";
    Serial.printf("[save] bytes=%u fail errno=%d\n", (unsigned)written, errno);
    setError(error, lastSaveError.c_str());
    pendingSave = true;
    pendingSaveAtMs = millis();
    return false;
  }

  DynamicJsonDocument verify(CONFIG_JSON_CAPACITY);
  String verifyErr;
  if (!readConfigFileToDoc(verify, verifyErr)) {
    lastSaveError = "readback_failed";
    Serial.printf("[save] bytes=%u fail errno=%d\n", (unsigned)written, errno);
    setError(error, verifyErr.c_str());
    pendingSave = true;
    pendingSaveAtMs = millis();
    return false;
  }
  if (!verify.is<JsonObject>()) {
    lastSaveError = "readback_invalid";
    Serial.printf("[save] bytes=%u fail errno=%d\n", (unsigned)written, errno);
    setError(error, lastSaveError.c_str());
    pendingSave = true;
    pendingSaveAtMs = millis();
    return false;
  }

  pendingSave = false;
  Serial.printf("[save] bytes=%u ok\n", (unsigned)written);
  return true;
}

String ConfigManager::toJson() {
  DynamicJsonDocument doc(CONFIG_JSON_CAPACITY);
  buildJson(doc);

  String out;
  serializeJson(doc, out);
  return out;
}

bool ConfigManager::fromJson(const String& json) {
  DynamicJsonDocument doc(CONFIG_JSON_CAPACITY);
  auto err = deserializeJson(doc, json);
  if (err) return false;
  return fromJsonVariant(doc.as<JsonVariantConst>());
}

bool ConfigManager::fromJsonVariant(JsonVariantConst root) {
  if (!root.is<JsonObjectConst>()) return false;
  JsonObjectConst obj = root.as<JsonObjectConst>();

  if (obj.containsKey("device")) {
    JsonObjectConst device = obj["device"];
    if (!device.isNull()) {
      deviceConfig.name = String((const char*)(device["name"] | deviceConfig.name.c_str()));
      deviceConfig.hostname = String((const char*)(device["hostname"] | deviceConfig.hostname.c_str()));
      deviceConfig.webPort = device["webPort"] | deviceConfig.webPort;
    }
  }

  if (obj.containsKey("wifi")) {
    JsonObjectConst wifi = obj["wifi"];
    if (!wifi.isNull()) {
      wifiConfig.ssid = String((const char*)(wifi["ssid"] | wifiConfig.ssid.c_str()));
      wifiConfig.password = String((const char*)(wifi["password"] | wifiConfig.password.c_str()));

      JsonObjectConst ap = wifi["apFallback"];
      if (!ap.isNull()) {
        wifiConfig.apFallback.ssid = String((const char*)(ap["ssid"] | wifiConfig.apFallback.ssid.c_str()));
        wifiConfig.apFallback.password = String((const char*)(ap["password"] | wifiConfig.apFallback.password.c_str()));
        wifiConfig.apFallback.fallbackTimeoutMs = ap["timeoutMs"] | wifiConfig.apFallback.fallbackTimeoutMs;
      }

      JsonObjectConst ip = wifi["staticIp"];
      if (!ip.isNull()) {
        wifiConfig.staticIp.enabled = ip["enabled"] | wifiConfig.staticIp.enabled;
        wifiConfig.staticIp.ip = String((const char*)(ip["ip"] | wifiConfig.staticIp.ip.c_str()));
        wifiConfig.staticIp.gateway = String((const char*)(ip["gateway"] | wifiConfig.staticIp.gateway.c_str()));
        wifiConfig.staticIp.netmask = String((const char*)(ip["netmask"] | wifiConfig.staticIp.netmask.c_str()));
        wifiConfig.staticIp.dns1 = String((const char*)(ip["dns1"] | wifiConfig.staticIp.dns1.c_str()));
        wifiConfig.staticIp.dns2 = String((const char*)(ip["dns2"] | wifiConfig.staticIp.dns2.c_str()));
      }
    }
  }

  if (obj.containsKey("mqtt")) {
    JsonObjectConst mqtt = obj["mqtt"];
    if (!mqtt.isNull()) {
      mqttConfig.enabled = mqtt["enabled"] | mqttConfig.enabled;
      mqttConfig.server = String((const char*)(mqtt["server"] | mqttConfig.server.c_str()));
      mqttConfig.port = mqtt["port"] | mqttConfig.port;
      mqttConfig.user = String((const char*)(mqtt["user"] | mqttConfig.user.c_str()));
      mqttConfig.password = String((const char*)(mqtt["password"] | mqttConfig.password.c_str()));
      mqttConfig.topicBase = String((const char*)(mqtt["topicBase"] | mqttConfig.topicBase.c_str()));
      mqttConfig.retain = mqtt["retain"] | mqttConfig.retain;
      mqttConfig.qos = mqtt["qos"] | mqttConfig.qos;
      mqttConfig.lwtTopic = String((const char*)(mqtt["lwtTopic"] | mqttConfig.lwtTopic.c_str()));
      mqttConfig.lwtPayload = String((const char*)(mqtt["lwtPayload"] | mqttConfig.lwtPayload.c_str()));
    }
  }

  if (obj.containsKey("ota")) {
    JsonObjectConst ota = obj["ota"];
    if (!ota.isNull()) {
      otaConfig.enabled = ota["enabled"] | otaConfig.enabled;
      otaConfig.port = ota["port"] | otaConfig.port;
      otaConfig.password = String((const char*)(ota["password"] | otaConfig.password.c_str()));
    }
  }

  if (obj.containsKey("gate")) {
    JsonObjectConst gate = obj["gate"];
    if (!gate.isNull()) {
      float maxDistance = gateConfig.maxDistance;
      if (gate.containsKey("maxDistance")) {
        maxDistance = gate["maxDistance"] | maxDistance;
      } else if (gate.containsKey("totalDistance")) {
        maxDistance = gate["totalDistance"] | maxDistance;
      }
      gateConfig.maxDistance = maxDistance;
      gateConfig.totalDistance = maxDistance;
      gateConfig.position = gate["position"] | gateConfig.position;
      if (gateConfig.position < 0.0f) gateConfig.position = 0.0f;
      if (gateConfig.maxDistance > 0.0f && gateConfig.position > gateConfig.maxDistance) {
        gateConfig.position = gateConfig.maxDistance;
      }
      gateConfig.hbOriginDistMm = gate["hbOriginDistMm"] | gateConfig.hbOriginDistMm;
      gateConfig.wheelCircumference = gate["wheelCircumference"] | gateConfig.wheelCircumference;
      gateConfig.pulsesPerRevolution = gate["pulsesPerRevolution"] | gateConfig.pulsesPerRevolution;
      gateConfig.movementTimeout = gate["movementTimeout"] | gateConfig.movementTimeout;
      gateConfig.stallTimeoutMs = gate["stallTimeoutMs"] | gateConfig.stallTimeoutMs;
      gateConfig.telemetryTimeoutMs = gate["telemetryTimeoutMs"] | gateConfig.telemetryTimeoutMs;
      gateConfig.telemetryGraceMs = gate["telemetryGraceMs"] | gateConfig.telemetryGraceMs;
      gateConfig.softLimitsEnabled = gate["softLimitsEnabled"] | gateConfig.softLimitsEnabled;
      gateConfig.currentLimitA = gate["currentLimitA"] | gateConfig.currentLimitA;
      gateConfig.overCurrentTripMs = gate["overCurrentTripMs"] | gateConfig.overCurrentTripMs;
      gateConfig.overCurrentCooldownMs = gate["overCurrentCooldownMs"] | gateConfig.overCurrentCooldownMs;
      gateConfig.overCurrentMaxAutoRearm = gate["overCurrentMaxAutoRearm"] | gateConfig.overCurrentMaxAutoRearm;
      gateConfig.overCurrentAutoRearm = gate["overCurrentAutoRearm"] | gateConfig.overCurrentAutoRearm;
      gateConfig.overCurrentAction = String((const char*)(gate["overCurrentAction"] | gateConfig.overCurrentAction.c_str()));
      gateConfig.autoCloseDelay = gate["autoCloseDelay"] | gateConfig.autoCloseDelay;
      gateConfig.toggleDirection = gate["toggleDirection"] | gateConfig.toggleDirection;
      gateConfig.positionSource = String((const char*)(gate["positionSource"] | gateConfig.positionSource.c_str()));
      gateConfig.allowMoveWithoutLimits = gate["allowMoveWithoutLimits"] | gateConfig.allowMoveWithoutLimits;
    }
  }

  bool limitsOpenSeen = false;
  bool limitsCloseSeen = false;
  bool photocellEnabledSeen = false;
  if (obj.containsKey("limits")) {
    JsonObjectConst lim = obj["limits"];
    if (!lim.isNull()) {
      limitsConfig.enabled = lim["enabled"] | limitsConfig.enabled;
      JsonObjectConst limOpen = lim["open"];
      if (!limOpen.isNull()) {
        limitsOpenSeen = true;
        limitsConfig.open.enabled = limOpen["enabled"] | limitsConfig.open.enabled;
        limitsConfig.open.pin = limOpen["pin"] | limitsConfig.open.pin;
        limitsConfig.open.invert = limOpen["invert"] | limitsConfig.open.invert;
        limitsConfig.open.pullMode = String((const char*)(limOpen["pullMode"] | limitsConfig.open.pullMode.c_str()));
        limitsConfig.open.debounceMs = limOpen["debounceMs"] | limitsConfig.open.debounceMs;
      }
      JsonObjectConst limClose = lim["close"];
      if (!limClose.isNull()) {
        limitsCloseSeen = true;
        limitsConfig.close.enabled = limClose["enabled"] | limitsConfig.close.enabled;
        limitsConfig.close.pin = limClose["pin"] | limitsConfig.close.pin;
        limitsConfig.close.invert = limClose["invert"] | limitsConfig.close.invert;
        limitsConfig.close.pullMode = String((const char*)(limClose["pullMode"] | limitsConfig.close.pullMode.c_str()));
        limitsConfig.close.debounceMs = limClose["debounceMs"] | limitsConfig.close.debounceMs;
      }
    }
  }

  
  if (obj.containsKey("security")) {
    JsonObjectConst sec = obj["security"];
    if (!sec.isNull()) {
      securityConfig.enabled = sec["enabled"] | securityConfig.enabled;
      securityConfig.apiToken = String((const char*)(sec["apiToken"] | sec["api_token"] | securityConfig.apiToken.c_str()));
    }
  }

if (obj.containsKey("motor")) {
    JsonObjectConst motor = obj["motor"];
    if (!motor.isNull()) {
      motorConfig.driverType = String((const char*)(motor["driverType"] | motorConfig.driverType.c_str()));
      motorConfig.pwmMin = motor["pwmMin"] | motorConfig.pwmMin;
      motorConfig.pwmMax = motor["pwmMax"] | motorConfig.pwmMax;
      motorConfig.pwmFreq = motor["pwmFreq"] | motorConfig.pwmFreq;
      motorConfig.pwmResolution = motor["pwmResolution"] | motorConfig.pwmResolution;
      motorConfig.softStartMs = motor["softStartMs"] | motorConfig.softStartMs;
      motorConfig.softStopMs = motor["softStopMs"] | motorConfig.softStopMs;
      motorConfig.rampCurve = String((const char*)(motor["rampCurve"] | motorConfig.rampCurve.c_str()));
      motorConfig.invertDir = motor["invertDir"] | motorConfig.invertDir;
    }
  }

  if (obj.containsKey("motion")) {
    JsonObjectConst motion = obj["motion"];
    if (!motion.isNull()) {
      motionConfig.profile = String((const char*)(motion["profile"] | motionConfig.profile.c_str()));
      motionConfig.expert = motion["expert"] | motionConfig.expert;

      JsonObjectConst ui = motion["ui"];
      if (!ui.isNull()) {
        motionConfig.ui.speedOpen = String((const char*)(ui["speedOpen"] | motionConfig.ui.speedOpen.c_str()));
        motionConfig.ui.speedClose = String((const char*)(ui["speedClose"] | motionConfig.ui.speedClose.c_str()));
        motionConfig.ui.accelSmoothness = String((const char*)(ui["accelSmoothness"] | motionConfig.ui.accelSmoothness.c_str()));
        motionConfig.ui.decelSmoothness = String((const char*)(ui["decelSmoothness"] | motionConfig.ui.decelSmoothness.c_str()));
        motionConfig.ui.slowdownDistance = String((const char*)(ui["slowdownDistance"] | motionConfig.ui.slowdownDistance.c_str()));
        motionConfig.ui.brakingFeel = String((const char*)(ui["brakingFeel"] | motionConfig.ui.brakingFeel.c_str()));
      }

      JsonObjectConst adv = motion["advanced"];
      if (!adv.isNull()) {
        motionConfig.advanced.maxSpeedOpen = adv["maxSpeedOpen"] | motionConfig.advanced.maxSpeedOpen;
        motionConfig.advanced.maxSpeedClose = adv["maxSpeedClose"] | motionConfig.advanced.maxSpeedClose;
        motionConfig.advanced.minSpeed = adv["minSpeed"] | motionConfig.advanced.minSpeed;
        JsonObjectConst rampOpen = adv["rampOpen"];
        if (!rampOpen.isNull()) {
          motionConfig.advanced.rampOpen.mode = String((const char*)(rampOpen["mode"] | motionConfig.advanced.rampOpen.mode.c_str()));
          motionConfig.advanced.rampOpen.value = rampOpen["value"] | motionConfig.advanced.rampOpen.value;
        }
        JsonObjectConst rampClose = adv["rampClose"];
        if (!rampClose.isNull()) {
          motionConfig.advanced.rampClose.mode = String((const char*)(rampClose["mode"] | motionConfig.advanced.rampClose.mode.c_str()));
          motionConfig.advanced.rampClose.value = rampClose["value"] | motionConfig.advanced.rampClose.value;
        }
        JsonObjectConst braking = adv["braking"];
        if (!braking.isNull()) {
          motionConfig.advanced.braking.startDistanceOpen = braking["startDistanceOpen"] | motionConfig.advanced.braking.startDistanceOpen;
          motionConfig.advanced.braking.startDistanceClose = braking["startDistanceClose"] | motionConfig.advanced.braking.startDistanceClose;
          motionConfig.advanced.braking.force = braking["force"] | motionConfig.advanced.braking.force;
          motionConfig.advanced.braking.mode = String((const char*)(braking["mode"] | motionConfig.advanced.braking.mode.c_str()));
        }
      }
    }
  }

  if (obj.containsKey("hoverUart")) {
    JsonObjectConst hover = obj["hoverUart"];
    if (!hover.isNull()) {
      hoverUartConfig.rxPin = hover["rx"] | hoverUartConfig.rxPin;
      hoverUartConfig.txPin = hover["tx"] | hoverUartConfig.txPin;
      hoverUartConfig.baud = hover["baud"] | hoverUartConfig.baud;
      hoverUartConfig.maxSpeed = hover["maxSpeed"] | hoverUartConfig.maxSpeed;
      hoverUartConfig.rampStep = hover["rampStep"] | hoverUartConfig.rampStep;
    }
  }

  if (obj.containsKey("gpio")) {
    JsonObjectConst gpio = obj["gpio"];
    if (!gpio.isNull()) {
      gpioConfig.pwmPin = gpio["pwm"] | gpioConfig.pwmPin;
      gpioConfig.dirPin = gpio["dir"] | gpioConfig.dirPin;
      gpioConfig.enPin = gpio["en"] | gpioConfig.enPin;
      gpioConfig.limitOpenPin = gpio["limitOpen"] | gpioConfig.limitOpenPin;
      gpioConfig.limitClosePin = gpio["limitClose"] | gpioConfig.limitClosePin;
      gpioConfig.buttonPin = gpio["button"] | gpioConfig.buttonPin;
      gpioConfig.stopPin = gpio["stop"] | gpioConfig.stopPin;
      gpioConfig.obstaclePin = gpio["obstacle"] | gpioConfig.obstaclePin;
      gpioConfig.hcsPin = gpio["hcs"] | gpioConfig.hcsPin;
      gpioConfig.ledPin = gpio["led"] | gpioConfig.ledPin;
      ledConfig.pin = gpioConfig.ledPin;
      gpioConfig.limitOpenInvert = gpio["limitOpenInvert"] | gpioConfig.limitOpenInvert;
      gpioConfig.limitCloseInvert = gpio["limitCloseInvert"] | gpioConfig.limitCloseInvert;
      gpioConfig.buttonInvert = gpio["buttonInvert"] | gpioConfig.buttonInvert;
      gpioConfig.obstacleInvert = gpio["obstacleInvert"] | gpioConfig.obstacleInvert;
      gpioConfig.dirInvert = gpio["dirInvert"] | gpioConfig.dirInvert;
    }
  }

  if (limitsOpenSeen) {
    gpioConfig.limitOpenPin = limitsConfig.open.pin;
    gpioConfig.limitOpenInvert = limitsConfig.open.invert;
  } else {
    limitsConfig.open.pin = gpioConfig.limitOpenPin;
    limitsConfig.open.invert = gpioConfig.limitOpenInvert;
    limitsConfig.open.enabled = limitsConfig.enabled;
  }
  if (limitsCloseSeen) {
    gpioConfig.limitClosePin = limitsConfig.close.pin;
    gpioConfig.limitCloseInvert = limitsConfig.close.invert;
  } else {
    limitsConfig.close.pin = gpioConfig.limitClosePin;
    limitsConfig.close.invert = gpioConfig.limitCloseInvert;
    limitsConfig.close.enabled = limitsConfig.enabled;
  }

  if (obj.containsKey("sensors")) {
    JsonObjectConst sensors = obj["sensors"];
    if (!sensors.isNull()) {
      JsonObjectConst photo = sensors["photocell"];
      if (!photo.isNull()) {
        if (photo.containsKey("enabled")) {
          photocellEnabledSeen = true;
        }
        sensorsConfig.photocell.enabled = photo["enabled"] | sensorsConfig.photocell.enabled;
        sensorsConfig.photocell.pin = photo["pin"] | sensorsConfig.photocell.pin;
        sensorsConfig.photocell.invert = photo["invert"] | sensorsConfig.photocell.invert;
        sensorsConfig.photocell.pullMode = String((const char*)(photo["pullMode"] | sensorsConfig.photocell.pullMode.c_str()));
        sensorsConfig.photocell.debounceMs = photo["debounceMs"] | sensorsConfig.photocell.debounceMs;
      }

    }
  }

  if (sensorsConfig.photocell.pin < 0 && gpioConfig.obstaclePin >= 0) {
    sensorsConfig.photocell.pin = gpioConfig.obstaclePin;
    sensorsConfig.photocell.invert = gpioConfig.obstacleInvert;
    if (!photocellEnabledSeen) {
      sensorsConfig.photocell.enabled = sensorsConfig.photocell.enabled || gpioConfig.obstaclePin >= 0;
    }
  }

  if (obj.containsKey("safety")) {
    JsonObjectConst safety = obj["safety"];
    if (!safety.isNull()) {
      safetyConfig.obstacleAction = String((const char*)(safety["obstacleAction"] | safetyConfig.obstacleAction.c_str()));
      safetyConfig.obstacleReverseCm = safety["obstacleReverseCm"] | safetyConfig.obstacleReverseCm;
      safetyConfig.watchdogEnabled = safety["watchdogEnabled"] | safetyConfig.watchdogEnabled;
    }
  }

  if (obj.containsKey("remotes")) {
    JsonVariantConst remVar = obj["remotes"];
    if (remVar.is<JsonObjectConst>()) {
      JsonObjectConst rem = remVar.as<JsonObjectConst>();
      remoteConfig.antiRepeatMs = rem["antiRepeatMs"] | remoteConfig.antiRepeatMs;
      remoteConfig.antiReplay = rem["antiReplay"] | remoteConfig.antiReplay;
      remoteConfig.replayWindow = rem["replayWindow"] | remoteConfig.replayWindow;

      JsonArrayConst items = rem["items"].as<JsonArrayConst>();
      if (!items.isNull()) {
        remotes.clear();
        for (JsonObjectConst item : items) {
          RemoteEntry r;
          r.serial = item["serial"] | 0;
          r.name = String((const char*)(item["name"] | ""));
          r.enabled = item["enabled"] | true;
          r.lastCounter = item["lastCounter"] | 0;
          r.lastSeenMs = item["lastSeenMs"] | 0;
          if (r.serial != 0) remotes.push_back(r);
        }
      }
    } else if (remVar.is<JsonArrayConst>()) {
      JsonArrayConst arr = remVar.as<JsonArrayConst>();
      remotes.clear();
      for (JsonVariantConst v : arr) {
        RemoteEntry r;
        r.serial = v.as<unsigned long>();
        if (r.serial != 0) remotes.push_back(r);
      }
    }
  }

  if (obj.containsKey("led")) {
    JsonObjectConst led = obj["led"];
    if (!led.isNull()) {
      ledConfig.enabled = led["enabled"] | ledConfig.enabled;
      ledConfig.type = String((const char*)(led["type"] | ledConfig.type.c_str()));
      ledConfig.pin = led["pin"] | ledConfig.pin;
      ledConfig.count = led["count"] | ledConfig.count;
      gpioConfig.ledPin = ledConfig.pin;
      ledConfig.brightness = led["brightness"] | ledConfig.brightness;
      ledConfig.ringStartIndex = led["ringStartIndex"] | ledConfig.ringStartIndex;
      ledConfig.ringReverse = led["ringReverse"] | ledConfig.ringReverse;
      ledConfig.colorOrder = String((const char*)(led["colorOrder"] | ledConfig.colorOrder.c_str()));
      ledConfig.defaultMode = String((const char*)(led["defaultMode"] | ledConfig.defaultMode.c_str()));
      ledConfig.mode = String((const char*)(led["mode"] | ledConfig.mode.c_str()));
      ledConfig.animSpeed = led["animSpeed"] | ledConfig.animSpeed;
      JsonObjectConst night = led["nightMode"];
      if (!night.isNull()) {
        ledConfig.nightMode.enabled = night["enabled"] | ledConfig.nightMode.enabled;
        ledConfig.nightMode.brightness = night["brightness"] | ledConfig.nightMode.brightness;
        ledConfig.nightMode.from = String((const char*)(night["from"] | ledConfig.nightMode.from.c_str()));
        ledConfig.nightMode.to = String((const char*)(night["to"] | ledConfig.nightMode.to.c_str()));
      }
      ledConfig.segmentCount = 0;
      JsonArrayConst segments = led["segments"].as<JsonArrayConst>();
      if (!segments.isNull()) {
        for (JsonObjectConst seg : segments) {
          if (ledConfig.segmentCount >= LedConfig::kMaxSegments) break;
          LedConfig::Segment& outSeg = ledConfig.segments[ledConfig.segmentCount++];
          outSeg.start = seg["start"] | outSeg.start;
          outSeg.length = seg["len"] | outSeg.length;
        }
      }
    }
  }

  if (obj.containsKey("security")) {
    JsonObjectConst sec = obj["security"];
    if (!sec.isNull()) {
      securityConfig.apiToken = String((const char*)(sec["apiToken"] | securityConfig.apiToken.c_str()));
    }
  }

  return true;
}

static bool isForbiddenPin(int pin) {
  const int forbidden[] = {6, 7, 8, 9, 10, 11};
  for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
    if (pin == forbidden[i]) return true;
  }
  return false;
}

bool ConfigManager::validate(JsonVariantConst root, String& error) {
  if (!root.is<JsonObjectConst>()) {
    error = "root_not_object";
    return false;
  }

  JsonObjectConst obj = root.as<JsonObjectConst>();
  int version = obj["version"] | CONFIG_VERSION;
  if (version != CONFIG_VERSION) {
    error = "version_mismatch";
    return false;
  }

  if (obj.containsKey("device")) {
    JsonObjectConst device = obj["device"];
    if (!device.isNull()) {
      int webPort = device["webPort"] | deviceConfig.webPort;
      if (webPort < 1 || webPort > 65535) {
        error = "device.webPort_out_of_range";
        return false;
      }
    }
  }

  if (obj.containsKey("gate")) {
    JsonObjectConst gate = obj["gate"];
    float maxDistance = gateConfig.maxDistance;
    if (gate.containsKey("maxDistance")) {
      maxDistance = gate["maxDistance"] | maxDistance;
    } else if (gate.containsKey("totalDistance")) {
      maxDistance = gate["totalDistance"] | maxDistance;
    }
    if (!(maxDistance > 0.0f) || maxDistance > 100.0f) {
      error = "gate.maxDistance_out_of_range";
      return false;
    }
    if (gate.containsKey("position")) {
      float position = gate["position"] | gateConfig.position;
      if (position < 0.0f || (maxDistance > 0.0f && position > maxDistance)) {
        error = "gate.position_out_of_range";
        return false;
      }
    }
    unsigned long stallTimeoutMs = gate["stallTimeoutMs"] | gateConfig.stallTimeoutMs;
    unsigned long telTimeoutMs = gate["telemetryTimeoutMs"] | gateConfig.telemetryTimeoutMs;
    unsigned long telGraceMs = gate["telemetryGraceMs"] | gateConfig.telemetryGraceMs;
    float currentLimitA = gate["currentLimitA"] | gateConfig.currentLimitA;
    if (stallTimeoutMs > 0 && (stallTimeoutMs < 200 || stallTimeoutMs > 60000)) {
      error = "gate.stallTimeoutMs_out_of_range";
      return false;
    }
    if (telTimeoutMs > 0 && (telTimeoutMs < 200 || telTimeoutMs > 10000)) {
      error = "gate.telemetryTimeoutMs_out_of_range";
      return false;
    }
    if (telGraceMs > 20000) {
      error = "gate.telemetryGraceMs_out_of_range";
      return false;
    }
    if (currentLimitA < 0.0f || currentLimitA > 80.0f) {
      error = "gate.currentLimitA_out_of_range";
      return false;
    }
    const char* overCurrentAction = gate["overCurrentAction"] | gateConfig.overCurrentAction.c_str();
    if (overCurrentAction && overCurrentAction[0] != '\0') {
      String action = String(overCurrentAction);
      if (action != "stop" && action != "reverse") {
        error = "gate.overCurrentAction_invalid";
        return false;
      }
    }
    const char* positionSource = gate["positionSource"] | gateConfig.positionSource.c_str();
    if (positionSource && positionSource[0] != '\0') {
      String src = String(positionSource);
      if (src != "encoder" && src != "hoverboard_tel") {
        error = "gate.positionSource_invalid";
        return false;
      }
    }
  }

  if (obj.containsKey("limits")) {
    JsonVariantConst limVar = obj["limits"];
    if (!limVar.isNull() && !limVar.is<JsonObjectConst>()) {
      error = "limits_not_object";
      return false;
    }
    JsonObjectConst lim = limVar.as<JsonObjectConst>();
    auto pullOk = [](const char* mode) {
      return mode[0] == '\0' || strcmp(mode, "none") == 0 || strcmp(mode, "up") == 0 || strcmp(mode, "down") == 0;
    };
    JsonObjectConst open = lim["open"];
    if (!open.isNull()) {
      int pin = open["pin"] | limitsConfig.open.pin;
      const char* pull = open["pullMode"] | "";
      int debounce = open["debounceMs"] | (int)limitsConfig.open.debounceMs;
      if (pin >= 0 && isForbiddenPin(pin)) {
        error = "limits.open.forbidden_pin";
        return false;
      }
      if (!pullOk(pull)) {
        error = "limits.open.pull_invalid";
        return false;
      }
      if (debounce < 0 || debounce > 2000) {
        error = "limits.open.debounce_out_of_range";
        return false;
      }
    }
    JsonObjectConst close = lim["close"];
    if (!close.isNull()) {
      int pin = close["pin"] | limitsConfig.close.pin;
      const char* pull = close["pullMode"] | "";
      int debounce = close["debounceMs"] | (int)limitsConfig.close.debounceMs;
      if (pin >= 0 && isForbiddenPin(pin)) {
        error = "limits.close.forbidden_pin";
        return false;
      }
      if (!pullOk(pull)) {
        error = "limits.close.pull_invalid";
        return false;
      }
      if (debounce < 0 || debounce > 2000) {
        error = "limits.close.debounce_out_of_range";
        return false;
      }
    }
  }

  if (obj.containsKey("gate")) {
    JsonObjectConst gate = obj["gate"];
    bool softLimitsEnabled = gate["softLimitsEnabled"] | gateConfig.softLimitsEnabled;
    bool limitsEnabled = limitsConfig.enabled;
    if (obj.containsKey("limits")) {
      JsonObjectConst lim = obj["limits"];
      limitsEnabled = lim["enabled"] | limitsEnabled;
    }
    if (!softLimitsEnabled && !limitsEnabled) {
      error = "gate.softLimits_required";
      return false;
    }
  }

  if (obj.containsKey("motor")) {
    JsonObjectConst motor = obj["motor"];
    int pwmMin = motor["pwmMin"] | motorConfig.pwmMin;
    int pwmMax = motor["pwmMax"] | motorConfig.pwmMax;
    int pwmFreq = motor["pwmFreq"] | motorConfig.pwmFreq;
    int pwmRes = motor["pwmResolution"] | motorConfig.pwmResolution;

    if (pwmMin < 0 || pwmMin > 255) {
      error = "motor.pwmMin_out_of_range";
      return false;
    }
    if (pwmMax < 0 || pwmMax > 255 || pwmMax < pwmMin) {
      error = "motor.pwmMax_out_of_range";
      return false;
    }
    if (pwmFreq < 1 || pwmFreq > 40000) {
      error = "motor.pwmFreq_out_of_range";
      return false;
    }
    if (pwmRes < 1 || pwmRes > 16) {
      error = "motor.pwmResolution_out_of_range";
      return false;
    }
  }

  if (obj.containsKey("led")) {
    JsonObjectConst led = obj["led"];
    if (!led.isNull()) {
      int count = led["count"] | ledConfig.count;
      int brightness = led["brightness"] | ledConfig.brightness;
      if (count < 1 || count > 1024) {
        error = "led.count_out_of_range";
        return false;
      }
      if (brightness < 0 || brightness > 100) {
        error = "led.brightness_out_of_range";
        return false;
      }
      if (led.containsKey("ringStartIndex")) {
        int ringStartIndex = led["ringStartIndex"] | 0;
        if (ringStartIndex < -4096 || ringStartIndex > 4096) {
          error = "led.ringStartIndex_out_of_range";
          return false;
        }
      }
    }
  }

  if (obj.containsKey("motion")) {
    JsonObjectConst motion = obj["motion"];
    if (!motion.isNull()) {
      JsonObjectConst adv = motion["advanced"];
      if (!adv.isNull()) {
        int maxSpeedOpen = adv["maxSpeedOpen"] | motionConfig.advanced.maxSpeedOpen;
        int maxSpeedClose = adv["maxSpeedClose"] | motionConfig.advanced.maxSpeedClose;
        int minSpeed = adv["minSpeed"] | motionConfig.advanced.minSpeed;
        if (maxSpeedOpen <= 0 || maxSpeedOpen > 2000) {
          error = "motion.maxSpeedOpen_out_of_range";
          return false;
        }
        if (maxSpeedClose <= 0 || maxSpeedClose > 2000) {
          error = "motion.maxSpeedClose_out_of_range";
          return false;
        }
        int limitSpeed = std::min(maxSpeedOpen, maxSpeedClose);
        if (limitSpeed <= 0) limitSpeed = max(maxSpeedOpen, maxSpeedClose);
        if (minSpeed <= 0 || minSpeed >= limitSpeed) {
          error = "motion.minSpeed_out_of_range";
          return false;
        }
        JsonObjectConst rampOpen = adv["rampOpen"];
        if (!rampOpen.isNull()) {
          const char* mode = rampOpen["mode"] | "";
          if (mode[0] != '\0' && strcmp(mode, "time") != 0 && strcmp(mode, "distance") != 0) {
            error = "motion.rampOpen.mode_invalid";
            return false;
          }
          float value = rampOpen["value"] | motionConfig.advanced.rampOpen.value;
          if (value <= 0.0f) {
            error = "motion.rampOpen.value_invalid";
            return false;
          }
        }
        JsonObjectConst rampClose = adv["rampClose"];
        if (!rampClose.isNull()) {
          const char* mode = rampClose["mode"] | "";
          if (mode[0] != '\0' && strcmp(mode, "time") != 0 && strcmp(mode, "distance") != 0) {
            error = "motion.rampClose.mode_invalid";
            return false;
          }
          float value = rampClose["value"] | motionConfig.advanced.rampClose.value;
          if (value <= 0.0f) {
            error = "motion.rampClose.value_invalid";
            return false;
          }
        }
        JsonObjectConst braking = adv["braking"];
        if (!braking.isNull()) {
          float startOpen = braking["startDistanceOpen"] | motionConfig.advanced.braking.startDistanceOpen;
          float startClose = braking["startDistanceClose"] | motionConfig.advanced.braking.startDistanceClose;
          int force = braking["force"] | motionConfig.advanced.braking.force;
          const char* mode = braking["mode"] | "";
          if (startOpen < 0.0f || startClose < 0.0f) {
            error = "motion.braking.start_distance_invalid";
            return false;
          }
          if (force < 0 || force > 100) {
            error = "motion.braking.force_invalid";
            return false;
          }
          if (mode[0] != '\0' &&
              strcmp(mode, "coast") != 0 &&
              strcmp(mode, "active") != 0 &&
              strcmp(mode, "hold") != 0) {
            error = "motion.braking.mode_invalid";
            return false;
          }
        }
      }
    }
  }

  if (obj.containsKey("hoverUart")) {
    JsonObjectConst hover = obj["hoverUart"];
    int rx = hover["rx"] | hoverUartConfig.rxPin;
    int tx = hover["tx"] | hoverUartConfig.txPin;
    int baud = hover["baud"] | hoverUartConfig.baud;
    int maxSpeed = hover["maxSpeed"] | hoverUartConfig.maxSpeed;
    int rampStep = hover["rampStep"] | hoverUartConfig.rampStep;
    if (baud < 1200 || baud > 1000000) {
      error = "hoverUart.baud_out_of_range";
      return false;
    }
    if (maxSpeed < 0 || maxSpeed > 2000) {
      error = "hoverUart.maxSpeed_out_of_range";
      return false;
    }
    if (rampStep < 1 || rampStep > 200) {
      error = "hoverUart.rampStep_out_of_range";
      return false;
    }
    if (rx >= 0 && isForbiddenPin(rx)) {
      error = "hoverUart.forbidden_pin";
      return false;
    }
    if (tx >= 0 && isForbiddenPin(tx)) {
      error = "hoverUart.forbidden_pin";
      return false;
    }
  }

  if (obj.containsKey("gpio")) {
    JsonObjectConst gpio = obj["gpio"];
    int pins[] = {
      gpio["pwm"] | gpioConfig.pwmPin,
      gpio["dir"] | gpioConfig.dirPin,
      gpio["en"] | gpioConfig.enPin,
      gpio["limitOpen"] | gpioConfig.limitOpenPin,
      gpio["limitClose"] | gpioConfig.limitClosePin,
      gpio["button"] | gpioConfig.buttonPin,
      gpio["stop"] | gpioConfig.stopPin,
      gpio["obstacle"] | gpioConfig.obstaclePin,
      gpio["hcs"] | gpioConfig.hcsPin,
      gpio["led"] | gpioConfig.ledPin
    };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
      if (pins[i] >= 0 && isForbiddenPin(pins[i])) {
        error = "gpio.forbidden_pin";
        return false;
      }
    }
  }

  if (obj.containsKey("sensors")) {
    JsonObjectConst sensors = obj["sensors"];
    if (!sensors.isNull()) {
      auto pullOk = [](const char* mode) {
        return mode[0] == '\0' || strcmp(mode, "none") == 0 || strcmp(mode, "up") == 0 || strcmp(mode, "down") == 0;
      };
      JsonObjectConst photo = sensors["photocell"];
      if (!photo.isNull()) {
        int pin = photo["pin"] | sensorsConfig.photocell.pin;
        int debounce = photo["debounceMs"] | (int)sensorsConfig.photocell.debounceMs;
        const char* pull = photo["pullMode"] | "";
        if (pin >= 0 && isForbiddenPin(pin)) {
          error = "sensors.photocell.forbidden_pin";
          return false;
        }
        if (!pullOk(pull)) {
          error = "sensors.photocell.pull_invalid";
          return false;
        }
        if (debounce < 0 || debounce > 2000) {
          error = "sensors.photocell.debounce_out_of_range";
          return false;
        }
      }

    }
  }

  if (obj.containsKey("mqtt")) {
    JsonObjectConst mqtt = obj["mqtt"];
    int qos = mqtt["qos"] | mqttConfig.qos;
    if (qos < 0 || qos > 2) {
      error = "mqtt.qos_out_of_range";
      return false;
    }
  }

  if (obj.containsKey("led")) {
    JsonObjectConst led = obj["led"];
    int count = led["count"] | ledConfig.count;
    int brightness = led["brightness"] | ledConfig.brightness;
    if (brightness < 0 || brightness > 100) {
      error = "led.brightness_out_of_range";
      return false;
    }
    if (count < 0 || count > 300) {
      error = "led.count_out_of_range";
      return false;
    }
    int animSpeed = led["animSpeed"] | ledConfig.animSpeed;
    if (animSpeed < 1 || animSpeed > 100) {
      error = "led.animSpeed_out_of_range";
      return false;
    }
    int pin = led["pin"] | ledConfig.pin;
    if (pin >= 0 && isForbiddenPin(pin)) {
      error = "led.forbidden_pin";
      return false;
    }
    JsonArrayConst segments = led["segments"].as<JsonArrayConst>();
    if (!segments.isNull()) {
      int idx = 0;
      for (JsonObjectConst seg : segments) {
        if (idx++ >= LedConfig::kMaxSegments) {
          error = "led.segments_too_many";
          return false;
        }
        int start = seg["start"] | 0;
        int len = seg["len"] | 0;
        if (start < 0 || len < 0 || start > 2048 || len > 2048) {
          error = "led.segment_range";
          return false;
        }
      }
    }
  }

  return true;
}

MotionAdvancedConfig ConfigManager::motionProfile() const {
  MotionAdvancedConfig result = motionConfig.advanced;
  float maxDistance = gateConfig.maxDistance > 0.0f ? gateConfig.maxDistance : gateConfig.totalDistance;
  float safeDistance = ensurePositive(maxDistance, 12.0f);

  if (!motionConfig.expert) {
    int fallbackOpen = result.maxSpeedOpen > 0 ? result.maxSpeedOpen : 220;
    int fallbackClose = result.maxSpeedClose > 0 ? result.maxSpeedClose : 220;
    result.maxSpeedOpen = mapSpeedLabel(motionConfig.ui.speedOpen, fallbackOpen);
    result.maxSpeedClose = mapSpeedLabel(motionConfig.ui.speedClose, fallbackClose);
    result.minSpeed = mapMinSpeed(motionConfig.ui.brakingFeel);
    result.rampOpen = deriveRamp(motionConfig.ui.accelSmoothness, safeDistance);
    result.rampClose = deriveRamp(motionConfig.ui.decelSmoothness, safeDistance);
    result.braking = deriveBraking(motionConfig.ui.slowdownDistance, safeDistance);
  } else {
    result.maxSpeedOpen = constrain(result.maxSpeedOpen, 20, 2000);
    result.maxSpeedClose = constrain(result.maxSpeedClose, 20, 2000);
    if (result.minSpeed <= 0) result.minSpeed = mapMinSpeed(motionConfig.ui.brakingFeel);
    if (result.rampOpen.mode != "time" && result.rampOpen.mode != "distance") {
      result.rampOpen.mode = "distance";
    }
    if (result.rampClose.mode != "time" && result.rampClose.mode != "distance") {
      result.rampClose.mode = "distance";
    }
    if (result.rampOpen.mode == "distance" && result.rampOpen.value > safeDistance) {
      result.rampOpen.value = safeDistance;
    }
    if (result.rampClose.mode == "distance" && result.rampClose.value > safeDistance) {
      result.rampClose.value = safeDistance;
    }
    if (result.rampOpen.value <= 0.0f) result.rampOpen.value = 0.1f;
    if (result.rampClose.value <= 0.0f) result.rampClose.value = 0.1f;
    if (result.braking.startDistanceOpen < 0.0f) result.braking.startDistanceOpen = 0.0f;
    if (result.braking.startDistanceClose < 0.0f) result.braking.startDistanceClose = 0.0f;
    if (result.braking.startDistanceOpen > safeDistance * 0.5f) {
      result.braking.startDistanceOpen = safeDistance * 0.5f;
    }
    if (result.braking.startDistanceClose > safeDistance * 0.5f) {
      result.braking.startDistanceClose = safeDistance * 0.5f;
    }
    if (result.braking.force < 0) result.braking.force = 0;
    if (result.braking.force > 100) result.braking.force = 100;
    if (result.braking.mode != "coast" && result.braking.mode != "active" && result.braking.mode != "hold") {
      result.braking.mode = "active";
    }
  }

  int limitSpeed = std::min(result.maxSpeedOpen, result.maxSpeedClose);
  if (limitSpeed <= 0) limitSpeed = max(result.maxSpeedOpen, result.maxSpeedClose);
  if (result.minSpeed >= limitSpeed) {
    result.minSpeed = max(10, limitSpeed - 10);
  }
  if (result.minSpeed <= 0) result.minSpeed = 10;

  if (!motionConfig.expert) {
    // ensure braking force/mode follow defaults
    if (motionConfig.ui.brakingFeel.length() == 0) {
      result.braking.force = 50;
      result.braking.mode = "active";
    }
  }

  return result;
}

bool ConfigManager::addRemote(unsigned long serial, const String& name) {
  for (auto& r : remotes) {
    if (r.serial == serial) return false;
  }
  RemoteEntry entry;
  entry.serial = serial;
  entry.name = name;
  entry.enabled = true;
  remotes.push_back(entry);
  String err;
  // Security-critical path: persist remote immediately even during motion.
  // Deferring may lose authorization changes on unexpected reboot.
  bool ok = saveInternal(&err, true);
  lastRemotesSaveMs = millis();
  lastRemotesSaveOk = ok;
  lastRemotesSaveError = ok ? "" : err;
  if (!ok) {
    remotes.pop_back();
  }
  return ok;
}

bool ConfigManager::updateRemote(unsigned long serial, const String& name, bool enabled) {
  for (auto& r : remotes) {
    if (r.serial == serial) {
      RemoteEntry prev = r;
      if (name.length() > 0) r.name = name;
      r.enabled = enabled;
      String err;
      // Security-critical path: persist remote immediately even during motion.
      bool ok = saveInternal(&err, true);
      lastRemotesSaveMs = millis();
      lastRemotesSaveOk = ok;
      lastRemotesSaveError = ok ? "" : err;
      if (!ok) {
        r = prev;
      }
      return ok;
    }
  }
  return false;
}

bool ConfigManager::removeRemote(unsigned long serial) {
  for (auto it = remotes.begin(); it != remotes.end(); ++it) {
    if (it->serial == serial) {
      RemoteEntry removed = *it;
      remotes.erase(it);
      String err;
      // Security-critical path: persist remote immediately even during motion.
      bool ok = saveInternal(&err, true);
      lastRemotesSaveMs = millis();
      lastRemotesSaveOk = ok;
      lastRemotesSaveError = ok ? "" : err;
      if (!ok) {
        remotes.push_back(removed);
      }
      return ok;
    }
  }
  return false;
}

void ConfigManager::clearRemotes() {
  std::vector<RemoteEntry> backup = remotes;
  remotes.clear();
  String err;
  // Security-critical path: persist remote immediately even during motion.
  bool ok = saveInternal(&err, true);
  lastRemotesSaveMs = millis();
  lastRemotesSaveOk = ok;
  lastRemotesSaveError = ok ? "" : err;
  if (!ok) {
    remotes = backup;
  }
}

void ConfigManager::touchRemote(unsigned long serial, unsigned long counter, unsigned long seenMs) {
  for (auto& r : remotes) {
    if (r.serial == serial) {
      r.lastCounter = counter;
      r.lastSeenMs = seenMs;
      return;
    }
  }
}

bool ConfigManager::isAuthorized(unsigned long serial) const {
  for (const auto& r : remotes) {
    if (r.serial == serial && r.enabled) return true;
  }
  return false;
}

bool ConfigManager::getRemote(unsigned long serial, RemoteEntry& out) const {
  for (const auto& r : remotes) {
    if (r.serial == serial) {
      out = r;
      return true;
    }
  }
  return false;
}
