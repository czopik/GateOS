#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <map>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config_manager.h"
#include "wifi_manager.h"
#include "motor_controller.h"
#include "gate_controller.h"
#include "hcs301_receiver.h"
#include "web_server.h"
#include "mqtt_manager.h"
#include "calibration_manager.h"
#include "led_controller.h"

ConfigManager config;
MotorController* motor = nullptr;
GateController* gate = nullptr;
HCS301Receiver* hcs = nullptr;
WebServerManager webserver(&config);
CalibrationManager calibration;

MqttManager mqtt;
LedController led;

struct RemoteSeen {
  unsigned long encript = 0;
  unsigned long lastMs = 0;

  // Debounce "press" events from the remote (many frames per one click)
  unsigned long lastActionMs = 0;
  unsigned long lastActionEncript = 0;
};

struct LastRemote {
  unsigned long serial = 0;
  unsigned long encript = 0;
  bool btnToggle = false;
  bool btnGreen = false;
  bool batteryLow = false;
  unsigned long ts = 0;
  bool known = false;
  bool authorized = false;
};

struct EventEntry {
  unsigned long ts = 0;
  char level[8] = {0};
  char code[16] = {0};
  char message[64] = {0};
};

struct DebouncedInput {
  int pin = -1;
  bool invert = false;
  unsigned long debounceMs = 30;
  bool stableState = false;
  bool lastRaw = false;
  unsigned long lastChange = 0;

  void begin(int pin_, bool invert_, unsigned long debounceMs_, int pullMode) {
    pin = pin_;
    invert = invert_;
    debounceMs = debounceMs_;
    if (pin >= 0) {
      if (pullMode == 2) pinMode(pin, INPUT_PULLDOWN);
      else if (pullMode == 1) pinMode(pin, INPUT_PULLUP);
      else pinMode(pin, INPUT);
    }
    lastRaw = readRaw();
    stableState = lastRaw;
    lastChange = millis();
  }

  bool readRaw() const {
    if (pin < 0) return false;
    bool raw = digitalRead(pin) == HIGH;
    return invert ? !raw : raw;
  }

  bool update() {
    if (pin < 0) return false;
    bool raw = readRaw();
    if (raw != lastRaw) {
      lastRaw = raw;
      lastChange = millis();
    }
    if (stableState != lastRaw && millis() - lastChange >= debounceMs) {
      stableState = lastRaw;
      return true;
    }
    return false;
  }

  bool isActive() const { return stableState; }
};

static int parsePullMode(const String& mode) {
  if (mode == "down") return 2;
  if (mode == "none") return 0;
  return 1;
}

struct Ld2410Status {
  bool available = false;
  bool present = false;
  bool moving = false;
  bool stationary = false;
  int distanceCm = -1;
  unsigned long lastUpdateMs = 0;
};

static std::map<unsigned long, RemoteSeen> lastRemoteMap;
static LastRemote lastRemote;
static bool learnMode = false;
static const int kMaxEvents = 80;
static EventEntry events[kMaxEvents];
static int eventHead = 0;
static int eventCount = 0;
static unsigned long lastStatusMs = 0;

static DebouncedInput limitOpenInput;
static DebouncedInput limitCloseInput;
static DebouncedInput stopInput;
static DebouncedInput obstacleInput;
static DebouncedInput buttonInput;

// Request one-shot resync of local position/hall counters when a limit is hit.
static bool resyncAtOpenLimit = false;
static bool resyncAtCloseLimit = false;

static bool otaReady = false;
static unsigned long lastMqttPublish = 0;
static unsigned long lastMqttTelemetryMs = 0;
static bool mqttWasConnected = false;
static unsigned long lastCalibWsMs = 0;

static volatile long hallCount = 0;
static portMUX_TYPE hallMux = portMUX_INITIALIZER_UNLOCKED;
static long hallCountLast = 0;
static long hallPosition = 0;
static volatile uint32_t hallLastIsrUs = 0;
static volatile uint32_t hallDebounceUs = 0;
static float hallPps = 0.0f;
static unsigned long hallPpsLastMs = 0;
static long hallPpsLastCount = 0;
static int positionPercent = -1;
static float positionMeters = 0.0f;
static float maxDistanceMeters = 0.0f;
static GateState lastGateState = GATE_STOPPED;
static unsigned long moveStartMs = 0;
static float moveStartPosition = 0.0f;
static int hallPinActive = -1;
static bool hallAttached = false;
static unsigned long lastPositionPersistMs = 0;
static float lastPersistedPosition = -1.0f;
static Ld2410Status ld2410Status;


static long readHallCountAtomic() {
  long v;
  portENTER_CRITICAL(&hallMux);
  v = hallCount;
  portEXIT_CRITICAL(&hallMux);
  return v;
}

TaskHandle_t gateTaskHandle = NULL;

void handleControlCmd(const char* action);
void mqttPublishEvent(const char* level, const char* message);
void mqttPublishLedState();
void mqttPublishTelemetry();
void mqttPublishPosition();
void handleLedCmd(const char* payload);
bool applyMaxDistance(float value, bool persist);
bool handleGateCalibrate(const char* mode);
void maybePersistPosition(uint32_t nowMs);
void updateHallAttachment();
void updatePositionPercent();
void onGateStatusChanged(const GateStatus& status, void* ctx);
void fillDiagnostics(JsonObject& out);
void IRAM_ATTR hallIsr();

void pushEvent(const char* level, const char* code, const char* message) {
  EventEntry& e = events[eventHead];
  e.ts = millis();
  strncpy(e.level, level, sizeof(e.level) - 1);
  e.level[sizeof(e.level) - 1] = '\0';
  strncpy(e.code, code ? code : "", sizeof(e.code) - 1);
  e.code[sizeof(e.code) - 1] = '\0';
  e.level[sizeof(e.level) - 1] = '\0';
  strncpy(e.message, message, sizeof(e.message) - 1);
  e.message[sizeof(e.message) - 1] = '\0';
  eventHead = (eventHead + 1) % kMaxEvents;
  if (eventCount < kMaxEvents) eventCount++;
  webserver.broadcastEvent(level, message);
  mqttPublishEvent(level, message);
}

void pushEvent(const char* level, const char* message) {
  pushEvent(level, "", message);
}

void pushEventf(const char* level, const char* fmt, unsigned long value) {
  char buf[64];
  snprintf(buf, sizeof(buf), fmt, value);
  pushEvent(level, buf);
}

void mqttPublishEvent(const char* level, const char* message) {
  const char* topic = mqtt.topicEvents();
  if (!mqtt.connected() || !topic || topic[0] == '\0') return;
  StaticJsonDocument<192> doc;
  doc["level"] = level;
  doc["message"] = message;
  doc["ts"] = millis();
  char payload[192];
  serializeJson(doc, payload, sizeof(payload));
  mqtt.publish(topic, payload, config.mqttConfig.retain);
}

void mqttPublishStatus() {
  const char* topic = mqtt.topicState();
  if (!mqtt.connected() || !topic || topic[0] == '\0') return;
  StaticJsonDocument<256> doc;
  doc["state"] = gate ? gate->getStateString() : "unknown";
  doc["moving"] = gate ? gate->isMoving() : false;
  doc["uptimeMs"] = millis();
  doc["wifiRssi"] = WiFiManager.isConnected() ? WiFi.RSSI() : 0;
  char payload[256];
  serializeJson(doc, payload, sizeof(payload));
  mqtt.publish(topic, payload, config.mqttConfig.retain);
}

void mqttPublishLedState() {
  const char* topic = mqtt.topicLedState();
  if (!mqtt.connected() || !topic || topic[0] == '\0') return;
  StaticJsonDocument<256> doc;
  JsonObject root = doc.to<JsonObject>();
  led.fillStatus(root);
  root["ts"] = millis();
  char payload[256];
  serializeJson(doc, payload, sizeof(payload));
  mqtt.publish(topic, payload, config.mqttConfig.retain);
}

void mqttPublishTelemetry() {
  const char* topic = mqtt.topicTelemetry();
  if (!mqtt.connected() || !topic || topic[0] == '\0') return;
  StaticJsonDocument<768> doc;
  JsonObject gateObj = doc.createNestedObject("gate");
  if (gate) {
    const GateStatus& st = gate->getStatus();
    gateObj["state"] = gate->getStateString();
    gateObj["moving"] = gate->isMoving();
    gateObj["position"] = st.position;
    gateObj["positionPercent"] = st.positionPercent;
    gateObj["targetPosition"] = st.targetPosition;
    gateObj["maxDistance"] = st.maxDistance;
    gateObj["errorCode"] = static_cast<int>(st.error);
  } else {
    gateObj["state"] = "unknown";
    gateObj["moving"] = false;
    gateObj["position"] = positionMeters;
    gateObj["positionPercent"] = positionPercent;
    gateObj["targetPosition"] = 0.0f;
    gateObj["maxDistance"] = maxDistanceMeters;
    gateObj["errorCode"] = 0;
  }

  JsonObject hbObj = doc.createNestedObject("hb");
  if (motor && motor->isHoverUart() && motor->hoverEnabled()) {
    hbObj["enabled"] = true;
    const HoverTelemetry& tel = motor->hoverTelemetry();
    hbObj["dir"] = tel.dir;
    hbObj["rpm"] = tel.rpm;
    hbObj["dist_mm"] = tel.distMm;
    hbObj["batValid"] = tel.batValid;
    hbObj["rawBat"] = tel.rawBat;
    hbObj["batScale"] = tel.batScale;
    if (tel.batValid) hbObj["batV"] = tel.batV;
    else hbObj["batV"] = nullptr;
    // expose raw centi-volts if available
    hbObj["bat_cV"] = tel.bat_cV;
    // current in A (float) if available, else -1
    if (tel.iA_x100 >= 0) hbObj["iA"] = ((float)tel.iA_x100) / 100.0f;
    else hbObj["iA"] = -1.0f;
    hbObj["armed"] = tel.armed;
    hbObj["fault"] = tel.fault;
    hbObj["lastTelMs"] = tel.lastTelMs;
  } else {
    hbObj["enabled"] = false;
    hbObj["dir"] = 0;
    hbObj["rpm"] = 0;
    hbObj["dist_mm"] = 0;
    hbObj["batValid"] = false;
    hbObj["rawBat"] = -1;
    hbObj["batScale"] = 0;
    hbObj["batV"] = nullptr;
    hbObj["bat_cV"] = -1;
    hbObj["iA"] = -1.0f;
    hbObj["armed"] = false;
    hbObj["fault"] = 0;
    hbObj["lastTelMs"] = 0;
  }

  doc["ts"] = millis();
  char payload[512];
  serializeJson(doc, payload, sizeof(payload));
  mqtt.publish(topic, payload, config.mqttConfig.retain);
}

void mqttPublishPosition() {
  const char* topic = mqtt.topicPosition();
  if (!mqtt.connected() || !topic || topic[0] == '\0') return;
  char payload[32];
  snprintf(payload, sizeof(payload), "%.3f", positionMeters);
  mqtt.publish(topic, payload, config.mqttConfig.retain);
}

void mqttPublishMotionState() {
  const char* topic = mqtt.topicMotionState();
  if (!mqtt.connected() || !topic || topic[0] == '\0') return;
  StaticJsonDocument<256> doc;
  JsonObject gateObj = doc.createNestedObject("gate");
  if (gate) {
    const GateStatus& st = gate->getStatus();
    gateObj["state"] = gate->getStateString();
    gateObj["moving"] = gate->isMoving();
    gateObj["position"] = st.position;
    gateObj["positionPercent"] = st.positionPercent;
    gateObj["targetPosition"] = st.targetPosition;
    gateObj["maxDistance"] = st.maxDistance;
    gateObj["lastDirection"] = gate->getLastDirection();
    gateObj["errorCode"] = static_cast<int>(st.error);
  } else {
    gateObj["state"] = "unknown";
    gateObj["moving"] = false;
  }
  doc["ts"] = millis();
  char payload[256];
  serializeJson(doc, payload, sizeof(payload));
  mqtt.publish(topic, payload, config.mqttConfig.retain);
}

void mqttPublishMotionPosition() {
  const char* topic = mqtt.topicMotionPosition();
  if (!mqtt.connected() || !topic || topic[0] == '\0') return;
  StaticJsonDocument<128> doc;
  long mm = (long)(positionMeters * 1000.0f);
  doc["mm"] = mm;
  doc["percent"] = positionPercent;
  doc["state"] = gate ? gate->getStateString() : "unknown";
  doc["ts"] = millis();
  char payload[128];
  serializeJson(doc, payload, sizeof(payload));
  mqtt.publish(topic, payload, config.mqttConfig.retain);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (!topic || length == 0) return;
  const char* cmdTopic = mqtt.topicCommand();
  const char* ledTopic = mqtt.topicLedCmd();
  const char* gateCmdTopic = mqtt.topicGateCmd();
  const char* setMaxTopic = mqtt.topicGateSetMax();
  const char* calTopic = mqtt.topicGateCalibrate();
  bool isCmd = cmdTopic && cmdTopic[0] != '\0' && strcmp(topic, cmdTopic) == 0;
  bool isLed = ledTopic && ledTopic[0] != '\0' && strcmp(topic, ledTopic) == 0;
  bool isGateCmd = gateCmdTopic && gateCmdTopic[0] != '\0' && strcmp(topic, gateCmdTopic) == 0;
  bool isSetMax = setMaxTopic && setMaxTopic[0] != '\0' && strcmp(topic, setMaxTopic) == 0;
  bool isCalibrate = calTopic && calTopic[0] != '\0' && strcmp(topic, calTopic) == 0;
  if (!isCmd && !isLed && !isGateCmd && !isSetMax && !isCalibrate) return;

  char buf[128];
  unsigned int n = length > sizeof(buf) - 1 ? sizeof(buf) - 1 : length;
  memcpy(buf, payload, n);
  buf[n] = '\0';

  if (isLed) {
    handleLedCmd(buf);
    return;
  }

  if (isSetMax) {
    float value = 0.0f;
    bool ok = false;
    if (buf[0] == '{') {
      StaticJsonDocument<128> doc;
      if (deserializeJson(doc, buf) == DeserializationError::Ok) {
        if (doc.containsKey("maxDistance")) {
          value = doc["maxDistance"].as<float>();
          ok = true;
        } else if (doc.containsKey("value")) {
          value = doc["value"].as<float>();
          ok = true;
        }
      }
    } else {
      value = (float)atof(buf);
      ok = value > 0.0f;
    }
    if (ok) {
      if (applyMaxDistance(value, true)) {
        mqttPublishTelemetry();
        mqttPublishPosition();
      } else {
        pushEvent("warn", "mqtt set_max_distance failed");
      }
    } else {
      pushEvent("warn", "mqtt set_max_distance invalid");
    }
    return;
  }

  if (isCalibrate) {
    const char* mode = nullptr;
    if (buf[0] == '{') {
      StaticJsonDocument<128> doc;
      if (deserializeJson(doc, buf) == DeserializationError::Ok) {
        mode = doc["set"] | "";
      }
    } else {
      mode = buf;
    }
    if (mode && mode[0] != '\0') {
      if (handleGateCalibrate(mode)) {
        mqttPublishTelemetry();
        mqttPublishPosition();
      } else {
        pushEvent("warn", "mqtt calibrate failed");
      }
    }
    return;
  }

  if (buf[0] == '{') {
    StaticJsonDocument<128> doc;
    if (deserializeJson(doc, buf) == DeserializationError::Ok) {
      const char* action = doc["action"] | "";
      if (action[0] != '\0') handleControlCmd(action);
    }
    return;
  }
  if (isCmd || isGateCmd) {
    handleControlCmd(buf);
  }
}

bool applyMaxDistance(float value, bool persist) {
  if (value <= 0.0f || value > 100.0f) return false;
  maxDistanceMeters = value;
  if (positionMeters < 0.0f) positionMeters = 0.0f;
  if (positionMeters > maxDistanceMeters) positionMeters = maxDistanceMeters;
  positionPercent = maxDistanceMeters > 0.0f ?
    (int)((positionMeters * 100.0f) / maxDistanceMeters + 0.5f) : -1;

  config.gateConfig.maxDistance = maxDistanceMeters;
  config.gateConfig.totalDistance = maxDistanceMeters;
  config.gateConfig.position = positionMeters;

  if (config.sensorsConfig.hall.enabled &&
      config.sensorsConfig.hall.pin >= 0 &&
      config.gateConfig.wheelCircumference > 0.0f &&
      config.gateConfig.pulsesPerRevolution > 0) {
    float pulsesPerMeter = (float)config.gateConfig.pulsesPerRevolution / config.gateConfig.wheelCircumference;
    long totalCounts = (long)(maxDistanceMeters * pulsesPerMeter);
    hallPosition = (long)(positionMeters * pulsesPerMeter);
    if (hallPosition < 0) hallPosition = 0;
    if (hallPosition > totalCounts) hallPosition = totalCounts;
  }

  if (gate) gate->setPosition(positionMeters, maxDistanceMeters);

  if (persist) {
    String err;
    if (!config.save(&err)) {
      Serial.printf("maxDistance save failed: %s\n", err.c_str());
      return false;
    }
    lastPersistedPosition = positionMeters;
    lastPositionPersistMs = millis();
  }
  return true;
}

bool handleGateCalibrate(const char* mode) {
  if (!mode || mode[0] == '\0') return false;
  if (gate && gate->isMoving()) return false;
  if (calibration.isRunning()) return false;

  char key[8];
  size_t n = strlen(mode);
  if (n >= sizeof(key)) n = sizeof(key) - 1;
  for (size_t i = 0; i < n; ++i) {
    char c = mode[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    key[i] = c;
  }
  key[n] = '\0';

  bool setZero = strcmp(key, "zero") == 0 || strcmp(key, "close") == 0 || strcmp(key, "closed") == 0;
  bool setMax = strcmp(key, "max") == 0 || strcmp(key, "open") == 0;
  if (!setZero && !setMax) return false;

  float maxDistance = config.gateConfig.maxDistance > 0.0f ?
    config.gateConfig.maxDistance : config.gateConfig.totalDistance;
  if (maxDistance <= 0.0f) return false;

  maxDistanceMeters = maxDistance;
  positionMeters = setZero ? 0.0f : maxDistanceMeters;
  positionPercent = maxDistanceMeters > 0.0f ?
    (int)((positionMeters * 100.0f) / maxDistanceMeters + 0.5f) : -1;

  if (config.sensorsConfig.hall.enabled &&
      config.sensorsConfig.hall.pin >= 0 &&
      config.gateConfig.wheelCircumference > 0.0f &&
      config.gateConfig.pulsesPerRevolution > 0) {
    float pulsesPerMeter = (float)config.gateConfig.pulsesPerRevolution / config.gateConfig.wheelCircumference;
    long totalCounts = (long)(maxDistanceMeters * pulsesPerMeter);
    hallPosition = setZero ? 0 : totalCounts;
  }

  config.gateConfig.maxDistance = maxDistanceMeters;
  config.gateConfig.totalDistance = maxDistanceMeters;
  config.gateConfig.position = positionMeters;

  if (gate) gate->setPosition(positionMeters, maxDistanceMeters);

  String err;
  if (!config.save(&err)) {
    Serial.printf("calibrate save failed: %s\n", err.c_str());
    return false;
  }
  lastPersistedPosition = positionMeters;
  lastPositionPersistMs = millis();
  return true;
}

void maybePersistPosition(uint32_t nowMs) {
  if (!gate || gate->isMoving()) return;
  if (maxDistanceMeters <= 0.0f) return;
  if (lastPositionPersistMs != 0 && nowMs - lastPositionPersistMs < 5000) return;
  if (lastPersistedPosition >= 0.0f) {
    float diff = positionMeters - lastPersistedPosition;
    if (diff < 0.0f) diff = -diff;
    if (diff < 0.01f) return;
  }
  config.gateConfig.position = positionMeters;
  config.gateConfig.maxDistance = maxDistanceMeters;
  config.gateConfig.totalDistance = maxDistanceMeters;
  String err;
  if (!config.save(&err)) {
    Serial.printf("position save failed: %s\n", err.c_str());
    return;
  }
  lastPersistedPosition = positionMeters;
  lastPositionPersistMs = nowMs;
}

void setupInputs() {
  const auto& limOpen = config.limitsConfig.open;
  const auto& limClose = config.limitsConfig.close;
  limitOpenInput.begin(limOpen.pin, limOpen.invert, limOpen.debounceMs, parsePullMode(limOpen.pullMode));
  limitCloseInput.begin(limClose.pin, limClose.invert, limClose.debounceMs, parsePullMode(limClose.pullMode));
  stopInput.begin(config.gpioConfig.stopPin, false, 30, 1);
  const auto& photo = config.sensorsConfig.photocell;
  int photoPin = photo.pin;
  bool photoInvert = photo.invert;
  int photoPull = parsePullMode(photo.pullMode);
  if (photoPin < 0) {
    photoPin = config.gpioConfig.obstaclePin;
    photoInvert = config.gpioConfig.obstacleInvert;
    photoPull = 1;
  }
  obstacleInput.begin(photoPin, photoInvert, photo.debounceMs, photoPull);
  buttonInput.begin(config.gpioConfig.buttonPin, config.gpioConfig.buttonInvert, 50, 1);
}

void handleInputs() {
  if (!gate) return;

  const bool limitsEnabled = config.limitsConfig.enabled;
  const bool limitOpenEnabled = limitsEnabled && config.limitsConfig.open.enabled;
  const bool limitCloseEnabled = limitsEnabled && config.limitsConfig.close.enabled;
  const bool photocellEnabled = config.sensorsConfig.photocell.enabled;

  if (stopInput.update() && stopInput.isActive()) {
    gate->onStopInput();
    pushEvent("warn", "stop input");
    led.setOverride("stopped", 1500);
  }

  if (obstacleInput.update()) {
    if (photocellEnabled) {
      GateCommandResponse r = gate->onObstacle(obstacleInput.isActive());
      if (obstacleInput.isActive()) {
        if (r.result == GATE_CMD_OK && r.applied) {
          if (r.cmd == GATE_CMD_OPEN) pushEvent("warn", "obstacle -> open");
          else if (r.cmd == GATE_CMD_CLOSE) pushEvent("warn", "obstacle -> close");
          else if (r.cmd == GATE_CMD_STOP) pushEvent("warn", "obstacle -> stop");
          else pushEvent("warn", "obstacle");
          led.setOverride("obstacle", 2000);
        } else if (r.result == GATE_CMD_BLOCKED) {
          pushEvent("warn", "obstacle action blocked");
        }
      }
    }
  }

  if (limitsEnabled) {
    static unsigned long limitsInvalidSinceMs = 0;
    bool openActive = limitOpenEnabled && limitOpenInput.isActive();
    bool closeActive = limitCloseEnabled && limitCloseInput.isActive();
    bool both = openActive && closeActive;
    if (both) {
      if (limitsInvalidSinceMs == 0) limitsInvalidSinceMs = millis();
      if (millis() - limitsInvalidSinceMs >= 200) {
        if (gate->getErrorCode() != GATE_ERR_LIMITS_INVALID) {
          pushEvent("error", "limits_invalid (both active)");
        }
        gate->onLimitsInvalid();
      }
    } else {
      limitsInvalidSinceMs = 0;
    }

    if (limitOpenEnabled && limitOpenInput.update() && limitOpenInput.isActive()) {
      gate->onLimitOpen();
      resyncAtOpenLimit = true;
      pushEvent("info", "limit open");
    }

    if (limitCloseEnabled && limitCloseInput.update() && limitCloseInput.isActive()) {
      gate->onLimitClose();
      resyncAtCloseLimit = true;
      pushEvent("info", "limit close");
    }
  }

  if (buttonInput.update() && buttonInput.isActive()) {
    GateCommandResponse r = gate->handleCommand("toggle");
    if (r.result == GATE_CMD_OK) {
      pushEvent("info", r.applied ? "button toggle" : "button toggle (no-op)");
    } else if (r.result == GATE_CMD_BLOCKED) {
      pushEvent("warn", "button toggle blocked");
    }
  }
}

void IRAM_ATTR hallIsr() {
  uint32_t nowUs = micros();
  uint32_t debounceUs = hallDebounceUs;
  if (debounceUs > 0) {
    uint32_t last = hallLastIsrUs;
    if (nowUs - last < debounceUs) return;
    hallLastIsrUs = nowUs;
  }
  portENTER_CRITICAL_ISR(&hallMux);
  hallCount++;
  portEXIT_CRITICAL_ISR(&hallMux);
}

void updateHallAttachment() {
  int pin = config.sensorsConfig.hall.pin;
  bool enabled = config.sensorsConfig.hall.enabled && pin >= 0;
  hallDebounceUs = (uint32_t)config.sensorsConfig.hall.debounceMs * 1000U;

  if (calibration.isRunning()) {
    hallAttached = false;
    hallPinActive = pin;
    return;
  }

  if (!enabled) {
    if (hallAttached && hallPinActive >= 0) {
      detachInterrupt(hallPinActive);
    }
    hallAttached = false;
    hallPinActive = pin;
    return;
  }

  if (hallAttached && hallPinActive == pin) return;
  if (hallAttached && hallPinActive >= 0) {
    detachInterrupt(hallPinActive);
  }
  hallPinActive = pin;
  int intNum = digitalPinToInterrupt(pin);
  if (intNum < 0) {
    hallAttached = false;
    return;
  }
  int pull = parsePullMode(config.sensorsConfig.hall.pullMode);
  pinMode(pin, pull == 2 ? INPUT_PULLDOWN : (pull == 1 ? INPUT_PULLUP : INPUT));
  int mode = config.sensorsConfig.hall.invert ? FALLING : RISING;
  attachInterrupt(intNum, hallIsr, mode);
  hallAttached = true;
  hallCountLast = readHallCountAtomic();
  float maxDistance = config.gateConfig.maxDistance > 0.0f ?
    config.gateConfig.maxDistance : config.gateConfig.totalDistance;
  if (maxDistance < 0.0f) maxDistance = 0.0f;
  if (maxDistance > 0.0f &&
      config.gateConfig.wheelCircumference > 0.0f &&
      config.gateConfig.pulsesPerRevolution > 0) {
    float pulsesPerMeter = (float)config.gateConfig.pulsesPerRevolution / config.gateConfig.wheelCircumference;
    long totalCounts = (long)(maxDistance * pulsesPerMeter);
    hallPosition = (long)(positionMeters * pulsesPerMeter);
    if (hallPosition < 0) hallPosition = 0;
    if (hallPosition > totalCounts) hallPosition = totalCounts;
  }
}

void updatePositionPercent() {
  if (!gate) return;

  auto syncConfigPosition = []() {
    config.gateConfig.position = positionMeters;
    config.gateConfig.maxDistance = maxDistanceMeters;
    config.gateConfig.totalDistance = maxDistanceMeters;
  };

  float maxDistance = config.gateConfig.maxDistance > 0.0f ?
    config.gateConfig.maxDistance : config.gateConfig.totalDistance;
  if (maxDistance < 0.0f) maxDistance = 0.0f;
  maxDistanceMeters = maxDistance;

  if (positionMeters < 0.0f) positionMeters = 0.0f;
  if (maxDistanceMeters > 0.0f && positionMeters > maxDistanceMeters) {
    positionMeters = maxDistanceMeters;
  }

  float pulsesPerMeter = 0.0f;
  long totalCounts = 0;
  if (config.sensorsConfig.hall.enabled &&
      config.sensorsConfig.hall.pin >= 0 &&
      maxDistanceMeters > 0.0f &&
      config.gateConfig.wheelCircumference > 0.0f &&
      config.gateConfig.pulsesPerRevolution > 0) {
    pulsesPerMeter = (float)config.gateConfig.pulsesPerRevolution / config.gateConfig.wheelCircumference;
    totalCounts = (long)(maxDistanceMeters * pulsesPerMeter);
  }

  // One-shot hard resync when a limit edge is detected (handled via GateController).
  if (resyncAtCloseLimit) {
    positionMeters = 0.0f;
    if (totalCounts > 0) hallPosition = 0;
    resyncAtCloseLimit = false;
  }
  if (resyncAtOpenLimit) {
    positionMeters = maxDistanceMeters;
    if (totalCounts > 0) hallPosition = totalCounts;
    resyncAtOpenLimit = false;
  }

  if (calibration.isRunning()) {
    syncConfigPosition();
    gate->setPosition(positionMeters, maxDistanceMeters);
    return;
  }

  if (hallAttached && totalCounts > 0 && pulsesPerMeter > 0.0f) {
    long current = readHallCountAtomic();
    long delta = current - hallCountLast;
    hallCountLast = current;
    if (gate->isMoving() && delta != 0) {
      int dir = gate->getLastDirection() >= 0 ? 1 : -1;
      hallPosition += delta * dir;
      if (hallPosition < 0) hallPosition = 0;
      if (hallPosition > totalCounts) hallPosition = totalCounts;
      positionMeters = (float)hallPosition / pulsesPerMeter;
    }
    positionPercent = (maxDistanceMeters > 0.0f) ?
      (int)((positionMeters * 100.0f) / maxDistanceMeters + 0.5f) : -1;
    syncConfigPosition();
    gate->setPosition(positionMeters, maxDistanceMeters);
    return;
  }

    if (motor && motor->isHoverUart() && motor->hoverEnabled()) {
    const HoverTelemetry& tel = motor->hoverTelemetry();
    const uint32_t now = millis();

    // Basic telemetry watchdog
    if (tel.lastTelMs != 0 && !motor->hoverTelemetryTimedOut(now, 1000)) {
      // Hoverboard telemetry distance is a continuous counter and can change sign depending on wiring/dir.
      // We therefore track position RELATIVE to the movement start (offset) and apply direction + clamp.
      const bool movingNow = gate->isMoving();
      const int gateDir = gate->getLastDirection(); // +1 opening, -1 closing

      static bool hoverTrackActive = false;
      static long hoverDistStartMm = 0;
      static float hoverPosStartM = 0.0f;
      static int hoverTrackDir = 0;

      if (!movingNow) {
        // Not moving -> do NOT overwrite position from tel.distMm (it is not absolute 0..max).
        hoverTrackActive = false;
      } else {
        // Movement start snapshot (or direction change)
        if (!hoverTrackActive || hoverTrackDir != gateDir) {
          hoverDistStartMm = tel.distMm;
          hoverPosStartM = positionMeters;   // start from our last known position
          hoverTrackDir = gateDir;
          hoverTrackActive = true;
        }

        // Only update position from distance when the hoverboard is armed.
        // If not armed, keep the last known position (prevents jumping due to tel.distMm being large).
        if (tel.armed) {
          const long relMm = tel.distMm - hoverDistStartMm;
          const float relM = fabsf((float)relMm) / 1000.0f;

          float nextPos = hoverPosStartM;
          if (hoverTrackDir > 0) nextPos = hoverPosStartM + relM;
          else if (hoverTrackDir < 0) nextPos = hoverPosStartM - relM;

          // Clamp to [0..max]
          if (nextPos < 0.0f) nextPos = 0.0f;
          if (maxDistanceMeters > 0.0f && nextPos > maxDistanceMeters) nextPos = maxDistanceMeters;

          positionMeters = nextPos;
        }

        positionPercent = (maxDistanceMeters > 0.0f) ?
          (int)((positionMeters * 100.0f) / maxDistanceMeters + 0.5f) : -1;
        syncConfigPosition();
        gate->setPosition(positionMeters, maxDistanceMeters);
        return;
      }
    }
    // Telemetry missing/timed out: fall through to keep last position.
  }

  // No sensor/telemetry available: keep last known position.
  positionPercent = (maxDistanceMeters > 0.0f) ?
    (int)((positionMeters * 100.0f) / maxDistanceMeters + 0.5f) : -1;
  syncConfigPosition();
  gate->setPosition(positionMeters, maxDistanceMeters);
}
void updateHallStats(uint32_t nowMs) {
  if (!config.sensorsConfig.hall.enabled || config.sensorsConfig.hall.pin < 0) {
    hallPps = 0.0f;
    hallPpsLastMs = nowMs;
    hallPpsLastCount = readHallCountAtomic();
    return;
  }
  if (hallPpsLastMs == 0) {
    hallPpsLastMs = nowMs;
    hallPpsLastCount = readHallCountAtomic();
    return;
  }
  uint32_t deltaMs = nowMs - hallPpsLastMs;
  if (deltaMs < 1000) return;
  long count = readHallCountAtomic();
  long delta = count - hallPpsLastCount;
  hallPps = deltaMs > 0 ? (float)delta / (deltaMs / 1000.0f) : 0.0f;
  hallPpsLastMs = nowMs;
  hallPpsLastCount = count;
}

void updateLd2410Status(uint32_t nowMs) {
  const auto& cfg = config.sensorsConfig.ld2410;
  bool enabled = cfg.enabled && cfg.rxPin >= 0 && cfg.txPin >= 0;
  if (!enabled) {
    ld2410Status.available = false;
    ld2410Status.present = false;
    ld2410Status.moving = false;
    ld2410Status.stationary = false;
    ld2410Status.distanceCm = -1;
    ld2410Status.lastUpdateMs = 0;
    return;
  }
  if (ld2410Status.lastUpdateMs == 0 || nowMs - ld2410Status.lastUpdateMs > 2000) {
    ld2410Status.available = false;
    ld2410Status.present = false;
    ld2410Status.moving = false;
    ld2410Status.stationary = false;
    ld2410Status.distanceCm = -1;
  }
}

void onGateStatusChanged(const GateStatus& status, void* ctx) {
  (void)ctx;
  led.setState(status.state,
               status.error,
               status.obstacle,
               status.wifiConnected,
               status.mqttConnected,
               status.positionPercent,
               status.apMode,
               status.otaInProgress);
}

void fillDiagnostics(JsonObject& out) {
  JsonObject hoverObj = out.createNestedObject("hoverUart");
  if (motor && motor->isHoverUart() && motor->hoverEnabled()) {
    const HoverTelemetry& tel = motor->hoverTelemetry();
    hoverObj["enabled"] = true;
    hoverObj["lastTelMs"] = tel.lastTelMs;
    hoverObj["rpm"] = tel.rpm;
    hoverObj["dist_mm"] = tel.distMm;
    hoverObj["armed"] = tel.armed;
    hoverObj["cmd_age_ms"] = tel.cmd_age_ms;
    hoverObj["batValid"] = tel.batValid;
    hoverObj["rawBat"] = tel.rawBat;
    hoverObj["batScale"] = tel.batScale;
    if (tel.batValid) hoverObj["batV"] = tel.batV;
    else hoverObj["batV"] = nullptr;
    hoverObj["fault"] = tel.fault;
    hoverObj["lastCmdSpeed"] = motor->hoverLastCmdSpeed();
  } else {
    hoverObj["enabled"] = false;
  }

  JsonObject remotesObj = out.createNestedObject("remotes");
  remotesObj["lastSaveOk"] = config.getLastRemotesSaveOk();
  remotesObj["lastSaveMs"] = config.getLastRemotesSaveMs();
  remotesObj["lastSaveError"] = config.getLastRemotesSaveError();
}

void fillStatus(JsonObject& out) {
  out["uptimeMs"] = millis();

  JsonObject gateObj = out.createNestedObject("gate");
  if (gate) {
    const GateStatus& st = gate->getStatus();
    gateObj["state"] = gate->getStateString();
    gateObj["moving"] = gate->isMoving();
    gateObj["position"] = st.position;
    gateObj["positionPercent"] = st.positionPercent;
    gateObj["targetPosition"] = st.targetPosition;
    gateObj["maxDistance"] = st.maxDistance;
    gateObj["lastDirection"] = gate->getLastDirection();
    gateObj["errorCode"] = static_cast<int>(st.error);
    gateObj["obstacle"] = st.obstacle;
    gateObj["lastMoveMs"] = st.lastMoveMs;
    gateObj["lastStateChangeMs"] = st.lastStateChangeMs;
  } else {
    gateObj["state"] = "unknown";
    gateObj["moving"] = false;
    gateObj["position"] = positionMeters;
    gateObj["positionPercent"] = positionPercent;
    gateObj["targetPosition"] = 0.0f;
    gateObj["maxDistance"] = maxDistanceMeters;
    gateObj["lastDirection"] = 0;
    gateObj["errorCode"] = 0;
    gateObj["obstacle"] = false;
    gateObj["lastMoveMs"] = 0;
    gateObj["lastStateChangeMs"] = 0;
  }

  JsonObject wifiObj = out.createNestedObject("wifi");
  bool wifiConnected = WiFiManager.isConnected();
  wifiObj["connected"] = wifiConnected;
  wifiObj["mode"] = WiFiManager.getModeString();
  wifiObj["ssid"] = wifiConnected ? WiFi.SSID() : "";
  wifiObj["ip"] = wifiConnected ? WiFi.localIP().toString() : "";
  wifiObj["rssi"] = wifiConnected ? WiFi.RSSI() : 0;
  wifiObj["apMode"] = WiFiManager.getModeString() == "AP";

  JsonObject mqttObj = out.createNestedObject("mqtt");
  mqttObj["connected"] = mqtt.connected();

  JsonObject hbObj = out.createNestedObject("hb");
  if (motor && motor->isHoverUart() && motor->hoverEnabled()) {
    const HoverTelemetry& tel = motor->hoverTelemetry();
    hbObj["enabled"] = true;
    hbObj["dir"] = tel.dir;
    hbObj["rpm"] = tel.rpm;
    hbObj["dist_mm"] = tel.distMm;
    hbObj["batValid"] = tel.batValid;
    hbObj["rawBat"] = tel.rawBat;
    hbObj["batScale"] = tel.batScale;
    if (tel.batValid) hbObj["batV"] = tel.batV;
    else hbObj["batV"] = nullptr;
    hbObj["fault"] = tel.fault;
    hbObj["lastTelMs"] = tel.lastTelMs;
  } else {
    hbObj["enabled"] = false;
    hbObj["dir"] = 0;
    hbObj["rpm"] = 0;
    hbObj["dist_mm"] = 0;
    hbObj["batValid"] = false;
    hbObj["rawBat"] = -1;
    hbObj["batScale"] = 0;
    hbObj["batV"] = nullptr;
    hbObj["fault"] = 0;
    hbObj["lastTelMs"] = 0;
  }

  JsonObject ledObj = out.createNestedObject("led");
  led.fillStatus(ledObj);

  JsonObject limitsObj = out.createNestedObject("limits");
  limitsObj["enabled"] = config.limitsConfig.enabled;
  limitsObj["openEnabled"] = config.limitsConfig.open.enabled;
  limitsObj["closeEnabled"] = config.limitsConfig.close.enabled;

  JsonObject ioObj = out.createNestedObject("io");
  bool limitOpenActive = config.limitsConfig.enabled && config.limitsConfig.open.enabled && limitOpenInput.isActive();
  bool limitCloseActive = config.limitsConfig.enabled && config.limitsConfig.close.enabled && limitCloseInput.isActive();
  ioObj["limitOpenRaw"] = limitOpenInput.isActive();
  ioObj["limitCloseRaw"] = limitCloseInput.isActive();
  ioObj["limitOpen"] = limitOpenActive;
  ioObj["limitClose"] = limitCloseActive;
  ioObj["stop"] = stopInput.isActive();
  ioObj["obstacle"] = obstacleInput.isActive();
  ioObj["button"] = buttonInput.isActive();

  JsonObject inputsObj = out.createNestedObject("inputs");
  inputsObj["limitOpen"] = limitOpenActive;
  inputsObj["limitClose"] = limitCloseActive;
  inputsObj["limitOpenRaw"] = limitOpenInput.isActive();
  inputsObj["limitCloseRaw"] = limitCloseInput.isActive();
  inputsObj["photocellBlocked"] = config.sensorsConfig.photocell.enabled && obstacleInput.isActive();
  inputsObj["photocellRaw"] = obstacleInput.isActive();
  inputsObj["photocellEnabled"] = config.sensorsConfig.photocell.enabled;
  inputsObj["hallEnabled"] = config.sensorsConfig.hall.enabled;
  inputsObj["hallPps"] = hallPps;
  inputsObj["hallCount"] = readHallCountAtomic();
  inputsObj["limitsEnabled"] = config.limitsConfig.enabled;
  inputsObj["limitOpenEnabled"] = config.limitsConfig.open.enabled;
  inputsObj["limitCloseEnabled"] = config.limitsConfig.close.enabled;

  JsonObject ldObj = out.createNestedObject("ld2410");
  ldObj["available"] = ld2410Status.available;
  ldObj["present"] = ld2410Status.present;
  ldObj["moving"] = ld2410Status.moving;
  ldObj["stationary"] = ld2410Status.stationary;
  ldObj["distanceCm"] = ld2410Status.available ? ld2410Status.distanceCm : -1;

  JsonObject fsObj = out.createNestedObject("fs");
  fsObj["totalBytes"] = LittleFS.totalBytes();
  fsObj["usedBytes"] = LittleFS.usedBytes();

  JsonObject remObj = out.createNestedObject("remotes");
  remObj["learnMode"] = learnMode;
  remObj["lastSaveOk"] = config.getLastRemotesSaveOk();
  remObj["lastSaveMs"] = config.getLastRemotesSaveMs();
  remObj["lastSaveError"] = config.getLastRemotesSaveError();
  JsonObject lastObj = remObj.createNestedObject("last");
  lastObj["serial"] = lastRemote.serial;
  lastObj["encript"] = lastRemote.encript;
  lastObj["btnToggle"] = lastRemote.btnToggle;
  lastObj["btnGreen"] = lastRemote.btnGreen;
  lastObj["batteryLow"] = lastRemote.batteryLow;
  lastObj["ts"] = lastRemote.ts;
  lastObj["known"] = lastRemote.known;
  lastObj["authorized"] = lastRemote.authorized;
  RemoteEntry r;
  if (lastRemote.serial != 0 && config.getRemote(lastRemote.serial, r)) {
    lastObj["name"] = r.name;
    lastObj["enabled"] = r.enabled;
  }

  JsonArray evArr = out.createNestedArray("events");
  int start = eventHead - eventCount;
  if (start < 0) start += kMaxEvents;
  for (int i = 0; i < eventCount; ++i) {
    int idx = (start + i) % kMaxEvents;
    JsonObject ev = evArr.createNestedObject();
    ev["ts"] = events[idx].ts;
    ev["level"] = events[idx].level;
    ev["message"] = events[idx].message;
  }
}

void fillRemoteState(JsonObject& out) {
  out["serial"] = lastRemote.serial;
  out["encript"] = lastRemote.encript;
  out["btnToggle"] = lastRemote.btnToggle;
  out["btnGreen"] = lastRemote.btnGreen;
  out["batteryLow"] = lastRemote.batteryLow;
  out["ts"] = lastRemote.ts;
  out["known"] = lastRemote.known;
  out["authorized"] = lastRemote.authorized;
  RemoteEntry r;
  if (lastRemote.serial != 0 && config.getRemote(lastRemote.serial, r)) {
    out["name"] = r.name;
    out["enabled"] = r.enabled;
  }
}

void handleControlCmd(const char* action) {
  if (!gate || !action) return;
  if (calibration.isRunning()) {
    pushEvent("warn", "control blocked by calibration");
    return;
  }
  GateCommandResponse r = gate->handleCommand(action);
  if (r.result == GATE_CMD_UNKNOWN) {
    pushEvent("warn", "command unknown");
    return;
  }

  if (r.result == GATE_CMD_BLOCKED) {
    if (r.cmd == GATE_CMD_OPEN) pushEvent("warn", "command open blocked");
    else if (r.cmd == GATE_CMD_CLOSE) pushEvent("warn", "command close blocked");
    else if (r.cmd == GATE_CMD_TOGGLE) pushEvent("warn", "command toggle blocked");
    else pushEvent("warn", "command blocked");
    return;
  }

  // OK
  if (r.cmd == GATE_CMD_OPEN) {
    pushEvent("info", "command open");
  } else if (r.cmd == GATE_CMD_CLOSE) {
    pushEvent("info", "command close");
  } else if (r.cmd == GATE_CMD_STOP) {
    pushEvent("info", "command stop");
    led.setOverride("stopped", 1500);
  } else if (r.cmd == GATE_CMD_TOGGLE) {
    pushEvent("info", r.applied ? "command toggle" : "command toggle (no-op)");
  } else {
    pushEvent("info", "command ok");
  }
}

void handleControlWrapper(const String& action) {
  handleControlCmd(action.c_str());
}

void handleLedCmd(const char* payload) {
  if (!payload) return;
  if (payload[0] == '{') {
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, payload) != DeserializationError::Ok) return;
    if (doc.containsKey("enabled")) {
      led.setEnabled(doc["enabled"] | false);
    }
    if (doc.containsKey("brightness")) {
      led.setBrightness(doc["brightness"] | led.getBrightness());
    }
    if (doc.containsKey("mode")) {
      const char* mode = doc["mode"] | "";
      led.setMode(mode);
    }
    if (doc.containsKey("pattern")) {
      const char* pattern = doc["pattern"] | "flash";
      unsigned long duration = doc["overrideMs"] | doc["duration"] | 800;
      led.setOverride(pattern, duration);
    }
    if (doc["test"] | false) {
      led.startTest();
    }
    mqttPublishLedState();
    return;
  }

  String cmd(payload);
  cmd.toLowerCase();
  if (cmd == "test") {
    led.startTest();
  } else if (cmd == "stealth") {
    led.setMode("stealth");
  } else if (cmd == "off") {
    led.setMode("off");
  } else if (cmd == "status") {
    led.setMode("status");
  } else if (cmd == "flash") {
    led.setOverride("flash", 600);
  } else if (cmd == "idle") {
    led.setMode("idle");
  } else if (cmd == "stopped") {
    led.setMode("stopped");
  }
  mqttPublishLedState();
}

void onHcsReceived(unsigned long serial, unsigned long encript, bool btnToggle, bool btnGreen, bool batt) {
  unsigned long now = millis();
  RemoteEntry entry;
  bool known = config.getRemote(serial, entry);
  bool authorized = known && entry.enabled;

  lastRemote.serial = serial;
  lastRemote.encript = encript;
  lastRemote.btnToggle = btnToggle;
  lastRemote.btnGreen = btnGreen;
  lastRemote.batteryLow = batt;
  lastRemote.ts = now;
  lastRemote.known = known;
  lastRemote.authorized = authorized;

  RemoteSeen& seen = lastRemoteMap[serial];
  const unsigned long kDebounceMs = 700;

  // Automatyczne uczenie pilota jeśli nieznany
  if (!known) {
    String name = String("Remote ") + String(serial);
    if (config.addRemote(serial, name)) {
      pushEventf("info", "auto-learned remote %lu", serial);
      led.setOverride("flash", 500);
      config.getRemote(serial, entry);
      known = true;
      authorized = entry.enabled;
    } else {
      pushEvent("warn", "remote auto-learn failed");
      return;
    }
  }

  if (!authorized) {
    pushEvent("warn", "remote not authorized");
    return;
  }

  // Debounce: tylko btnToggle, ignoruj btnGreen
  if (!btnToggle) return;
  if (btnGreen) return;
  if (now - seen.lastActionMs < kDebounceMs) return;
  if (seen.encript == encript) return;
  seen.lastActionMs = now;
  seen.encript = encript;

  handleControlCmd("toggle");
  pushEvent("info", "remote: toggle");
}

void learnCallback(bool enable) {
  learnMode = enable;
  if (hcs) hcs->setLearnMode(enable);
  led.setLearnMode(enable);
  webserver.setLearnState(enable);
  pushEvent("info", enable ? "learn mode on" : "learn mode off");
  StaticJsonDocument<128> ev;
  ev["type"] = "learn_mode";
  ev["enabled"] = enable;
  char payload[128];
  serializeJson(ev, payload, sizeof(payload));
  webserver.broadcastJson(payload);
}

void testCallback(unsigned long serial, unsigned long encript, bool btnT, bool btnG, bool batt) {
  onHcsReceived(serial, encript, btnT, btnG, batt);
}

void setupOta() {
  if (!config.otaConfig.enabled || otaReady) return;
  ArduinoOTA.setHostname(config.deviceConfig.hostname.c_str());
  ArduinoOTA.setPort(config.otaConfig.port);
  if (config.otaConfig.password.length() > 0) {
    ArduinoOTA.setPassword(config.otaConfig.password.c_str());
  }
  ArduinoOTA.onEnd([]() {
    pushEvent("info", "ota end");
    led.setOtaActive(false);
    if (gate) gate->setOtaActive(false);
  });
  ArduinoOTA.onStart([]() {
    pushEvent("info", "ota start");
    led.setOtaActive(true);
    if (gate) gate->setOtaActive(true);
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    if (total > 0) {
      int pct = (int)((progress * 100U) / total);
      led.setOtaProgress(pct);
    }
  });
  ArduinoOTA.onError([](ota_error_t error) {
    (void)error;
    pushEvent("error", "ota error");
    if (gate) gate->setOtaActive(false);
  });
  ArduinoOTA.begin();
  otaReady = true;
}

void gateTask(void* pvParameters) {
  if (config.safetyConfig.watchdogEnabled) {
    esp_task_wdt_add(NULL);
  }
  while (1) {
    if (gate) gate->loop();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  Serial.begin(115200);
  delay(10);

  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed - attempting to format...");
    if (LittleFS.format()) {
      Serial.println("LittleFS formatted, attempting to mount again...");
      if (!LittleFS.begin()) {
        Serial.println("LittleFS mount failed after format");
      } else {
        Serial.println("LittleFS mounted OK after format");
      }
    } else {
      Serial.println("LittleFS format failed");
    }
  } else {
    Serial.println("LittleFS mounted OK");
  }

  if (LittleFS.exists("/index.html")) {
    Serial.println("index.html found in LittleFS");
  } else {
    Serial.println("index.html NOT found in LittleFS - run uploadfs to upload UI files");
  }

  config.begin();
  config.load();
  maxDistanceMeters = config.gateConfig.maxDistance > 0.0f ?
    config.gateConfig.maxDistance : config.gateConfig.totalDistance;
  if (maxDistanceMeters < 0.0f) maxDistanceMeters = 0.0f;
  positionMeters = config.gateConfig.position;
  if (positionMeters < 0.0f) positionMeters = 0.0f;
  if (maxDistanceMeters > 0.0f && positionMeters > maxDistanceMeters) {
    positionMeters = maxDistanceMeters;
  }
  config.gateConfig.position = positionMeters;
  config.gateConfig.maxDistance = maxDistanceMeters;
  config.gateConfig.totalDistance = maxDistanceMeters;
  lastPersistedPosition = positionMeters;
  lastPositionPersistMs = millis();
  led.init(config.ledConfig);
  led.setMqttEnabled(config.mqttConfig.enabled);

  motor = new MotorController();
  motor->applyConfig(config.motorConfig, config.gpioConfig, config.hoverUartConfig);
  motor->setInvertDir(config.motorConfig.invertDir || config.gpioConfig.dirInvert);
  motor->setMotionProfile(config.motionProfile());
  motor->begin();

  gate = new GateController(motor, &config);
  gate->begin();
  gate->setStatusCallback(onGateStatusChanged, nullptr);
  xTaskCreatePinnedToCore(gateTask, "GateTask", 4096, NULL, 1, &gateTaskHandle, 1);

  setupInputs();
  updateHallAttachment();
  WiFiManager.begin(&config);

  hcs = new HCS301Receiver(config.gpioConfig.hcsPin);
  hcs->begin();
  hcs->setCallback(onHcsReceived);

  mqtt.setCallback(mqttCallback);
  mqtt.begin(config.mqttConfig);

  webserver.setStatusCallback(fillStatus);
  webserver.setDiagnosticsCallback(fillDiagnostics);
  webserver.setRemoteStateCallback(fillRemoteState);
  webserver.setControlCallback(handleControlWrapper);
  webserver.setGateCalibrateCallback(handleGateCalibrate);
  webserver.setLearnCallback(learnCallback);
  webserver.setTestCallback(testCallback);
  webserver.setLearnState(learnMode);
  led.setLearnMode(learnMode);
  webserver.setMqttManager(&mqtt);
  webserver.setLedController(&led);
  webserver.setMotorController(motor);
  calibration.begin(&config, motor, gate);
  webserver.setCalibrationManager(&calibration);
  webserver.begin();

  pushEvent("info", "setup done");
}

void loop() {
  WiFiManager.loop();
  if (hcs) hcs->loop();

  calibration.loop();
  if (!calibration.isRunning()) {
    handleInputs();
  }
  updateHallAttachment();
  updatePositionPercent();
  unsigned long now = millis();
  updateHallStats(now);
  updateLd2410Status(now);
  maybePersistPosition(now);

  mqtt.loop();
  bool nowConnected = mqtt.connected();
  bool wifiConnected = WiFiManager.isConnected();
  bool apMode = WiFiManager.getModeString() == "AP";
  if (gate) gate->setConnectivity(wifiConnected, nowConnected, apMode);
  led.setMqttEnabled(mqtt.enabled());
  led.tick(now);
  if (nowConnected && !mqttWasConnected) {
    const char* cmdTopic = mqtt.topicCommand();
    if (cmdTopic && cmdTopic[0] != '\0') {
      mqtt.subscribe(cmdTopic);
    }
    const char* gateCmdTopic = mqtt.topicGateCmd();
    if (gateCmdTopic && gateCmdTopic[0] != '\0') {
      mqtt.subscribe(gateCmdTopic);
    }
    const char* setMaxTopic = mqtt.topicGateSetMax();
    if (setMaxTopic && setMaxTopic[0] != '\0') {
      mqtt.subscribe(setMaxTopic);
    }
    const char* calTopic = mqtt.topicGateCalibrate();
    if (calTopic && calTopic[0] != '\0') {
      mqtt.subscribe(calTopic);
    }
    const char* ledCmdTopic = mqtt.topicLedCmd();
    if (ledCmdTopic && ledCmdTopic[0] != '\0') {
      mqtt.subscribe(ledCmdTopic);
    }
    pushEvent("info", "mqtt connected");
    mqttPublishStatus();
    mqttPublishLedState();
    mqttPublishTelemetry();
    mqttPublishPosition();
  }
  mqttWasConnected = nowConnected;
  if (nowConnected) {
    if (now - lastMqttPublish > 5000) {
      lastMqttPublish = now;
      mqttPublishStatus();
      mqttPublishLedState();
    }
    if (now - lastMqttTelemetryMs > 1000) {
      lastMqttTelemetryMs = now;
      mqttPublishTelemetry();
      mqttPublishPosition();
      mqttPublishMotionState();
      mqttPublishMotionPosition();
    }
  }

  if (config.otaConfig.enabled && WiFiManager.isConnected()) {
    setupOta();
    if (otaReady) ArduinoOTA.handle();
  }

  if (calibration.isRunning() || calibration.hasError() || calibration.isComplete()) {
    if (now - lastCalibWsMs > 250) {
      lastCalibWsMs = now;
      StaticJsonDocument<768> doc;
      doc["type"] = "calibration";
      JsonObject data = doc.createNestedObject("data");
      calibration.fillStatus(data);
      char payload[768];
      serializeJson(doc, payload, sizeof(payload));
      webserver.broadcastJson(payload);
    }
  }
  if (now - lastStatusMs > 1000) {
    lastStatusMs = now;
    webserver.broadcastStatus();
  }
  vTaskDelay(pdMS_TO_TICKS(1));
}