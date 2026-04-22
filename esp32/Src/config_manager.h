#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ConfigManager - persistent JSON config stored in LittleFS (/config.json)
static constexpr const char* CONFIG_PATH = "/config.json";
static constexpr const char* CONFIG_TMP_PATH = "/config.tmp";
static constexpr const char* CONFIG_BAK_PATH = "/config.bak";
static constexpr int CONFIG_VERSION = 1;
static constexpr size_t CONFIG_JSON_CAPACITY = 16384;

struct GateConfig {
  float totalDistance = 12.0f; // legacy alias for maxDistance
  float maxDistance = 12.0f;
  float position = 0.0f;
  int32_t hbOriginDistMm = 0;
  float wheelCircumference = 0.132f;
  int pulsesPerRevolution = 12;
  unsigned long movementTimeout = 30000;
  unsigned long startupHomingTimeoutMs = 90000;
  unsigned long stallTimeoutMs = 1500;
  unsigned long telemetryTimeoutMs = 1200;
  unsigned long telemetryGraceMs = 1500;
  int homingScalePercent = 30;
  bool softLimitsEnabled = true;
  float currentLimitA = 0.0f;
  // Over-current protection (hover telemetry current iA)
  unsigned long overCurrentTripMs = 120; // ms above limit to trigger (debounce)
  unsigned long overCurrentCooldownMs = 4000; // ms to block motion after trip
  int overCurrentMaxAutoRearm = 3; // attempts before requiring manual intervention
  bool overCurrentAutoRearm = true;
  String overCurrentAction = "stop"; // stop | reverse
  unsigned long autoCloseDelay = 0; // 0 - disabled
  bool toggleDirection = true;
  String positionSource = "encoder"; // encoder | hoverboard_tel
  bool allowMoveWithoutLimits = false;
  // Compatibility aliases for older code expecting these fields
  float slowDownDistance = 0.5f; // meters
  int softStartTime = 3500; // ms
  int pwmMin = 75;
  int pwmMax = 100;
  int gateSpeed = 100;
  int slowSpeed = 75;
};

struct LimitsConfig {
  bool enabled = false;
  struct LimitInputConfig {
    bool enabled = true;
    int pin = -1;
    bool invert = false;
    String pullMode = "up"; // none | up | down
    unsigned long debounceMs = 30;
  } open, close;
};

struct WiFiApConfig {
  String ssid = "GateOS-Setup";
  String password = "";
  unsigned long fallbackTimeoutMs = 30000;
};

struct StaticIpConfig {
  bool enabled = false;
  String ip = "";
  String gateway = "";
  String netmask = "";
  String dns1 = "";
  String dns2 = "";
};

struct WiFiConfig {
  String ssid = "ChemiXv3";
  String password = "chemik123";
  WiFiApConfig apFallback;
  StaticIpConfig staticIp;
};

struct MQTTConfig {
  bool enabled = false;
  String server = "";
  int port = 1883;
  String user = "";
  String password = "";
  String topicBase = "brama";
  bool retain = true;
  int qos = 0;
  String lwtTopic = "";
  String lwtPayload = "";
};

struct OTAConfig {
  bool enabled = false;
  int port = 3232;
  String password = "";
};

struct MotorConfig {
  String driverType = "pwm_dir"; // hbridge | pwm_dir | relay
  int pwmMin = 20;
  int pwmMax = 255;
  int pwmFreq = 5000;
  int pwmResolution = 8;
  int softStartMs = 1000;
  int softStopMs = 600;
  String rampCurve = "linear";
  bool invertDir = false;
  int maxSpeedOpen = 180;
  int maxSpeedClose = 180;
  int minSpeed = 40;
  struct RampConfig {
    String mode = "distance"; // time | distance
    float value = 1.2f;       // ms or meters (when distance)
  } rampOpen, rampClose;
  struct BrakingConfig {
    float startDistanceOpen = 0.4f;
    float startDistanceClose = 0.4f;
    int force = 60;          // 0-100
    String mode = "coast";   // coast | active | hold
  } braking;
};

struct HoverUartConfig {
  int rxPin = 16;
  int txPin = 17;
  int baud = 115200;
  int maxSpeed = 300;
  int rampStep = 2;
  // Keep the hoverboard controller armed even when target speed is 0 (prevents unwanted power state changes)
  bool keepArmed = true;
  // Cooldown after a fault before attempting to re-arm (ms)
  uint32_t faultCooldownMs = 4000;
  // Maximum auto re-arm attempts after consecutive faults (0 = disabled)
  uint8_t maxAutoRearm = 3;
};

struct GpioConfig {
  int pwmPin = 33;
  int dirPin = 25;
  int enPin = -1;
  int limitOpenPin = 27;
  int limitClosePin = 26;
  int buttonPin = 12;
  int stopPin = -1;
  int obstaclePin = -1;
  int hcsPin = 13;
  int ledPin = 2;
  bool limitOpenInvert = false;
  bool limitCloseInvert = false;
  bool buttonInvert = false;
  bool obstacleInvert = false;
  bool dirInvert = false;
};

struct HallConfig {
  bool enabled = false;
  int pin = 14;
  bool invert = false;
  String pullMode = "up"; // none | up | down
  int debounceMs = 10;
};

struct PhotocellConfig {
  bool enabled = false;
  int pin = -1;
  bool invert = true;
  String pullMode = "up"; // none | up | down
  int debounceMs = 10;
};

struct SensorsConfig {
  HallConfig hall;
  PhotocellConfig photocell;
};

struct SafetyConfig {
  String obstacleAction = "open"; // legacy/UI compatibility; photocell forces stop+open while closing
  int obstacleReverseCm = 50;
  bool watchdogEnabled = true;
};

struct RemoteConfig {
  int antiRepeatMs = 250;
  bool antiReplay = true;
  int replayWindow = 5;
};

struct LedConfig {
  bool enabled = true;
  String type = "ws2812";
  int pin = 2;
  int count = 7;
  int brightness = 80;
  int ringStartIndex = 0;
  bool ringReverse = false;
  String colorOrder = "GRB";
  String defaultMode = "status";
  String mode = "status";
  int animSpeed = 50;
  struct NightMode {
    bool enabled = false;
    int brightness = 15;
    String from = "22:00";
    String to = "06:00";
  } nightMode;
  struct Segment {
    int start = 0;
    int length = 0;
  };
  static constexpr int kMaxSegments = 8;
  int segmentCount = 0;
  Segment segments[kMaxSegments] = {};
};

struct MotionUiConfig {
  String speedOpen = "normal";        // slow | normal | fast
  String speedClose = "normal";
  String accelSmoothness = "normal";  // soft | normal | dynamic
  String decelSmoothness = "normal";  // soft | normal | dynamic
  String slowdownDistance = "normal"; // close | normal | early
  String brakingFeel = "normal";      // soft | normal | strong
};

struct MotionAdvancedConfig {
  int maxSpeedOpen = 180;
  int maxSpeedClose = 180;
  int minSpeed = 40;
  MotorConfig::RampConfig rampOpen;
  MotorConfig::RampConfig rampClose;
  MotorConfig::BrakingConfig braking;
};

struct MotionConfig {
  String profile = "user"; // user | soft | dynamic | industrial
  bool expert = false;
  MotionUiConfig ui;
  MotionAdvancedConfig advanced;
};

struct SecurityConfig {
  bool enabled = false;
  String apiToken = "";
};

struct DeviceConfig {
  String name = "GateOS";
  String hostname = "gateos";
  int webPort = 80;
};

struct RemoteEntry {
  unsigned long serial = 0;
  String name = "";
  bool enabled = true;
  unsigned long lastCounter = 0;
  unsigned long lastSeenMs = 0;
};

class ConfigManager {
public:
  ConfigManager();
  void begin();

  void load();
  bool save(String* error = nullptr);
  using SaveAllowedCb = bool (*)();
  void setSaveAllowedCallback(SaveAllowedCb cb) { saveAllowedCb = cb; }
  bool processDeferredSave();
  bool hasPendingSave() const { return pendingSave; }
  bool ensureDefaultConfigExists(String* error = nullptr);
  void resetToDefaults();

  GateConfig gateConfig;
  LimitsConfig limitsConfig;
  WiFiConfig wifiConfig;
  MQTTConfig mqttConfig;
  OTAConfig otaConfig;
  MotorConfig motorConfig;
  HoverUartConfig hoverUartConfig;
  GpioConfig gpioConfig;
  SensorsConfig sensorsConfig;
  SafetyConfig safetyConfig;
  RemoteConfig remoteConfig;
  LedConfig ledConfig;
  MotionConfig motionConfig;
  SecurityConfig securityConfig;
  DeviceConfig deviceConfig;

  // Serialize/deserialize JSON for web UI
  String toJson();
  bool fromJson(const String& json);
  bool fromJsonVariant(JsonVariantConst root);
  bool validate(JsonVariantConst root, String& error);
  unsigned long getLastSaveMs() const { return lastSaveMs; }
  const String& getLastSaveError() const { return lastSaveError; }
  bool getLastRemotesSaveOk() const { return lastRemotesSaveOk; }
  unsigned long getLastRemotesSaveMs() const { return lastRemotesSaveMs; }
  const String& getLastRemotesSaveError() const { return lastRemotesSaveError; }
  uint32_t getRevision() const { return revision; }
  MotionAdvancedConfig motionProfile() const;
  void adoptPersistenceMetaFrom(const ConfigManager& other) {
    revision = other.revision;
    lastSaveMs = other.lastSaveMs;
    lastSaveError = other.lastSaveError;
    lastRemotesSaveOk = other.lastRemotesSaveOk;
    lastRemotesSaveMs = other.lastRemotesSaveMs;
    lastRemotesSaveError = other.lastRemotesSaveError;
  }

  // Remote management
  bool addRemote(unsigned long serial, const String& name = "");
  bool updateRemote(unsigned long serial, const String& name, bool enabled);
  bool removeRemote(unsigned long serial);
  void clearRemotes();
  void touchRemote(unsigned long serial, unsigned long counter, unsigned long seenMs);
  bool isAuthorized(unsigned long serial) const;
  bool getRemote(unsigned long serial, RemoteEntry& out) const;
  const std::vector<RemoteEntry>& getRemotes() const { return remotes; }

private:
  std::vector<RemoteEntry> remotes;
  uint32_t revision = 0;
  unsigned long lastSaveMs = 0;
  String lastSaveError;
  bool lastRemotesSaveOk = true;
  unsigned long lastRemotesSaveMs = 0;
  String lastRemotesSaveError;
  SaveAllowedCb saveAllowedCb = nullptr;
  bool pendingSave = false;
  unsigned long pendingSaveAtMs = 0;
  unsigned long lastDeferredAttemptMs = 0;
  bool readConfigFileToDoc(DynamicJsonDocument& doc, String& error);
  void buildJson(JsonDocument& doc) const;
  bool saveInternal(String* error, bool force);
  // FIX #3: mutex serialises concurrent saveInternal() calls from
  // configSaveTask and processPendingRuntimeConfigApply().
  SemaphoreHandle_t _saveMutex = nullptr;
};
