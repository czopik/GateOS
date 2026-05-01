#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <map>
#include <math.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "config_manager.h"
#include "wifi_manager.h"
#include "motor_controller.h"

// Wersja firmware — aktualizowana automatycznie przy każdej kompilacji.
// Format: "YYYY-MM-DD HH:MM:SS" wynikający z makr __DATE__ i __TIME__.
static const char FW_VERSION[] = __DATE__ " " __TIME__;
#include "gate_controller.h"
#include "hcs301_receiver.h"
#include "web_server.h"
#include "mqtt_manager.h"
#include "led_controller.h"
#include "input_manager.h"
#include "position_tracker.h"

ConfigManager config;
MotorController* motor = nullptr;
GateController* gate = nullptr;
HCS301Receiver* hcs = nullptr;
WebServerManager webserver(&config);

MqttManager mqtt;
LedController led;
InputManager inputManager;
PositionTracker positionTracker;

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

static std::map<unsigned long, RemoteSeen> lastRemoteMap;
static LastRemote lastRemote;
static volatile bool learnMode = false;
static volatile uint32_t learnModeUntilMs = 0;
static constexpr uint32_t kLearnModeWindowMs = 30000;
static const int kMaxEvents = 80;
static EventEntry events[kMaxEvents];
static int eventHead = 0;
static int eventCount = 0;
static unsigned long lastStatusMs = 0;
static uint32_t fsTotalBytesCached = 0;
static uint32_t fsUsedBytesCached = 0;
static unsigned long fsLastStatsMs = 0;
static QueueHandle_t eventQueue = nullptr;

// === FIX #2: Gate command queue ===
// All gate control commands (web, MQTT, remote, button) are enqueued here and
// drained exclusively in GateTask, eliminating concurrent gate-state access.
struct GateCmdItem {
  char action[32];
};
static QueueHandle_t gateCommandQueue = nullptr;
static constexpr uint32_t kGateCommandMinIntervalMs = 180;
static constexpr uint32_t kGateCommandDuplicateWindowMs = 120;
static constexpr uint8_t kGateCommandDrainPerCycle = 1;

struct GateCommandIngressStats {
  uint32_t accepted = 0;
  uint32_t rateLimited = 0;
  uint32_t duplicates = 0;
  uint32_t queueFull = 0;
  uint32_t stopNotifications = 0;
  uint32_t lastAcceptedMs = 0;
  char lastAction[32] = {0};
};
static GateCommandIngressStats gateCommandIngress;
// FIX C1: portMUX_TYPE spinlock guards gateCommandIngress from concurrent
// writes by AsyncWebServer task (Core 0) and main loop (Core 1).
static portMUX_TYPE gateCommandIngressMux = portMUX_INITIALIZER_UNLOCKED;

// === FIX #3: Config save background task ===
static SemaphoreHandle_t configSaveSem      = nullptr;
static TaskHandle_t      configSaveTaskHandle = nullptr;

// === v2.1: Inter-task communication via xTaskNotify bits ===
// All urgent gate commands use task notifications (eSetBits) into GateTask.
// Bits are OR-accumulated between GateTask cycles and processed atomically at
// the top of each cycle with xTaskNotifyWait(0, UINT32_MAX, &val, 0).
//
//   GATE_NOTIFY_STOP        — "stop" from web/MQTT handler (no ack required)
//   GATE_NOTIFY_EMERGENCY   — emergency stop from OTA callback (ack via sem)
//   GATE_NOTIFY_LIMIT_OPEN  — rising edge of open limit switch (from handleInputs)
//   GATE_NOTIFY_LIMIT_CLOSE — rising edge of close limit switch (from handleInputs)
//
// emergencyStopAckSem: binary semaphore given by GateTask after it processes
//   GATE_NOTIFY_EMERGENCY so OTA callback can wait with a hard timeout.
//
// inputsInitialized: set by main loop after the first handleInputs() iteration;
//   GateTask homing waits for this before reading limit switch states.
static constexpr uint32_t GATE_NOTIFY_STOP           = (1u << 0);
static constexpr uint32_t GATE_NOTIFY_EMERGENCY      = (1u << 1);
static constexpr uint32_t GATE_NOTIFY_LIMIT_OPEN     = (1u << 2);
static constexpr uint32_t GATE_NOTIFY_LIMIT_CLOSE    = (1u << 3);
static constexpr uint32_t GATE_NOTIFY_LIMITS_INVALID = (1u << 4);  // both limits active simultaneously
static constexpr uint32_t GATE_NOTIFY_OBSTACLE       = (1u << 5);  // obstacle sensor state changed
static SemaphoreHandle_t  emergencyStopAckSem         = nullptr;
// Carries the obstacle active/clear state for GATE_NOTIFY_OBSTACLE; written before xTaskNotify.
static volatile bool      obstacleNotifyActive        = false;
static volatile bool      inputsInitialized           = false;

static DebouncedInput limitOpenInput;
static DebouncedInput limitCloseInput;
static DebouncedInput stopInput;
static DebouncedInput obstacleInput;
static DebouncedInput buttonInput;

// Request one-shot resync of local position/hall counters when a limit is hit.
static bool resyncAtOpenLimit = false;
static bool resyncAtCloseLimit = false;

static bool otaReady = false;
static bool otaActive = false;
static int otaProgress = -1;
static char otaError[48] = {0};
static bool otaNetDiagLogged = false;
static unsigned long lastMqttPublish = 0;
static unsigned long lastMqttTelemetryMs = 0;
static unsigned long lastMqttHoverTelMs = 0;  // v2.2: hover telemetry at 5s (separate from motion 1s)
static bool mqttWasConnected = false;
static volatile bool restartPending = false;
static volatile uint32_t restartAtMs = 0;
static char pendingRestartReason[32] = "";
static volatile bool factoryResetPending = false;
static volatile uint32_t factoryResetAtMs = 0;
static volatile bool runtimeConfigApplyPending = false;
static volatile uint32_t runtimeConfigApplyRequestedMs = 0;
// FIX A1: cooperative pause flag for processPendingRuntimeConfigApply().
// Main loop sets configApplyPausing=true; GateTask sees it at the top of its
// while(1) loop and parks in a WDT-safe spin until the flag is cleared.
// configApplyPaused is set by GateTask to acknowledge it has entered the park.
static volatile bool configApplyPausing = false;
static volatile bool configApplyPaused  = false;
static volatile uint32_t mainLoopHeartbeatMs = 0;
static volatile uint32_t gateTaskHeartbeatMs = 0;
RTC_DATA_ATTR static uint32_t rtcBootCount = 0;
RTC_DATA_ATTR static uint32_t rtcLastResetReason = 0;
RTC_DATA_ATTR static char rtcLastRestartReason[32] = "";
static uint32_t prevRtcResetReason = 0;
static uint32_t bootCount = 0;
static esp_reset_reason_t bootResetReason = ESP_RST_UNKNOWN;
static char bootRestartReason[32] = "";

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
static float positionMetersRaw = 0.0f;
static float hoverOffsetMeters = 0.0f;
static bool hoverOffsetValid = false;
static float maxDistanceMeters = 0.0f;
static GateState lastGateState = GATE_STOPPED;
static unsigned long moveStartMs = 0;
static float moveStartPosition = 0.0f;
static int hallPinActive = -1;
static bool hallAttached = false;
static unsigned long lastPositionPersistMs = 0;
static float lastPersistedPosition = -1.0f;
// Startup homing diagnostics/state
static bool startupLimitRefDone = false;
static bool homingChecked = false;
static bool homingActive = false;
static uint32_t homingStartMs = 0;
static uint32_t homingReadySinceMs = 0;
static bool startupHomingEnabled = true;
static bool startupPositionCertain = false;
static bool startupSafetyLocked = false;
static constexpr float kStartupTempDistanceMeters = 0.100f;
static bool startupUiUnknownPos10 = false;
static uint32_t startupSyntheticUiLogMs = 0;
static uint32_t startupBootMs = 0;
static MotionAdvancedConfig normalMotionProfile;
static bool homingProfileApplied = false;
static bool homingSoftLimitsOverridden = false;
static bool homingSoftLimitsPrev = true;
static int homingForceCmd = 80;
static bool homingSearchOpen = false;

// === v2.1: Homing status as enums (written in GateTask, read in main loop) ===
// Stored as volatile uint8_t: single-byte write/read is atomic on Xtensa.
// String mapping is deferred to the API/status layer only.
enum HomingResult : uint8_t {
  HOMING_RESULT_IDLE    = 0,
  HOMING_RESULT_PENDING,
  HOMING_RESULT_RUNNING,
  HOMING_RESULT_SUCCESS,
  HOMING_RESULT_SKIP,
  HOMING_RESULT_BLOCKED,
  HOMING_RESULT_ERROR,
  HOMING_RESULT_ABORT,
};
enum HomingReason : uint8_t {
  HOMING_REASON_NONE                    = 0,
  HOMING_REASON_COLD_BOOT_PENDING,
  HOMING_REASON_NON_COLD_BOOT,
  HOMING_REASON_CLOSE_LIMIT_ACTIVE,
  HOMING_REASON_OPEN_LIMIT_ACTIVE,
  HOMING_REASON_BOTH_LIMITS_ACTIVE,
  HOMING_REASON_MANUAL_REF_REQUIRED,
  HOMING_REASON_LIMITS_INACTIVE_UNKNOWN,
  HOMING_REASON_OBSTACLE_ACTIVE,
  HOMING_REASON_WAIT_COMM_RECOVERY,
  HOMING_REASON_CRITICAL_ERROR,
  HOMING_REASON_WAIT_TEL_STARTUP,
  HOMING_REASON_STARTUP_TEL_FAULT,
  HOMING_REASON_STARTUP_TEL_MISSING,
  HOMING_REASON_START_FAILED,
  HOMING_REASON_SEARCH_OPEN,
  HOMING_REASON_SEARCH_CLOSE,
  HOMING_REASON_OPEN_LIMIT_FOUND,
  HOMING_REASON_CLOSE_LIMIT_FOUND,
  HOMING_REASON_OBSTACLE_DURING_HOMING,
  HOMING_REASON_TEL_LOST_OR_FAULT,
  HOMING_REASON_GATE_ERROR_DURING_HOMING,
  HOMING_REASON_DISTANCE_LIMIT,
  HOMING_REASON_TIMEOUT,
};
static volatile uint8_t homingResultEnum = (uint8_t)HOMING_RESULT_IDLE;
static volatile uint8_t homingReasonEnum = (uint8_t)HOMING_REASON_NONE;
static uint32_t homingLastChangeMs = 0;
static char startupSafetyReason[32] = "boot_pending";
static bool chargerConnected = false;
static bool chargerStateKnown = false;
static bool chargerPending = false;
static bool chargerPendingState = false;
static uint32_t chargerPendingSinceMs = 0;
static constexpr float kChargerConnectV = 40.2f;
static constexpr float kChargerDisconnectV = 39.2f;
static constexpr uint32_t kChargerDebounceMs = 1200;

static void updateChargerConnectedFromTelemetry(const HoverTelemetry& tel, uint32_t nowMs) {
  bool desiredKnown = false;
  bool desired = chargerConnected;

  // Preferred source: explicit charger state from STM32 telemetry.
  if (tel.charger == 0 || tel.charger == 1) {
    desiredKnown = true;
    desired = (tel.charger == 1);
  } else if (tel.batValid && tel.batV > 0.0f && isfinite(tel.batV)) {
    // Fallback source: battery voltage hysteresis.
    desiredKnown = true;
    if (!chargerStateKnown) {
      desired = tel.batV >= ((kChargerConnectV + kChargerDisconnectV) * 0.5f);
    } else {
      desired = chargerConnected;
      if (!chargerConnected && tel.batV >= kChargerConnectV) desired = true;
      if (chargerConnected && tel.batV <= kChargerDisconnectV) desired = false;
    }
  }

  if (!desiredKnown) {
    chargerPending = false;
    return;
  }

  if (!chargerStateKnown) {
    chargerConnected = desired;
    chargerStateKnown = true;
    chargerPending = false;
    return;
  }

  if (desired == chargerConnected) {
    chargerPending = false;
    return;
  }

  if (!chargerPending || chargerPendingState != desired) {
    chargerPending = true;
    chargerPendingState = desired;
    chargerPendingSinceMs = nowMs;
    return;
  }

  if ((nowMs - chargerPendingSinceMs) >= kChargerDebounceMs) {
    chargerConnected = desired;
    chargerPending = false;
  }
}


static long readHallCountAtomic() {
  long v;
  portENTER_CRITICAL(&hallMux);
  v = hallCount;
  portEXIT_CRITICAL(&hallMux);
  return v;
}

void updateFsStats(uint32_t nowMs) {
  if (fsLastStatsMs != 0) return;
  fsTotalBytesCached = LittleFS.totalBytes();
  fsUsedBytesCached = LittleFS.usedBytes();
  fsLastStatsMs = nowMs;
}

bool isSafeToSaveConfig() {
  return !gate || !gate->isMoving();
}

void scheduleRestart(uint32_t delayMs, const char* reason) {
  restartPending = true;
  restartAtMs = millis() + delayMs;
  strncpy(pendingRestartReason, reason ? reason : "unspecified", sizeof(pendingRestartReason) - 1);
  pendingRestartReason[sizeof(pendingRestartReason) - 1] = '\0';
  strncpy(rtcLastRestartReason, pendingRestartReason, sizeof(rtcLastRestartReason) - 1);
  rtcLastRestartReason[sizeof(rtcLastRestartReason) - 1] = '\0';
  Serial.printf("[RESTART] scheduled reason=%s delayMs=%lu\n",
                pendingRestartReason,
                (unsigned long)delayMs);
}

void scheduleFactoryReset(uint32_t delayMs) {
  factoryResetPending = true;
  factoryResetAtMs = millis() + delayMs;
}

void scheduleRuntimeConfigApply() {
  runtimeConfigApplyPending = true;
  runtimeConfigApplyRequestedMs = millis();
}

TaskHandle_t gateTaskHandle = NULL;

ControlResult handleControlCmd(const char* action);
static bool sendGateCommand(const char* action); // FIX #2: forward declaration
void mqttPublishEvent(const char* level, const char* message);
void mqttPublishLedState();
void mqttPublishTelemetry();
void mqttPublishPosition();
void handleLedCmd(const char* payload);
bool applyMaxDistance(float value, bool persist);
bool handleGateCalibrate(const char* mode);
bool isSafeToSaveConfig();
void updateHallAttachment();
void updatePositionPercent();
void updateFsStats(uint32_t nowMs);
void scheduleRestart(uint32_t delayMs, const char* reason);
void scheduleFactoryReset(uint32_t delayMs);
void scheduleRuntimeConfigApply();
void onGateStatusChanged(const GateStatus& status, void* ctx);
void fillDiagnostics(JsonObject& out);
void syncLegacyPositionState();
const char* resetReasonToString(esp_reset_reason_t reason);

static const char* effectiveGateStateString() {
  if (homingActive) return "homing";
  return gate ? gate->getStateString() : "unknown";
}

static bool effectiveGateMoving() {
  return homingActive || (gate && gate->isMoving());
}

static ControlResult makeControlResult(bool ok,
                                       bool applied,
                                       int httpCode,
                                       const char* status,
                                       const char* error = "") {
  ControlResult result;
  result.ok = ok;
  result.applied = applied;
  result.httpCode = httpCode;
  result.status = status;
  result.error = error;
  return result;
}

static const char* homingResultStr(HomingResult r) {
  switch (r) {
    case HOMING_RESULT_IDLE:    return "idle";
    case HOMING_RESULT_PENDING: return "pending";
    case HOMING_RESULT_RUNNING: return "running";
    case HOMING_RESULT_SUCCESS: return "success";
    case HOMING_RESULT_SKIP:    return "skip";
    case HOMING_RESULT_BLOCKED: return "blocked";
    case HOMING_RESULT_ERROR:   return "error";
    case HOMING_RESULT_ABORT:   return "abort";
    default:                    return "unknown";
  }
}
static const char* homingReasonStr(HomingReason r) {
  switch (r) {
    case HOMING_REASON_NONE:                     return "none";
    case HOMING_REASON_COLD_BOOT_PENDING:        return "cold_boot_pending";
    case HOMING_REASON_NON_COLD_BOOT:            return "non_cold_boot";
    case HOMING_REASON_CLOSE_LIMIT_ACTIVE:       return "close_limit_active";
    case HOMING_REASON_OPEN_LIMIT_ACTIVE:        return "open_limit_active";
    case HOMING_REASON_BOTH_LIMITS_ACTIVE:       return "both_limits_active";
    case HOMING_REASON_MANUAL_REF_REQUIRED:      return "manual_reference_required";
    case HOMING_REASON_LIMITS_INACTIVE_UNKNOWN:  return "limits_inactive_unknown_position";
    case HOMING_REASON_OBSTACLE_ACTIVE:          return "obstacle_active";
    case HOMING_REASON_WAIT_COMM_RECOVERY:       return "wait_comm_recovery";
    case HOMING_REASON_CRITICAL_ERROR:           return "critical_error";
    case HOMING_REASON_WAIT_TEL_STARTUP:         return "wait_telemetry_startup";
    case HOMING_REASON_STARTUP_TEL_FAULT:        return "startup_tel_fault";
    case HOMING_REASON_STARTUP_TEL_MISSING:      return "startup_tel_missing_retry";
    case HOMING_REASON_START_FAILED:             return "start_failed";
    case HOMING_REASON_SEARCH_OPEN:              return "search_open_limit";
    case HOMING_REASON_SEARCH_CLOSE:             return "search_close_limit";
    case HOMING_REASON_OPEN_LIMIT_FOUND:         return "open_limit_found";
    case HOMING_REASON_CLOSE_LIMIT_FOUND:        return "close_limit_found";
    case HOMING_REASON_OBSTACLE_DURING_HOMING:   return "obstacle_during_homing";
    case HOMING_REASON_TEL_LOST_OR_FAULT:        return "telemetry_lost_or_fault";
    case HOMING_REASON_GATE_ERROR_DURING_HOMING: return "gate_error_during_homing";
    case HOMING_REASON_DISTANCE_LIMIT:           return "distance_limit";
    case HOMING_REASON_TIMEOUT:                  return "timeout";
    default:                                     return "unknown";
  }
}

static void setHomingResult(HomingResult result, HomingReason reason) {
  homingResultEnum = (uint8_t)result;
  homingReasonEnum = (uint8_t)reason;
  homingLastChangeMs = millis();
}

static void setStartupSafetyState(bool positionCertain, bool safetyLocked, const char* reason) {
  const bool hadSyntheticUi = startupUiUnknownPos10;
  startupPositionCertain = positionCertain;
  startupSafetyLocked = safetyLocked;
  if (positionCertain) {
    startupUiUnknownPos10 = false;
    if (hadSyntheticUi) {
      Serial.printf("[HOMING_TMP_POS] clear reason=%s\n", reason ? reason : "position_certain");
    }
  }
  strncpy(startupSafetyReason, reason ? reason : "", sizeof(startupSafetyReason) - 1);
  startupSafetyReason[sizeof(startupSafetyReason) - 1] = '\0';
}

static void logStartupHomingDiag(const char* stage, uint32_t nowMs, const char* note = "") {
  // v2: Read limit states from gate volatile vars (safe from GateTask context)
  const bool openActive  = gate ? gate->getLimitOpenActive()  : inputManager.limitOpenActive(config);
  const bool closeActive = gate ? gate->getLimitCloseActive() : inputManager.limitCloseActive(config);
  const bool obstacle = config.sensorsConfig.photocell.enabled && inputManager.obstacleActive();
  const bool gateExists = (gate != nullptr);
  const bool motorExists = (motor != nullptr);
  int gateState = gateExists ? (int)gate->getState() : -1;
  int stopReason = gateExists ? (int)gate->getLastStopReason() : -1;
  int errCode = gateExists ? (int)gate->getErrorCode() : -1;
  bool moving = gateExists ? gate->isMoving() : false;
  float targetPos = gateExists ? gate->getTargetPosition() : -1.0f;
  float ctrlPos = gateExists ? gate->getControlPosition() : -1.0f;
  int fault = -1;
  int rpm = 0;
  int armed = -1;
  long telAge = -1;
  bool telTimedOut = true;
  bool telEnabled = false;
  if (motorExists && motor->isHoverUart() && motor->hoverEnabled()) {
    telEnabled = true;
    const HoverTelemetry& tel = motor->hoverTelemetry();
    fault = tel.fault;
    rpm = tel.rpm;
    armed = tel.armed ? 1 : 0;
    telAge = (tel.lastTelMs == 0) ? -1 : (long)(nowMs - (uint32_t)tel.lastTelMs);
    uint32_t tmo = config.gateConfig.telemetryTimeoutMs > 0 ? config.gateConfig.telemetryTimeoutMs : 1200;
    telTimedOut = motor->hoverTelemetryTimedOut(nowMs, tmo);
  }
  Serial.printf("[HOMING_DIAG] stage=%s note=%s enabled=%d certain=%d lock=%d reason=%s active=%d reset=%s open=%d close=%d obstacle=%d fault=%d armed=%d telEnabled=%d telAge=%ld telTimeout=%d state=%d moving=%d stopReason=%d target=%.3f ctrl=%.3f\n",
                stage ? stage : "",
                note ? note : "",
                startupHomingEnabled ? 1 : 0,
                startupPositionCertain ? 1 : 0,
                startupSafetyLocked ? 1 : 0,
                startupSafetyReason,
                homingActive ? 1 : 0,
                resetReasonToString(esp_reset_reason()),
                openActive ? 1 : 0,
                closeActive ? 1 : 0,
                obstacle ? 1 : 0,
                fault,
                armed,
                telEnabled ? 1 : 0,
                telAge,
                telTimedOut ? 1 : 0,
                gateState,
                moving ? 1 : 0,
                stopReason,
                targetPos,
                ctrlPos);
}

static bool hoverTelemetryHealthyForStartup(uint32_t nowMs) {
  if (!motor || !motor->isHoverUart() || !motor->hoverEnabled()) return false;
  const HoverTelemetry& tel = motor->hoverTelemetry();
  uint32_t timeoutMs = config.gateConfig.telemetryTimeoutMs > 0 ? config.gateConfig.telemetryTimeoutMs : 1200;
  if (tel.lastTelMs == 0) return false;
  if (motor->hoverTelemetryTimedOut(nowMs, timeoutMs)) return false;
  if (tel.fault != 0) return false;
  if (abs(tel.rpm) > 8) return false;
  return true;
}

static bool hoverTelemetryHealthyForHomingMotion(uint32_t nowMs) {
  if (!motor || !motor->isHoverUart() || !motor->hoverEnabled()) return false;
  const HoverTelemetry& tel = motor->hoverTelemetry();
  uint32_t timeoutMs = config.gateConfig.telemetryTimeoutMs > 0 ? config.gateConfig.telemetryTimeoutMs : 1200;
  if (tel.lastTelMs == 0) return false;
  if (motor->hoverTelemetryTimedOut(nowMs, timeoutMs)) return false;
  if (tel.fault != 0) return false;
  // During active homing we expect non-zero RPM, so do not reject by rpm value here.
  return true;
}

static bool startupCanRunHoverHoming() {
  return motor && motor->isHoverUart() && motor->hoverEnabled();
}

static void restoreNormalMotionProfile() {
  if (!motor || !homingProfileApplied) return;
  motor->setMotionProfile(normalMotionProfile);
  homingProfileApplied = false;
}

static void setHomingSoftLimitsOverride(bool enabled) {
  if (enabled) {
    if (!homingSoftLimitsOverridden) {
      homingSoftLimitsPrev = config.gateConfig.softLimitsEnabled;
      config.gateConfig.softLimitsEnabled = false;
      homingSoftLimitsOverridden = true;
      Serial.printf("[HOMING] soft-limits override ON (prev=%d)\n", homingSoftLimitsPrev ? 1 : 0);
    }
  } else {
    if (homingSoftLimitsOverridden) {
      config.gateConfig.softLimitsEnabled = homingSoftLimitsPrev;
      homingSoftLimitsOverridden = false;
      Serial.printf("[HOMING] soft-limits override OFF (restored=%d)\n", config.gateConfig.softLimitsEnabled ? 1 : 0);
    }
  }
}

static void applySlowHomingProfile() {
  if (!motor || homingProfileApplied) return;
  normalMotionProfile = config.motionProfile();
  MotionAdvancedConfig p = normalMotionProfile;
  int homingScalePercent = config.gateConfig.homingScalePercent;
  homingScalePercent = constrain(homingScalePercent, 5, 100);
  p.maxSpeedOpen = max(4, (normalMotionProfile.maxSpeedOpen * homingScalePercent) / 100);
  p.maxSpeedClose = max(4, (normalMotionProfile.maxSpeedClose * homingScalePercent) / 100);
  p.minSpeed = max(2, (normalMotionProfile.minSpeed * homingScalePercent) / 100);
  if (p.minSpeed > p.maxSpeedClose - 1) p.minSpeed = max(1, p.maxSpeedClose - 1);
  if (p.minSpeed > p.maxSpeedOpen - 1) p.minSpeed = max(1, p.maxSpeedOpen - 1);
  motor->setMotionProfile(p);
  homingForceCmd = max(70, max(p.maxSpeedClose, p.maxSpeedOpen));
  Serial.printf("[HOMING] slow profile applied scale=%d%% open=%d close=%d min=%d forceCmd=%d (orig open=%d close=%d min=%d)\n",
                homingScalePercent, p.maxSpeedOpen, p.maxSpeedClose, p.minSpeed, homingForceCmd,
                normalMotionProfile.maxSpeedOpen, normalMotionProfile.maxSpeedClose, normalMotionProfile.minSpeed);
  homingProfileApplied = true;
}

static void applyStartupLimitReference() {
  // v2: Guard: wait for main loop to complete at least one handleInputs() so that
  // gate->limitOpenActive/limitCloseActive reflect real hardware state, not defaults.
  if (startupLimitRefDone || !gate || !startupHomingEnabled || !inputsInitialized) return;
  // Read limit states from gate volatile vars (written by main-loop handleInputs() via
  // gate->updateLimitState(); volatile guarantees cross-core visibility on Xtensa).
  bool openActive  = gate->getLimitOpenActive();
  bool closeActive = gate->getLimitCloseActive();
  if (closeActive && !openActive) {
    gate->onLimitClose();
    positionTracker.requestResyncClose();
    setStartupSafetyState(true, false, "close_limit_reference");
    setHomingResult(HOMING_RESULT_SKIP, HOMING_REASON_CLOSE_LIMIT_ACTIVE);
    Serial.println("[HOMING] startup reference from CLOSE limit (position certain)");
    homingChecked = true;
    startupLimitRefDone = true;
  } else if (openActive && !closeActive) {
    gate->onLimitOpen();
    positionTracker.requestResyncOpen();
    setStartupSafetyState(true, false, "open_limit_reference");
    setHomingResult(HOMING_RESULT_SKIP, HOMING_REASON_OPEN_LIMIT_ACTIVE);
    Serial.println("[HOMING] startup reference from OPEN limit (position certain)");
    homingChecked = true;
    startupLimitRefDone = true;
  } else if (openActive && closeActive) {
    gate->setError(GATE_ERR_LIMITS_INVALID, GATE_STOP_ERROR);
    setStartupSafetyState(false, true, "both_limits_active");
    setHomingResult(HOMING_RESULT_BLOCKED, HOMING_REASON_BOTH_LIMITS_ACTIVE);
    Serial.println("[HOMING] blocked: both limits active -> ERROR");
    homingChecked = true;
    startupLimitRefDone = true;
  } else {
    // Both limits inactive at restart -> unknown position.
    // If hover homing is unavailable in the current runtime config, do not loop forever
    // pretending that automatic reference is about to start. Hold a stable blocked state
    // until the user restores a valid runtime config or references the gate manually.
    if (!startupCanRunHoverHoming()) {
      startupUiUnknownPos10 = false;
      setStartupSafetyState(false, true, "manual_reference_required");
      setHomingResult(HOMING_RESULT_BLOCKED, HOMING_REASON_MANUAL_REF_REQUIRED);
      homingChecked = true;
      Serial.println("[HOMING] blocked: no startup homing path (limits inactive, hover unavailable)");
    } else {
      // Use temporary 100 mm helper distance until OPEN reference is found.
      startupUiUnknownPos10 = true;
      Serial.printf("[HOMING_TMP_POS] set mode=temp_100mm reason=limits_inactive_unknown_position mm=%ld\n",
                    (long)lroundf(kStartupTempDistanceMeters * 1000.0f));
      setStartupSafetyState(false, false, "limits_inactive_unknown_position");
      setHomingResult(HOMING_RESULT_PENDING, HOMING_REASON_LIMITS_INACTIVE_UNKNOWN);
      homingSearchOpen = true;
    }
    startupLimitRefDone = true;
  }
}

// === v2: Homing state machine — runs exclusively in GateTask ===
// Renamed from runStartupHoming(). All motor/gate mutations happen in GateTask
// context: no cross-task races on MotorController or GateController state.
// Limit switch state is read from gate->limitOpen/CloseActive (volatile bool,
// written by main-loop handleInputs() → gate->updateLimitState()).
static void runGateTaskHoming(uint32_t nowMs) {
  static const uint32_t kHomingReadyStableMs = 1200;
  static const uint32_t kHomingTelFailDebounceMs = 1500;
  static uint32_t lastDiagMs = 0;
  static uint32_t homingTelBadSinceMs = 0;
  uint32_t kHomingTimeoutMs = config.gateConfig.startupHomingTimeoutMs;
  if (kHomingTimeoutMs < 5000 || kHomingTimeoutMs > 300000) {
    kHomingTimeoutMs = 45000;
  }

  if (!startupHomingEnabled || !gate) return;
  if (lastDiagMs == 0 || (nowMs - lastDiagMs) > 15000) {
#if defined(GATE_DEBUG_HOMING)
    logStartupHomingDiag("loop", nowMs, "tick");
#endif
    lastDiagMs = nowMs;
  }
  applyStartupLimitReference();

  if (startupSafetyLocked) return;
  if (startupPositionCertain) {
    homingChecked = true;
    return;
  }

  if (homingChecked && !homingActive) return;

  const bool openActive  = gate->getLimitOpenActive();
  const bool closeActive = gate->getLimitCloseActive();
  // obstacle: read from inputManager (debounced by main loop, eventual-consistent — safe)
  const bool obstacle          = config.sensorsConfig.photocell.enabled && inputManager.obstacleActive();
  const bool criticalError     = gate->getErrorCode() != GATE_ERR_NONE || gate->getState() == GATE_ERROR;
  const bool telHealthyForStart  = hoverTelemetryHealthyForStartup(nowMs);
  const bool telHealthyForHoming = hoverTelemetryHealthyForHomingMotion(nowMs);

  if (!homingActive) {
    homingTelBadSinceMs = 0;
    if (closeActive) {
      gate->onLimitClose();
      positionTracker.requestResyncClose();
      setStartupSafetyState(true, false, "close_limit_reference");
      homingChecked = true;
      setHomingResult(HOMING_RESULT_SKIP, HOMING_REASON_CLOSE_LIMIT_ACTIVE);
      Serial.println("[HOMING] close limit active -> reference set, no movement");
      return;
    }
    if (obstacle && !homingSearchOpen) {
      setStartupSafetyState(false, false, "obstacle_active");
      setHomingResult(HOMING_RESULT_BLOCKED, HOMING_REASON_OBSTACLE_ACTIVE);
      return;
    }
    if (criticalError) {
      const GateErrorCode ec = gate->getErrorCode();
      const bool retryableCommErr = (ec == GATE_ERR_HOVER_TEL_TIMEOUT || ec == GATE_ERR_HOVER_OFFLINE);
      setStartupSafetyState(false, retryableCommErr ? false : true, retryableCommErr ? "wait_comm_recovery" : "critical_error");
      homingChecked = retryableCommErr ? false : true;
      setHomingResult(retryableCommErr ? HOMING_RESULT_PENDING : HOMING_RESULT_BLOCKED,
                   retryableCommErr ? HOMING_REASON_WAIT_COMM_RECOVERY : HOMING_REASON_CRITICAL_ERROR);
      return;
    }
    if (!telHealthyForStart) {
      const uint32_t kStartupTelGraceMs = 5000;
      if ((nowMs - startupBootMs) < kStartupTelGraceMs) {
        setHomingResult(HOMING_RESULT_PENDING, HOMING_REASON_WAIT_TEL_STARTUP);
        homingReadySinceMs = 0;
        return;
      }
      const HoverTelemetry& tel = motor->hoverTelemetry();
      if (tel.fault != 0) {
        gate->setError(GATE_ERR_HOVER_FAULT, GATE_STOP_HOVER_FAULT);
        setStartupSafetyState(false, true, "startup_tel_fault");
        setHomingResult(HOMING_RESULT_ERROR, HOMING_REASON_STARTUP_TEL_FAULT);
        Serial.printf("[HOMING] startup blocked: telemetry fault=%d -> ERROR\n", tel.fault);
      } else {
        // Telemetry may appear late after reboot / flash / hoverboard re-arm.
        // Do not permanently lock startup homing here; keep waiting and retry.
        setStartupSafetyState(false, false, "startup_tel_missing_retry");
        setHomingResult(HOMING_RESULT_PENDING, HOMING_REASON_STARTUP_TEL_MISSING);
        homingChecked = false;
        if (lastDiagMs == 0 || (nowMs - lastDiagMs) > 30000) {
#if defined(GATE_DEBUG_HOMING)
          Serial.println("[HOMING] startup telemetry missing -> keep waiting/retrying");
#endif
          lastDiagMs = nowMs;
        }
        homingReadySinceMs = 0;
        return;
      }
      homingChecked = true;
      homingReadySinceMs = 0;
      return;
    }
    if (homingReadySinceMs == 0) homingReadySinceMs = nowMs;
    if (nowMs - homingReadySinceMs < kHomingReadyStableMs) return;

    applySlowHomingProfile();
    setHomingSoftLimitsOverride(true);
    bool started = false;
    const bool searchOpen = homingSearchOpen;
    if (motor) {
      motor->setDirection(searchOpen);
      if (motor->isHoverUart()) {
        motor->hoverArm();
        motor->setHoverTargetSpeed((int16_t)(searchOpen ? homingForceCmd : -homingForceCmd));
        started = true;
        Serial.printf("[HOMING] force start %s via hover speed=%d\n",
                      searchOpen ? "OPEN" : "CLOSE",
                      searchOpen ? homingForceCmd : -homingForceCmd);
      } else {
        // FIX: setDuty() clamps to [0, maxDuty]; direction is already set by setDirection().
        // Always pass positive duty — negative duty would be clamped to 0 (motor stall).
        int duty = max(40, homingForceCmd);
        motor->setDuty(duty);
        started = true;
        Serial.printf("[HOMING] force start %s via PWM duty=%d\n",
                      searchOpen ? "OPEN" : "CLOSE",
                      duty);
      }
    }
    if (!started) {
      setHomingSoftLimitsOverride(false);
      restoreNormalMotionProfile();
      homingChecked = true;
      setStartupSafetyState(false, true, "start_failed");
      setHomingResult(HOMING_RESULT_BLOCKED, HOMING_REASON_START_FAILED);
      Serial.println("[HOMING] start failed (no motor path)");
      return;
    }
    homingActive = true;
    homingStartMs = nowMs;
    moveStartPosition = gate->getControlPosition();
    setStartupSafetyState(false, false, "homing_running");
    setHomingResult(HOMING_RESULT_RUNNING, searchOpen ? HOMING_REASON_SEARCH_OPEN : HOMING_REASON_SEARCH_CLOSE);
    Serial.printf("[HOMING] start -> %s at slow profile\n", searchOpen ? "OPEN" : "CLOSE");
    return;
  }

  // Active homing supervision.
  const bool searchOpen = homingSearchOpen;
  if (motor) {
    motor->setDirection(searchOpen);
    if (motor->isHoverUart()) {
      motor->hoverArm();
      motor->setHoverTargetSpeed((int16_t)(searchOpen ? homingForceCmd : -homingForceCmd));
    } else {
      // FIX: setDuty() clamps to [0, maxDuty]; direction is already set by setDirection().
      motor->setDuty(max(40, homingForceCmd));
    }
  }
  if (searchOpen ? openActive : closeActive) {
    if (searchOpen) {
      gate->onLimitOpen();
      positionTracker.requestResyncOpen();
    } else {
      gate->onLimitClose();
      positionTracker.requestResyncClose();
    }
    if (motor) {
      if (motor->isHoverUart()) motor->setHoverTargetSpeed(0);
      motor->stopHard();
    }
    homingActive = false;
    homingChecked = true;
    setHomingSoftLimitsOverride(false);
    restoreNormalMotionProfile();
    setStartupSafetyState(true, false, searchOpen ? "open_limit_found" : "close_limit_found");
    setHomingResult(HOMING_RESULT_SUCCESS, searchOpen ? HOMING_REASON_OPEN_LIMIT_FOUND : HOMING_REASON_CLOSE_LIMIT_FOUND);
    Serial.printf("[HOMING] success (%s limit)\n", searchOpen ? "open" : "close");
    return;
  }

  if (obstacle && !searchOpen) {
    gate->stop(GATE_STOP_OBSTACLE);
    gate->setError(GATE_ERR_OBSTACLE, GATE_STOP_OBSTACLE);
    homingActive = false;
    homingChecked = true;
    setHomingSoftLimitsOverride(false);
    restoreNormalMotionProfile();
    setStartupSafetyState(false, true, "obstacle_during_homing");
    setHomingResult(HOMING_RESULT_ABORT, HOMING_REASON_OBSTACLE_DURING_HOMING);
    Serial.println("[HOMING] abort: obstacle");
    return;
  }

  if (!telHealthyForHoming) {
    if (homingTelBadSinceMs == 0) homingTelBadSinceMs = nowMs;
    if (nowMs - homingTelBadSinceMs < kHomingTelFailDebounceMs) {
      return;
    }
    gate->stop(GATE_STOP_TELEMETRY_TIMEOUT);
    gate->setError(GATE_ERR_HOVER_TEL_TIMEOUT, GATE_STOP_TELEMETRY_TIMEOUT);
    homingActive = false;
    homingChecked = false;
    setHomingSoftLimitsOverride(false);
    restoreNormalMotionProfile();
    setStartupSafetyState(false, false, "telemetry_lost_or_fault");
    setHomingResult(HOMING_RESULT_ABORT, HOMING_REASON_TEL_LOST_OR_FAULT);
    Serial.println("[HOMING] abort: telemetry lost/fault -> retry pending");
    return;
  }
  homingTelBadSinceMs = 0;

  if (gate->getErrorCode() != GATE_ERR_NONE || gate->getState() == GATE_ERROR) {
    homingActive = false;
    homingChecked = true;
    setHomingSoftLimitsOverride(false);
    restoreNormalMotionProfile();
    setStartupSafetyState(false, true, "gate_error_during_homing");
    setHomingResult(HOMING_RESULT_ABORT, HOMING_REASON_GATE_ERROR_DURING_HOMING);
    Serial.println("[HOMING] abort: gate error");
    return;
  }

  float maxDistance = gate->getMaxDistance();
  if (maxDistance <= 0.0f) maxDistance = 4.0f;
  const float traveled = fabsf(gate->getControlPosition() - moveStartPosition);
  const float maxAllowedTravel = maxDistance * 1.25f;
  if (traveled > maxAllowedTravel) {
    gate->stop(GATE_STOP_ERROR);
    gate->setError(GATE_ERR_TIMEOUT, GATE_STOP_ERROR);
    homingActive = false;
    homingChecked = true;
    setHomingSoftLimitsOverride(false);
    restoreNormalMotionProfile();
    setStartupSafetyState(false, true, "distance_limit");
    setHomingResult(HOMING_RESULT_ABORT, HOMING_REASON_DISTANCE_LIMIT);
    Serial.printf("[HOMING] abort: distance limit traveled=%.2fm limit=%.2fm\n", traveled, maxAllowedTravel);
    return;
  }

  if (nowMs - homingStartMs > kHomingTimeoutMs) {
    gate->stop(GATE_STOP_ERROR);
    gate->setError(GATE_ERR_TIMEOUT, GATE_STOP_ERROR);
    homingActive = false;
    homingChecked = true;
    setHomingSoftLimitsOverride(false);
    restoreNormalMotionProfile();
    setStartupSafetyState(false, true, "timeout");
    setHomingResult(HOMING_RESULT_ABORT, HOMING_REASON_TIMEOUT);
    Serial.println("[HOMING] abort: timeout");
    return;
  }
}

const char* resetReasonToString(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_UNKNOWN: return "unknown";
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "ext";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "other_wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "invalid";
  }
}

void syncLegacyPositionState() {
  positionMeters = positionTracker.positionMeters();
  positionMetersRaw = positionTracker.positionMetersRaw();
  maxDistanceMeters = positionTracker.maxDistanceMeters();
  positionPercent = positionTracker.positionPercent();
  hallPps = positionTracker.hallPps();
}

void pushEvent(const char* level, const char* code, const char* message) {
#if !defined(GATE_LOG_INFO_EVENTS)
  if (level && strcmp(level, "info") == 0) return;
#endif
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
  if (eventQueue) {
    EventEntry out = e;
    if (xQueueSend(eventQueue, &out, 0) != pdTRUE) {
      static uint32_t lastOverflowLogMs = 0;
      uint32_t now = millis();
      if (lastOverflowLogMs == 0 || now - lastOverflowLogMs > 2000) {
        Serial.println("[EVENT] queue overflow, dropping event");
        lastOverflowLogMs = now;
      }
    }
  }
}

void pushEvent(const char* level, const char* message) {
  pushEvent(level, "", message);
}

void pushEventf(const char* level, const char* fmt, unsigned long value) {
  char buf[64];
  snprintf(buf, sizeof(buf), fmt, value);
  pushEvent(level, buf);
}

void drainEventQueue() {
  if (!eventQueue) return;
  EventEntry e;
  while (xQueueReceive(eventQueue, &e, 0) == pdTRUE) {
    webserver.broadcastEvent(e.level, e.message);
    mqttPublishEvent(e.level, e.message);
  }
}

bool isOtaActive() {
  return otaActive;
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
  doc["state"] = effectiveGateStateString();
  doc["moving"] = effectiveGateMoving();
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
    gateObj["state"] = effectiveGateStateString();
    gateObj["moving"] = effectiveGateMoving();
    gateObj["position"] = st.position;
    gateObj["positionPercent"] = st.positionPercent;
    gateObj["targetPosition"] = st.targetPosition;
    gateObj["maxDistance"] = st.maxDistance;
    gateObj["errorCode"] = static_cast<int>(st.error);
    gateObj["stopReason"] = static_cast<int>(st.lastStopReason);
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
    updateChargerConnectedFromTelemetry(tel, millis());
    hbObj["dir"] = tel.dir;
    hbObj["rpm"] = tel.rpm;
    // Normalized distance in mm (0..max). This matches the gate position and doesn't go negative.
    hbObj["dist_mm"] = (long)lroundf(positionMetersRaw * 1000.0f);
    // Keep raw telemetry for debugging.
    hbObj["dist_mm_raw"] = tel.distMm;
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
    hbObj["armed"] = tel.armed;
    hbObj["cmdAgeMs"] = tel.cmdAgeMs;
    hbObj["lastTelMs"] = tel.lastTelMs;
    hbObj["telAgeMs"] = (tel.lastTelMs == 0) ? -1 : (long)(millis() - tel.lastTelMs);
    hbObj["chargerConnected"] = chargerStateKnown ? chargerConnected : false;
    hbObj["chargerKnown"] = chargerStateKnown;
    hbObj["chargerPending"] = chargerPending;
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
    hbObj["armed"] = false;
    hbObj["cmdAgeMs"] = -1;
    hbObj["lastTelMs"] = 0;
    hbObj["telAgeMs"] = -1;
    hbObj["chargerConnected"] = false;
    hbObj["chargerKnown"] = false;
    hbObj["chargerPending"] = false;
  }

  doc["ts"] = millis();
  char payload[1024];
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

void logSummary1Hz() {
#if !defined(GATE_LOG_PERIODIC)
  return;
#endif
  static unsigned long lastLogMs = 0;
  unsigned long now = millis();
  const uint32_t logIntervalMs = (!startupPositionCertain && !homingActive) ? 15000 : 5000;
  if (now - lastLogMs < logIntervalMs) return;
  lastLogMs = now;
  const WebRuntimeStats ws = webserver.runtimeStats();
  const bool wifiConnected = WiFiManager.isConnected();
  const UBaseType_t mainLoopStackWords = uxTaskGetStackHighWaterMark(NULL);
  const UBaseType_t gateTaskStackWords = gateTaskHandle ? uxTaskGetStackHighWaterMark(gateTaskHandle) : 0;
  const long loopAgeMs = (mainLoopHeartbeatMs == 0 || now < mainLoopHeartbeatMs) ? -1 : (long)(now - mainLoopHeartbeatMs);
  const long gateAgeMs = (gateTaskHeartbeatMs == 0 || now < gateTaskHeartbeatMs) ? -1 : (long)(now - gateTaskHeartbeatMs);
  Serial.printf("[SYS] up=%lums boot=%lu reset=%s(%d) loopAge=%ld gateAge=%ld heap=%u minHeap=%u maxAlloc=%u stackMain=%u stackGate=%u wifi=%d mode=%s ip=%s ws=%u apiReq=%lu statusReq=%lu liteReq=%lu statusErr=%lu slow=%lu statusLastUs=%lu statusMaxUs=%lu\n",
                now,
                (unsigned long)bootCount,
                resetReasonToString(bootResetReason),
                (int)bootResetReason,
                loopAgeMs,
                gateAgeMs,
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMinFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap(),
                (unsigned)mainLoopStackWords,
                (unsigned)gateTaskStackWords,
                wifiConnected ? 1 : 0,
                WiFiManager.getModeCString(),
                wifiConnected ? WiFi.localIP().toString().c_str() : "",
                (unsigned)ws.wsClients,
                (unsigned long)ws.apiReqCount,
                (unsigned long)ws.statusReqCount,
                (unsigned long)ws.statusLiteReqCount,
                (unsigned long)ws.statusErrors,
                (unsigned long)ws.statusSlowCount,
                (unsigned long)ws.lastStatusDurationUs,
                (unsigned long)ws.maxStatusDurationUs);
  if (gate) {
    Serial.printf("[GATE] state=%s pos=%.3fm target=%.3fm max=%.3fm stopReason=%s err=%d\n",
                  effectiveGateStateString(),
                  gate->getPosition(),
                  gate->getTargetPosition(),
                  gate->getMaxDistance(),
                  gate->getStopReasonString(gate->getLastStopReason()),
                  (int)gate->getErrorCode());
  }
  if (motor && motor->isHoverUart() && motor->hoverEnabled()) {
    const HoverTelemetry& tel = motor->hoverTelemetry();
    long telAge = (tel.lastTelMs == 0 || now < tel.lastTelMs) ? -1 : (long)(now - tel.lastTelMs);
    Serial.printf("[HB] telAge=%ld cmdAge=%d rpm=%d dist=%ldmm iA=%.2f fault=%d armed=%d rx=%lu tel=%lu bad=%lu\n",
                  telAge,
                  tel.cmdAgeMs,
                  tel.rpm,
                  tel.distMm,
                  tel.iA_x100 >= 0 ? ((float)tel.iA_x100) / 100.0f : -1.0f,
                  tel.fault,
                  tel.armed ? 1 : 0,
                  (unsigned long)motor->hoverRxLines(),
                  (unsigned long)motor->hoverRxTelLines(),
                  (unsigned long)motor->hoverRxBadLines());
  }
}

void mqttPublishMotionState() {
  const char* topic = mqtt.topicMotionState();
  if (!mqtt.connected() || !topic || topic[0] == '\0') return;
  StaticJsonDocument<256> doc;
  JsonObject gateObj = doc.createNestedObject("gate");
  if (gate) {
    const GateStatus& st = gate->getStatus();
    gateObj["state"] = effectiveGateStateString();
    gateObj["moving"] = effectiveGateMoving();
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
  doc["state"] = effectiveGateStateString();
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
      if (action[0] != '\0') {
        // FIX #2: route via queue
        if (!sendGateCommand(action)) pushEvent("warn", "mqtt command throttled");
      }
    }
    return;
  }
  if (isCmd || isGateCmd) {
    // FIX #2: route via queue
    if (!sendGateCommand(buf)) pushEvent("warn", "mqtt command throttled");
  }
}

bool applyMaxDistance(float value, bool persist) {
  bool ok = positionTracker.applyMaxDistance(value, persist);
  syncLegacyPositionState();
  return ok;
}

bool handleGateCalibrate(const char* mode) {
  bool ok = positionTracker.calibrateToMode(mode, false);
  syncLegacyPositionState();
  return ok;
}

void setupInputs() {
  inputManager.begin(config);
}

void handleInputs() {
  if (!gate) return;

  InputEvents ev = inputManager.poll(config, millis());
  const bool photocellEnabled = config.sensorsConfig.photocell.enabled;
  gate->updateLimitState(inputManager.limitOpenActive(config), inputManager.limitCloseActive(config));

  if (ev.stopPressed) {
    // v2.1: route via GATE_NOTIFY_STOP — gate->stop() executes in GateTask.
    if (gateTaskHandle) xTaskNotify(gateTaskHandle, GATE_NOTIFY_STOP, eSetBits);
    pushEvent("warn", "stop input");
  }

  if (ev.obstacleChanged) {
    if (photocellEnabled) {
      // v2.1: route gate mutation via GATE_NOTIFY_OBSTACLE + volatile state.
      // Main loop: push conservative event. GateTask: calls gate->onObstacle().
      obstacleNotifyActive = ev.obstacleActive;
      if (gateTaskHandle) xTaskNotify(gateTaskHandle, GATE_NOTIFY_OBSTACLE, eSetBits);
      if (ev.obstacleActive) {
        pushEvent("warn", "obstacle detected");
      }
    }
  }

  if (ev.limitsInvalid) {
    if (ev.limitsInvalidEdge && gate->getErrorCode() != GATE_ERR_LIMITS_INVALID) {
      Serial.printf("[INPUT] limits_invalid open=%d close=%d\n",
                    inputManager.limitOpenActive(config) ? 1 : 0,
                    inputManager.limitCloseActive(config) ? 1 : 0);
      pushEvent("error", "limits_invalid (both active)");
    }
    // v2.1: route gate mutation via GATE_NOTIFY_LIMITS_INVALID — executes in GateTask.
    if (gateTaskHandle) xTaskNotify(gateTaskHandle, GATE_NOTIFY_LIMITS_INVALID, eSetBits);
  }

  if (ev.limitOpenRising) {
    led.setOverride("limit_open_hit", 380);
    // v2.1: route to GateTask via xTaskNotify — gate->onLimitOpen() mutates state machine.
    if (gateTaskHandle) xTaskNotify(gateTaskHandle, GATE_NOTIFY_LIMIT_OPEN, eSetBits);
    pushEvent("info", "limit open");
  }

  if (ev.limitCloseRising) {
    led.setOverride("limit_close_hit", 380);
    // v2.1: route to GateTask via xTaskNotify — gate->onLimitClose() mutates state machine.
    if (gateTaskHandle) xTaskNotify(gateTaskHandle, GATE_NOTIFY_LIMIT_CLOSE, eSetBits);
    pushEvent("info", "limit close");
  }

  if (ev.buttonPressed) {
    // FIX #2: enqueue toggle — GateTask processes it in safe context
    if (sendGateCommand("toggle")) {
      pushEvent("info", "button toggle");
    } else {
      pushEvent("warn", "button toggle throttled");
      led.setOverride("command_rejected", 300);
    }
  }
  // v2: Signal GateTask that limit states are now valid (at least one poll done).
  // GateTask homing waits for this before reading gate->limitOpenActive/limitCloseActive.
  inputsInitialized = true;
}

void IRAM_ATTR hallIsr() {
  // Hall ISR moved to PositionTracker in Phase 1.
}

void updateHallAttachment() {
  positionTracker.updateHallAttachment(false);
}

void updatePositionPercent() {
  if (gate && startupUiUnknownPos10 && !startupPositionCertain) {
    float maxD = gate->getMaxDistance();
    if (maxD <= 0.0f) maxD = config.gateConfig.maxDistance > 0.0f ? config.gateConfig.maxDistance : config.gateConfig.totalDistance;
    if (maxD <= 0.0f) maxD = 3.0f;
    float synthetic = kStartupTempDistanceMeters;
    if (maxD > 0.0f && synthetic > maxD) synthetic = maxD;
    gate->setPosition(synthetic, maxD);
    gate->setControlPosition(synthetic);
    positionMeters = synthetic;
    positionMetersRaw = synthetic;
    maxDistanceMeters = maxD;
    positionPercent = (maxD > 0.0f)
      ? (int)((synthetic * 100.0f) / maxD + 0.5f)
      : -1;
    const uint32_t nowMs = millis();
    if (startupSyntheticUiLogMs == 0 || (nowMs - startupSyntheticUiLogMs) > 30000) {
      startupSyntheticUiLogMs = nowMs;
      Serial.printf("[HOMING_TMP_POS] apply mode=temp_100mm pos=%.3fm posRaw=%.3fm pct=%d active=%d certain=%d\n",
                    positionMeters,
                    positionMetersRaw,
                    positionPercent,
                    homingActive ? 1 : 0,
                    startupPositionCertain ? 1 : 0);
    }
    return;
  }
  positionTracker.updatePosition(false);
  syncLegacyPositionState();
}

void updateHallStats(uint32_t nowMs) {
  positionTracker.updateHallStats(nowMs);
  hallPps = positionTracker.hallPps();
}

void onGateStatusChanged(const GateStatus& status, void* ctx) {
  (void)ctx;
  led.setState(status.state,
               status.error,
               status.lastStopReason,
               status.obstacle,
               homingActive,
               status.wifiConnected,
               status.mqttConnected,
               status.positionPercent,
               status.apMode,
               status.otaInProgress);
}

static void fillRuntimeSnapshot(JsonObject& runtimeObj, const WebRuntimeStats& ws, uint32_t nowMs) {
  const long mainLoopAgeMs = (mainLoopHeartbeatMs == 0 || nowMs < mainLoopHeartbeatMs)
                               ? -1
                               : (long)(nowMs - mainLoopHeartbeatMs);
  const long gateTaskAgeMs = (gateTaskHeartbeatMs == 0 || nowMs < gateTaskHeartbeatMs)
                               ? -1
                               : (long)(nowMs - gateTaskHeartbeatMs);
  const long webMaintenanceAgeMs = (ws.lastMaintenanceMs == 0 || nowMs < ws.lastMaintenanceMs)
                                     ? -1
                                     : (long)(nowMs - ws.lastMaintenanceMs);

  runtimeObj["hardDiagVer"] = 1;
  runtimeObj["uptimeMs"] = nowMs;
  runtimeObj["bootCount"] = bootCount;
  runtimeObj["resetReasonCode"] = (int)bootResetReason;
  runtimeObj["resetReason"] = resetReasonToString(bootResetReason);
  runtimeObj["scheduledRestartReason"] = bootRestartReason[0] ? bootRestartReason : "";
  runtimeObj["restartPending"] = restartPending;
  runtimeObj["pendingRestartReason"] = pendingRestartReason[0] ? pendingRestartReason : "";
  runtimeObj["prevResetReasonCodeRtc"] = (int)prevRtcResetReason;
  runtimeObj["prevResetReasonRtc"] = resetReasonToString((esp_reset_reason_t)prevRtcResetReason);
  runtimeObj["freeHeap"] = ESP.getFreeHeap();
  runtimeObj["minFreeHeap"] = ESP.getMinFreeHeap();
  runtimeObj["maxAllocHeap"] = ESP.getMaxAllocHeap();
  runtimeObj["mainLoopStackWords"] = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
  runtimeObj["gateTaskStackWords"] = (uint32_t)(gateTaskHandle ? uxTaskGetStackHighWaterMark(gateTaskHandle) : 0);
  runtimeObj["mainLoopHeartbeatMs"] = (uint32_t)mainLoopHeartbeatMs;
  runtimeObj["mainLoopAgeMs"] = mainLoopAgeMs;
  runtimeObj["gateTaskHeartbeatMs"] = (uint32_t)gateTaskHeartbeatMs;
  runtimeObj["gateTaskAgeMs"] = gateTaskAgeMs;
  runtimeObj["apiReqCount"] = ws.apiReqCount;
  runtimeObj["statusReqCount"] = ws.statusReqCount;
  runtimeObj["statusLiteReqCount"] = ws.statusLiteReqCount;
  runtimeObj["statusErrors"] = ws.statusErrors;
  runtimeObj["statusSlowCount"] = ws.statusSlowCount;
  runtimeObj["lastApiReqMs"] = ws.lastApiReqMs;
  runtimeObj["lastStatusReqMs"] = ws.lastStatusReqMs;
  runtimeObj["lastStatusDurationUs"] = ws.lastStatusDurationUs;
  runtimeObj["maxStatusDurationUs"] = ws.maxStatusDurationUs;
  runtimeObj["lastWebMaintenanceMs"] = ws.lastMaintenanceMs;
  runtimeObj["webMaintenanceAgeMs"] = webMaintenanceAgeMs;
  runtimeObj["lastWsConnectMs"] = ws.lastWsConnectMs;
  runtimeObj["lastWsDisconnectMs"] = ws.lastWsDisconnectMs;
  runtimeObj["wsClients"] = ws.wsClients;
  runtimeObj["bodyBuffers"] = ws.bodyBuffers;
  runtimeObj["bodyBufferCleanupCount"] = ws.bodyBufferCleanupCount;
}

void fillDiagnostics(JsonObject& out) {
  const WebRuntimeStats ws = webserver.runtimeStats();
  const uint32_t nowMs = millis();
  JsonObject runtimeObj = out.createNestedObject("runtime");
  fillRuntimeSnapshot(runtimeObj, ws, nowMs);

  JsonObject hoverObj = out.createNestedObject("hoverUart");
  if (motor && motor->isHoverUart() && motor->hoverEnabled()) {
    const HoverTelemetry& tel = motor->hoverTelemetry();
    updateChargerConnectedFromTelemetry(tel, millis());
    uint32_t now = millis();
    hoverObj["enabled"] = true;
    hoverObj["lastTelMs"] = tel.lastTelMs;
    hoverObj["telAgeMs"] = (tel.lastTelMs == 0 || now < tel.lastTelMs) ? -1 : (long)(now - tel.lastTelMs);
    hoverObj["rpm"] = tel.rpm;
    // Normalized distance in mm (0..max). This is what the UI should treat as "przejechana odlegĹ‚oĹ›Ä‡".
    hoverObj["dist_mm"] = (long)lroundf(positionMetersRaw * 1000.0f);
    hoverObj["dist_mm_raw"] = tel.distMm;
    hoverObj["batValid"] = tel.batValid;
    hoverObj["rawBat"] = tel.rawBat;
    hoverObj["batScale"] = tel.batScale;
    if (tel.batValid) hoverObj["batV"] = tel.batV;
    else hoverObj["batV"] = nullptr;
    if (tel.iA_x100 >= 0) hoverObj["iA"] = ((float)tel.iA_x100) / 100.0f;
    else hoverObj["iA"] = -1.0f;
    hoverObj["fault"] = tel.fault;
    hoverObj["armed"] = tel.armed;
    hoverObj["chargerConnected"] = chargerStateKnown ? chargerConnected : false;
    hoverObj["chargerKnown"] = chargerStateKnown;
    hoverObj["chargerPending"] = chargerPending;
    hoverObj["lastCmdSpeed"] = motor->hoverLastCmdSpeed();
    hoverObj["cmdAgeMs"] = tel.cmdAgeMs;
    hoverObj["rxLines"] = motor->hoverRxLines();
    hoverObj["rxTelLines"] = motor->hoverRxTelLines();
    hoverObj["rxBadLines"] = motor->hoverRxBadLines();
  } else {
    hoverObj["enabled"] = false;
    hoverObj["iA"] = -1.0f;
    hoverObj["chargerConnected"] = false;
    hoverObj["chargerKnown"] = false;
    hoverObj["chargerPending"] = false;
  }

  JsonObject positionObj = out.createNestedObject("position");
  positionObj["positionMetersRaw"] = positionMetersRaw;
  positionObj["positionMetersFiltered"] = positionMeters;
  positionObj["hbOriginDistMm"] = config.gateConfig.hbOriginDistMm;
  positionObj["maxDistanceMeters"] = maxDistanceMeters;
  JsonObject gateDiag = out.createNestedObject("gate");
  if (gate) {
    gateDiag["stopReason"] = static_cast<int>(gate->getLastStopReason());
    gateDiag["stopReasonLabel"] = gate->getStopReasonString(gate->getLastStopReason());
    gateDiag["lastOverCurrentA"] = gate->getLastOverCurrentA();
    gateDiag["lastOverCurrentMs"] = gate->getLastOverCurrentMs();
    gateDiag["overCurrentCooldownUntilMs"] = gate->getOverCurrentCooldownUntilMs();
    gateDiag["overCurrentAutoRearmCount"] = gate->getOverCurrentAutoRearmCount();
    gateDiag["hoverRecoveryActive"] = gate->isHoverRecoveryActive();
    gateDiag["lastHoverRestoreMs"] = gate->getLastHoverRestoreMs();
    gateDiag["lastHoverLossMs"] = gate->getLastHoverLossMs();
    // FIX #6: limit-safety diagnostic counters
    gateDiag["limitSafetyStopCount"] = gate->getLimitSafetyStopCount();
    gateDiag["limitActiveWhileMovingMs"] = gate->getLimitActiveWhileMovingMs();
  } else {
    gateDiag["stopReason"] = 0;
    gateDiag["stopReasonLabel"] = "none";
  }
  gateDiag["softLimitsEnabled"] = config.gateConfig.softLimitsEnabled;
  JsonObject cmdIngressObj = gateDiag.createNestedObject("commandIngress");
  cmdIngressObj["accepted"] = gateCommandIngress.accepted;
  cmdIngressObj["rateLimited"] = gateCommandIngress.rateLimited;
  cmdIngressObj["duplicates"] = gateCommandIngress.duplicates;
  cmdIngressObj["queueFull"] = gateCommandIngress.queueFull;
  cmdIngressObj["stopNotifications"] = gateCommandIngress.stopNotifications;
  cmdIngressObj["lastAcceptedMs"] = gateCommandIngress.lastAcceptedMs;
  cmdIngressObj["lastAction"] = gateCommandIngress.lastAction;
  gateDiag["telemetryTimeoutMs"] = config.gateConfig.telemetryTimeoutMs;
  gateDiag["telemetryGraceMs"] = config.gateConfig.telemetryGraceMs;
  gateDiag["stallTimeoutMs"] = config.gateConfig.stallTimeoutMs;
  gateDiag["overCurrentTripMs"] = config.gateConfig.overCurrentTripMs;
  gateDiag["overCurrentCooldownMs"] = config.gateConfig.overCurrentCooldownMs;
  gateDiag["overCurrentMaxAutoRearm"] = config.gateConfig.overCurrentMaxAutoRearm;
  gateDiag["overCurrentAutoRearm"] = config.gateConfig.overCurrentAutoRearm;
  if (gate) {
    positionObj["positionMmCtrl"] = (long)lroundf(gate->getControlPosition() * 1000.0f);
    positionObj["finalErrorMm"] = gate->getLastFinalErrorMm();
    positionObj["stopConfirmCount"] = gate->getStopConfirmCount();
  } else {
    positionObj["positionMmCtrl"] = (long)lroundf(positionMeters * 1000.0f);
    positionObj["finalErrorMm"] = 0;
    positionObj["stopConfirmCount"] = 0;
  }

  JsonObject remotesObj = out.createNestedObject("remotes");
  remotesObj["lastSaveOk"] = config.getLastRemotesSaveOk();
  remotesObj["lastSaveMs"] = config.getLastRemotesSaveMs();
  remotesObj["lastSaveError"] = config.getLastRemotesSaveError();

  JsonObject otaObj = out.createNestedObject("ota");
  otaObj["enabled"] = config.otaConfig.enabled;
  otaObj["ready"] = otaReady;
  otaObj["active"] = otaActive;
  otaObj["progress"] = otaProgress;
  otaObj["error"] = otaError;
  otaObj["port"] = config.otaConfig.port;
  otaObj["hostname"] = config.deviceConfig.hostname;
  otaObj["passwordSet"] = config.otaConfig.password.length() > 0;
  otaObj["wifiConnected"] = WiFiManager.isConnected();
  otaObj["ip"] = WiFiManager.isConnected() ? WiFi.localIP().toString() : "";
  otaObj["freeSketchSpace"] = ESP.getFreeSketchSpace();
  out["build"] = FW_VERSION;

  JsonObject wifiObj = out.createNestedObject("wifi");
  const bool wifiConnected = WiFiManager.isConnected();
  wifiObj["connected"] = wifiConnected;
  wifiObj["mode"] = WiFiManager.getModeCString();
  wifiObj["status"] = WiFiManager.getLastStatusCString();
  wifiObj["statusCode"] = WiFiManager.getLastStatus();
  wifiObj["ssid"] = wifiConnected ? WiFi.SSID() : "";
  wifiObj["ip"] = wifiConnected ? WiFi.localIP().toString() : "";
  wifiObj["rssi"] = wifiConnected ? WiFi.RSSI() : 0;
  wifiObj["reconnectAttempts"] = WiFiManager.getReconnectAttempts();
  wifiObj["retryIntervalMs"] = WiFiManager.getRetryIntervalMs();

  JsonObject homingObj = out.createNestedObject("homing");
  homingObj["active"] = homingActive;
  homingObj["reason"] = homingReasonStr((HomingReason)homingReasonEnum);
  homingObj["result"] = homingResultStr((HomingResult)homingResultEnum);
  homingObj["lastChangeMs"] = homingLastChangeMs;
  homingObj["enabled"] = startupHomingEnabled;
  homingObj["positionCertain"] = startupPositionCertain;
  homingObj["safetyLocked"] = startupSafetyLocked;
  homingObj["safetyReason"] = startupSafetyReason;
}

void fillStatus(JsonObject& out) {
  syncLegacyPositionState();
  const WebRuntimeStats ws = webserver.runtimeStats();
  const uint32_t nowMs = millis();
  out["uptimeMs"] = nowMs;
  JsonObject runtimeObj = out.createNestedObject("runtime");
  fillRuntimeSnapshot(runtimeObj, ws, nowMs);

  JsonObject gateObj = out.createNestedObject("gate");
  if (gate) {
    const GateStatus& st = gate->getStatus();
    gateObj["state"] = effectiveGateStateString();
    gateObj["moving"] = effectiveGateMoving();
    gateObj["position"] = st.position;
    gateObj["positionPercent"] = st.positionPercent;
    gateObj["targetPosition"] = st.targetPosition;
    gateObj["maxDistance"] = st.maxDistance;
    gateObj["lastDirection"] = gate->getLastDirection();
    gateObj["errorCode"] = static_cast<int>(st.error);
    gateObj["stopReason"] = static_cast<int>(st.lastStopReason);
    gateObj["obstacle"] = st.obstacle;
    gateObj["lastMoveMs"] = st.lastMoveMs;
    gateObj["lastStateChangeMs"] = st.lastStateChangeMs;
    gateObj["hoverRecoveryActive"] = gate->isHoverRecoveryActive();
    gateObj["lastHoverRestoreMs"] = gate->getLastHoverRestoreMs();
    gateObj["lastHoverLossMs"] = gate->getLastHoverLossMs();
    if (startupUiUnknownPos10 && !startupPositionCertain) {
      const float maxD = st.maxDistance > 0.0f ? st.maxDistance : maxDistanceMeters;
      float synthetic = kStartupTempDistanceMeters;
      if (maxD > 0.0f && synthetic > maxD) synthetic = maxD;
      gateObj["position"] = synthetic;
      gateObj["positionPercent"] = (maxD > 0.0f) ? (int)((synthetic * 100.0f) / maxD + 0.5f) : -1;
      gateObj["targetPosition"] = synthetic;
    }
  } else {
    gateObj["state"] = "unknown";
    gateObj["moving"] = false;
    gateObj["position"] = positionMeters;
    gateObj["positionPercent"] = positionPercent;
    gateObj["targetPosition"] = 0.0f;
    gateObj["maxDistance"] = maxDistanceMeters;
    gateObj["lastDirection"] = 0;
    gateObj["errorCode"] = 0;
    gateObj["stopReason"] = 0;
    gateObj["obstacle"] = false;
    gateObj["lastMoveMs"] = 0;
    gateObj["lastStateChangeMs"] = 0;
    gateObj["hoverRecoveryActive"] = false;
    gateObj["lastHoverRestoreMs"] = 0;
    gateObj["lastHoverLossMs"] = 0;
  }

  JsonObject wifiObj = out.createNestedObject("wifi");
  bool wifiConnected = WiFiManager.isConnected();
  wifiObj["connected"] = wifiConnected;
  const char* wifiMode = WiFiManager.getModeCString();
  wifiObj["mode"] = wifiMode;
  wifiObj["ssid"] = wifiConnected ? WiFi.SSID() : "";
  wifiObj["ip"] = wifiConnected ? WiFi.localIP().toString() : "";
  wifiObj["rssi"] = wifiConnected ? WiFi.RSSI() : 0;
  wifiObj["apMode"] = strcmp(wifiMode, "AP") == 0;
  wifiObj["status"] = WiFiManager.getLastStatusCString();
  wifiObj["statusCode"] = WiFiManager.getLastStatus();
  wifiObj["reconnectAttempts"] = WiFiManager.getReconnectAttempts();
  wifiObj["retryIntervalMs"] = WiFiManager.getRetryIntervalMs();

  JsonObject mqttObj = out.createNestedObject("mqtt");
  mqttObj["connected"] = mqtt.connected();

  JsonObject otaObj = out.createNestedObject("ota");
  otaObj["enabled"] = config.otaConfig.enabled;
  otaObj["ready"] = otaReady;
  otaObj["active"] = otaActive;
  otaObj["progress"] = otaProgress;
  otaObj["error"] = otaError;
  otaObj["port"] = config.otaConfig.port;
  otaObj["hostname"] = config.deviceConfig.hostname;
  otaObj["passwordSet"] = config.otaConfig.password.length() > 0;
  otaObj["wifiConnected"] = WiFiManager.isConnected();
  otaObj["ip"] = WiFiManager.isConnected() ? WiFi.localIP().toString() : "";
  otaObj["freeSketchSpace"] = ESP.getFreeSketchSpace();
  out["build"] = FW_VERSION;

  JsonObject hbObj = out.createNestedObject("hb");
  if (motor && motor->isHoverUart() && motor->hoverEnabled()) {
    const HoverTelemetry& tel = motor->hoverTelemetry();
    updateChargerConnectedFromTelemetry(tel, millis());
    hbObj["enabled"] = true;
    hbObj["dir"] = tel.dir;
    hbObj["rpm"] = tel.rpm;
    // Normalized distance in mm (0..max). This matches the gate position and doesn't go negative.
    hbObj["dist_mm"] = (long)lroundf(positionMetersRaw * 1000.0f);
    hbObj["dist_mm_raw"] = tel.distMm;
    hbObj["batValid"] = tel.batValid;
    hbObj["rawBat"] = tel.rawBat;
    hbObj["batScale"] = tel.batScale;
    if (tel.batValid) hbObj["batV"] = tel.batV;
    else hbObj["batV"] = nullptr;
    if (tel.iA_x100 >= 0) hbObj["iA"] = ((float)tel.iA_x100) / 100.0f;
    else hbObj["iA"] = -1.0f;
    hbObj["fault"] = tel.fault;
    hbObj["lastTelMs"] = tel.lastTelMs;
    hbObj["cmdAgeMs"] = tel.cmdAgeMs;
    hbObj["telAgeMs"] = (tel.lastTelMs == 0) ? -1 : (long)(millis() - tel.lastTelMs);
    hbObj["chargerConnected"] = chargerStateKnown ? chargerConnected : false;
    hbObj["chargerKnown"] = chargerStateKnown;
    hbObj["chargerPending"] = chargerPending;
  } else {
    hbObj["enabled"] = false;
    hbObj["dir"] = 0;
    hbObj["rpm"] = 0;
    hbObj["dist_mm"] = 0;
    hbObj["batValid"] = false;
    hbObj["rawBat"] = -1;
    hbObj["batScale"] = 0;
    hbObj["batV"] = nullptr;
    hbObj["iA"] = -1.0f;
    hbObj["fault"] = 0;
    hbObj["lastTelMs"] = 0;
    hbObj["cmdAgeMs"] = -1;
    hbObj["telAgeMs"] = -1;
    hbObj["chargerConnected"] = false;
    hbObj["chargerKnown"] = false;
    hbObj["chargerPending"] = false;
  }

  JsonObject ledObj = out.createNestedObject("led");
  led.fillStatus(ledObj);

  JsonObject limitsObj = out.createNestedObject("limits");
  limitsObj["enabled"] = config.limitsConfig.enabled;
  limitsObj["openEnabled"] = config.limitsConfig.open.enabled;
  limitsObj["closeEnabled"] = config.limitsConfig.close.enabled;

  JsonObject ioObj = out.createNestedObject("io");
  bool limitOpenActive = inputManager.limitOpenActive(config);
  bool limitCloseActive = inputManager.limitCloseActive(config);
  ioObj["limitOpenRaw"] = inputManager.limitOpenRaw();
  ioObj["limitCloseRaw"] = inputManager.limitCloseRaw();
  ioObj["limitOpen"] = limitOpenActive;
  ioObj["limitClose"] = limitCloseActive;
  ioObj["stop"] = inputManager.stopActive();
  ioObj["obstacle"] = inputManager.obstacleActive();
  ioObj["button"] = inputManager.buttonActive();

  JsonObject inputsObj = out.createNestedObject("inputs");
  inputsObj["limitOpen"] = limitOpenActive;
  inputsObj["limitClose"] = limitCloseActive;
  inputsObj["limitOpenRaw"] = inputManager.limitOpenRaw();
  inputsObj["limitCloseRaw"] = inputManager.limitCloseRaw();
  inputsObj["photocellBlocked"] = config.sensorsConfig.photocell.enabled && inputManager.obstacleActive();
  inputsObj["photocellRaw"] = inputManager.obstacleActive();
  inputsObj["photocellEnabled"] = config.sensorsConfig.photocell.enabled;
  inputsObj["limitsEnabled"] = config.limitsConfig.enabled;
  inputsObj["limitOpenEnabled"] = config.limitsConfig.open.enabled;
  inputsObj["limitCloseEnabled"] = config.limitsConfig.close.enabled;

  JsonObject fsObj = out.createNestedObject("fs");
  fsObj["totalBytes"] = fsTotalBytesCached;
  fsObj["usedBytes"] = fsUsedBytesCached;

  JsonObject homingObj = out.createNestedObject("homing");
  homingObj["active"] = homingActive;
  homingObj["reason"] = homingReasonStr((HomingReason)homingReasonEnum);
  homingObj["result"] = homingResultStr((HomingResult)homingResultEnum);
  homingObj["lastChangeMs"] = homingLastChangeMs;
  homingObj["enabled"] = startupHomingEnabled;
  homingObj["positionCertain"] = startupPositionCertain;
  homingObj["safetyLocked"] = startupSafetyLocked;
  homingObj["safetyReason"] = startupSafetyReason;

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

void fillStatusLite(JsonObject& out) {
  syncLegacyPositionState();
  if (gate) {
    const GateStatus& st = gate->getStatus();
    out["state"] = effectiveGateStateString();
    out["moving"] = effectiveGateMoving();
    if (startupUiUnknownPos10 && !startupPositionCertain) {
      const float maxD = st.maxDistance > 0.0f ? st.maxDistance : maxDistanceMeters;
      float synthetic = kStartupTempDistanceMeters;
      if (maxD > 0.0f && synthetic > maxD) synthetic = maxD;
      out["positionMm"] = (long)lroundf(synthetic * 1000.0f);
      out["positionPercent"] = (maxD > 0.0f) ? (int)((synthetic * 100.0f) / maxD + 0.5f) : -1;
    } else {
      out["positionMm"] = (long)lroundf(st.position * 1000.0f);
      out["positionPercent"] = st.positionPercent;
    }
    out["errorCode"] = static_cast<int>(st.error);
    out["limitOpen"] = inputManager.limitOpenActive(config);
    out["limitClose"] = inputManager.limitCloseActive(config);
  } else {
    out["state"] = "unknown";
    out["moving"] = false;
    out["positionMm"] = (long)lroundf(positionMeters * 1000.0f);
    out["positionPercent"] = positionPercent;
    out["errorCode"] = 0;
    out["limitOpen"] = false;
    out["limitClose"] = false;
  }

  if (motor && motor->isHoverUart() && motor->hoverEnabled()) {
    const HoverTelemetry& tel = motor->hoverTelemetry();
    updateChargerConnectedFromTelemetry(tel, millis());
    out["rpm"] = tel.rpm;
    out["iA"] = tel.iA_x100 >= 0 ? ((float)tel.iA_x100) / 100.0f : -1.0f;
    out["chargerConnected"] = chargerStateKnown ? chargerConnected : false;
    out["chargerKnown"] = chargerStateKnown;
    out["chargerPending"] = chargerPending;
  } else {
    out["rpm"] = 0;
    out["iA"] = -1.0f;
    out["chargerConnected"] = false;
    out["chargerKnown"] = false;
    out["chargerPending"] = false;
  }

  const bool wifiConnected = WiFiManager.isConnected();
  out["wifiConnected"] = wifiConnected;
  out["wifiRssi"] = wifiConnected ? WiFi.RSSI() : 0;
  out["wifiMode"] = WiFiManager.getModeCString();
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

ControlResult handleControlCmd(const char* action) {
  if (!gate) return makeControlResult(false, false, 503, "error", "gate_not_ready");
  if (!action || action[0] == '\0') return makeControlResult(false, false, 422, "invalid", "missing_action");

  // FIX #4: Allow "stop" even when position is not yet referenced (safety measure).
  // Movement commands are still blocked until homing establishes position.
  const bool isStopCmd = (strcmp(action, "stop") == 0);
  if (startupHomingEnabled && !startupPositionCertain && !isStopCmd) {
    pushEvent("warn", "control blocked: homing pending — stop still allowed");
#if defined(GATE_DEBUG_UI)
    Serial.printf("[UI] control blocked: startup_safety reason=%s action=%s (stop always permitted)\n",
                  startupSafetyReason, action);
#endif
    return makeControlResult(false, false, 409, "blocked", "position_not_referenced");
  }
  if (startupSafetyLocked && !isStopCmd) {
    pushEvent("warn", "control blocked: startup safety lock");
#if defined(GATE_DEBUG_UI)
    Serial.printf("[UI] control blocked: startup safety lock (reason=%s)\n", startupSafetyReason);
#endif
    return makeControlResult(false, false, 423, "blocked", "startup_safety_lock");
  }
  if (homingActive) {
    pushEvent("warn", "control blocked by homing");
#if defined(GATE_DEBUG_UI)
    Serial.println("[UI] control blocked: homing active");
#endif
    return makeControlResult(false, false, 409, "blocked", "homing_active");
  }
  if (otaActive) {
    pushEvent("warn", "control blocked by ota");
#if defined(GATE_DEBUG_UI)
    Serial.println("[UI] control blocked: OTA active");
#endif
    return makeControlResult(false, false, 423, "blocked", "ota_active");
  }
#if defined(GATE_DEBUG_UI)
  Serial.printf("[UI] control action=%s\n", action);
#endif
  if (strcmp(action, "zero") == 0) {
    if (handleGateCalibrate("zero")) {
      pushEvent("info", "command zero");
      return makeControlResult(true, true, 200, "ok");
    } else {
      pushEvent("warn", "command zero failed");
      return makeControlResult(false, false, 409, "blocked", "zero_calibration_failed");
    }
  }
  GateCommandResponse r = gate->handleCommand(action);
  if (r.result == GATE_CMD_UNKNOWN) {
    pushEvent("warn", "command unknown");
#if defined(GATE_DEBUG_UI)
    Serial.printf("[UI] control result=unknown\n");
#endif
    return makeControlResult(false, false, 422, "invalid", "unknown_command");
  }

  if (r.result == GATE_CMD_BLOCKED) {
    if (r.cmd == GATE_CMD_OPEN) pushEvent("warn", "command open blocked");
    else if (r.cmd == GATE_CMD_CLOSE) pushEvent("warn", "command close blocked");
    else if (r.cmd == GATE_CMD_TOGGLE) pushEvent("warn", "command toggle blocked");
    else pushEvent("warn", "command blocked");
    led.setOverride("command_rejected", 600);
#if defined(GATE_DEBUG_UI)
    Serial.printf("[UI] control result=blocked cmd=%d\n", (int)r.cmd);
#endif
    return makeControlResult(false, false, 409, "blocked", "command_blocked");
  }

  // OK
  if (r.cmd == GATE_CMD_OPEN) {
    pushEvent("info", "command open");
#if defined(GATE_DEBUG_UI)
    Serial.printf("[UI] control result=open applied=%d\n", r.applied ? 1 : 0);
#endif
  } else if (r.cmd == GATE_CMD_CLOSE) {
    pushEvent("info", "command close");
#if defined(GATE_DEBUG_UI)
    Serial.printf("[UI] control result=close applied=%d\n", r.applied ? 1 : 0);
#endif
  } else if (r.cmd == GATE_CMD_STOP) {
    pushEvent("info", "command stop");
#if defined(GATE_DEBUG_UI)
    Serial.printf("[UI] control result=stop applied=%d\n", r.applied ? 1 : 0);
#endif
  } else if (r.cmd == GATE_CMD_TOGGLE) {
    pushEvent("info", r.applied ? "command toggle" : "command toggle (no-op)");
#if defined(GATE_DEBUG_UI)
    Serial.printf("[UI] control result=toggle applied=%d\n", r.applied ? 1 : 0);
#endif
  } else {
    pushEvent("info", "command ok");
#if defined(GATE_DEBUG_UI)
    Serial.printf("[UI] control result=ok cmd=%d applied=%d\n", (int)r.cmd, r.applied ? 1 : 0);
#endif
  }
  return makeControlResult(true, r.applied, 200, "ok");
}

static void processPendingRuntimeConfigApply(uint32_t nowMs) {
  if (!runtimeConfigApplyPending) return;
  if (otaActive || homingActive) return;
    if (gate && gate->getState() != GATE_STOPPED) return;

  runtimeConfigApplyPending = false;
  const uint32_t reqAgeMs =
    (runtimeConfigApplyRequestedMs != 0 && nowMs >= runtimeConfigApplyRequestedMs)
      ? (nowMs - runtimeConfigApplyRequestedMs)
      : 0;
#if defined(GATE_DEBUG_CONFIG)
  Serial.printf("[CFG_APPLY] start reqAgeMs=%lu\n",
                (unsigned long)reqAgeMs);
#endif

  if (gateTaskHandle != NULL) {
    // FIX A1: instead of vTaskSuspend (which starves the WDT), ask GateTask
    // to park itself in a WDT-safe spin and wait for its acknowledgement.
    configApplyPausing = true;
    portMEMORY_BARRIER();
    const uint32_t pauseWaitStart = millis();
    while (!configApplyPaused && (millis() - pauseWaitStart < 120)) {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (!configApplyPaused) {
      Serial.println("[CFG_APPLY] WARNING: GateTask pause ack timeout – proceeding anyway");
    }
  }

  config.load();
#if defined(GATE_DEBUG_CONFIG)
  Serial.printf("[CFG_APPLY] after load: gate.maxDistance=%.3f gate.position=%.3f\n",
                config.gateConfig.maxDistance, config.gateConfig.position);
#endif
  setupInputs();
  updateHallAttachment();

  mqtt.applyConfig(config.mqttConfig);
  led.applyConfig(config.ledConfig);
  led.setMqttEnabled(config.mqttConfig.enabled);

  if (motor) {
    motor->applyConfig(config.motorConfig, config.gpioConfig, config.hoverUartConfig);
    motor->setInvertDir(config.motorConfig.invertDir);
    motor->setMotionProfile(config.motionProfile());
  }

  if (gateTaskHandle != NULL) {
    // FIX A1: clear the pause flag so GateTask exits its spin loop.
    configApplyPausing = false;
    portMEMORY_BARRIER();
  }

#if defined(GATE_DEBUG_CONFIG)
  Serial.printf("[CFG_APPLY] done rev=%lu heap=%u\n",
                (unsigned long)config.getRevision(),
                (unsigned)ESP.getFreeHeap());
#endif
}

ControlResult handleControlWrapper(const String& action) {
  // === v2.1 FIX B: "stop" routed via xTaskNotify — no volatile flag, no direct gate->stop().
  // GateTask processes GATE_NOTIFY_STOP within ≤10 ms (one cycle).
  if (action == "stop") {
    const bool wasMoving = gate && gate->isMoving();  // bool read: atomic on Xtensa
    if (gateTaskHandle) xTaskNotify(gateTaskHandle, GATE_NOTIFY_STOP, eSetBits);
    return makeControlResult(true, wasMoving, 200, "ok");
  }
  // Pre-validate startup safety before enqueuing (so we can return an
  // accurate HTTP status to the caller right now).
  if (!gate) return makeControlResult(false, false, 503, "error", "gate_not_ready");
  const bool isStopCmd = false;  // already handled above
  if (startupHomingEnabled && !startupPositionCertain && !isStopCmd) {
    pushEvent("warn", "control blocked: homing pending");
    return makeControlResult(false, false, 409, "blocked", "position_not_referenced");
  }
  if (startupSafetyLocked) {
    pushEvent("warn", "control blocked: startup safety lock");
    return makeControlResult(false, false, 423, "blocked", "startup_safety_lock");
  }
  if (homingActive) {
    pushEvent("warn", "control blocked by homing");
    return makeControlResult(false, false, 409, "blocked", "homing_active");
  }
  if (otaActive) {
    pushEvent("warn", "control blocked by ota");
    return makeControlResult(false, false, 423, "blocked", "ota_active");
  }
  // Enqueue movement command for GateTask
  if (sendGateCommand(action.c_str())) {
#if defined(GATE_DEBUG_UI)
    Serial.printf("[UI] control queued action=%s\n", action.c_str());
#endif
    return makeControlResult(true, true, 200, "ok");
  }
#if defined(GATE_DEBUG_UI)
  Serial.printf("[UI] control rejected by ingress guard action=%s\n", action.c_str());
#endif
  return makeControlResult(false, false, 429, "busy", "command_rate_limited");
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
      int value = doc["brightness"] | led.getBrightness();
      if (value < 0) value = 0;
      if (value > 100) value = 100;
      led.setBrightness(value);
    }
    if (doc.containsKey("ringStartIndex") || doc.containsKey("ringReverse")) {
      int startIndex = doc["ringStartIndex"] | led.getRingStartIndex();
      bool reverse = doc["ringReverse"] | led.getRingReverse();
      if (startIndex < -4096) startIndex = -4096;
      if (startIndex > 4096) startIndex = 4096;
      led.setRingOrientation(startIndex, reverse);
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
  unsigned long debounceMs = (unsigned long)config.remoteConfig.antiRepeatMs;
  if (debounceMs < 50) debounceMs = 50;
  if (debounceMs > 2000) debounceMs = 2000;

  // Auto-learn only in explicit learn mode window.
    if (!known) {
      if (!learnMode || (learnModeUntilMs != 0 && (int32_t)(now - learnModeUntilMs) > 0)) {
        pushEventf("warn", "unknown remote rejected %lu", serial);
        led.setOverride("remote_reject", 700);
        return;
      }
      String name = String("Remote ") + String(serial);
      if (config.addRemote(serial, name)) {
        pushEventf("info", "learned remote %lu", serial);
        led.setOverride("remote_ok", 700);
        config.getRemote(serial, entry);
        known = true;
        authorized = entry.enabled;
      } else {
        pushEvent("warn", "remote auto-learn failed");
        led.setOverride("remote_reject", 700);
        return;
      }
    }

    if (!authorized) {
      pushEvent("warn", "remote not authorized");
      led.setOverride("remote_reject", 700);
      return;
    }

  // Debounce: accept toggle or green (fallback) to avoid "dead" buttons.
  bool actionToggle = btnToggle || btnGreen;
  if (!actionToggle) return;
  if (now - seen.lastActionMs < debounceMs) return;
  if (seen.encript == encript) return;
  seen.lastActionMs = now;
  seen.encript = encript;

  // FIX #2: enqueue toggle command for GateTask
  if (sendGateCommand("toggle")) {
    pushEvent("info", "remote: toggle");
  } else {
    pushEvent("warn", "remote: toggle throttled");
  }
}

void learnCallback(bool enable) {
  learnMode = enable;
  learnModeUntilMs = enable ? (millis() + kLearnModeWindowMs) : 0;
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
  Serial.printf("[OTA] init enabled=1 host=%s port=%d password=%s\n",
                config.deviceConfig.hostname.c_str(),
                config.otaConfig.port,
                config.otaConfig.password.length() > 0 ? "set" : "empty");
  ArduinoOTA.setHostname(config.deviceConfig.hostname.c_str());
  ArduinoOTA.setPort(config.otaConfig.port);
  if (config.otaConfig.password.length() > 0) {
    ArduinoOTA.setPassword(config.otaConfig.password.c_str());
  }
  ArduinoOTA.onEnd([]() {
    otaActive = false;
    otaProgress = 100;
    otaError[0] = '\0';
    pushEvent("info", "ota end");
    led.setOtaActive(false);
    if (gate) gate->setOtaActive(false);
    scheduleRestart(1200, "arduino_ota_end");
  });
  ArduinoOTA.onStart([]() {
    otaActive = true;
    otaProgress = 0;
    otaError[0] = '\0';
    pushEvent("info", "ota start");
    // === v2.1 FIX A: Emergency stop via xTaskNotify + ack semaphore handshake.
    // GateTask processes GATE_NOTIFY_EMERGENCY within ≤10 ms (one cycle), then
    // gives emergencyStopAckSem. OTA waits up to 50 ms for the ack.
    // If ack times out, GateTask was likely blocked (log error, continue OTA).
    if (gateTaskHandle && emergencyStopAckSem) {
      xTaskNotify(gateTaskHandle, GATE_NOTIFY_EMERGENCY, eSetBits);
      if (xSemaphoreTake(emergencyStopAckSem, pdMS_TO_TICKS(50)) != pdTRUE) {
        Serial.println("[OTA] WARNING: emergency stop ack timeout — GateTask may be blocked");
      }
    }
    led.setOtaActive(true);
    if (gate) gate->setOtaActive(true);
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    if (total > 0) {
      int pct = (int)((progress * 100U) / total);
      otaProgress = pct;
      led.setOtaProgress(pct);
    }
  });
  ArduinoOTA.onError([](ota_error_t error) {
    otaActive = false;
    otaProgress = -1;
    snprintf(otaError, sizeof(otaError), "err_%d", (int)error);
    pushEvent("error", "ota error");
    led.setOtaActive(false);
    if (gate) gate->setOtaActive(false);
  });
  ArduinoOTA.begin();
  otaReady = true;
}

// === FIX #2: Send a gate action to the command queue (fire-and-forget). ===
// Safe to call from any task/context. Returns false if queue is full.
static bool sendGateCommand(const char* action) {
  if (!gateCommandQueue || !action) return false;
  GateCmdItem item;
  strncpy(item.action, action, sizeof(item.action) - 1);
  item.action[sizeof(item.action) - 1] = '\0';
  if (strcmp(item.action, "stop") == 0) {
    if (gateTaskHandle) xTaskNotify(gateTaskHandle, GATE_NOTIFY_STOP, eSetBits);
    gateCommandIngress.stopNotifications++;
    return true;
  }

  const uint32_t nowMs = millis();
  // FIX C1: take the ingress spinlock before reading/writing shared stats.
  portENTER_CRITICAL_SAFE(&gateCommandIngressMux);
  const uint32_t sinceLast =
      (gateCommandIngress.lastAcceptedMs == 0 || nowMs < gateCommandIngress.lastAcceptedMs)
        ? UINT32_MAX
        : (nowMs - gateCommandIngress.lastAcceptedMs);
  const bool sameAsLast = strncmp(gateCommandIngress.lastAction,
                                  item.action,
                                  sizeof(gateCommandIngress.lastAction)) == 0;
  if (sameAsLast && sinceLast < kGateCommandDuplicateWindowMs) {
    gateCommandIngress.duplicates++;
    portEXIT_CRITICAL_SAFE(&gateCommandIngressMux);
    return false;
  }
  if (sinceLast < kGateCommandMinIntervalMs) {
    gateCommandIngress.rateLimited++;
    portEXIT_CRITICAL_SAFE(&gateCommandIngressMux);
    return false;
  }
  if (uxQueueSpacesAvailable(gateCommandQueue) == 0) {
    gateCommandIngress.queueFull++;
    portEXIT_CRITICAL_SAFE(&gateCommandIngressMux);
    return false;
  }
  portEXIT_CRITICAL_SAFE(&gateCommandIngressMux);
  if (xQueueSend(gateCommandQueue, &item, 0) != pdTRUE) {
    portENTER_CRITICAL_SAFE(&gateCommandIngressMux);
    gateCommandIngress.queueFull++;
    portEXIT_CRITICAL_SAFE(&gateCommandIngressMux);
    return false;
  }
  portENTER_CRITICAL_SAFE(&gateCommandIngressMux);
  gateCommandIngress.accepted++;
  gateCommandIngress.lastAcceptedMs = nowMs;
  strncpy(gateCommandIngress.lastAction, item.action, sizeof(gateCommandIngress.lastAction) - 1);
  gateCommandIngress.lastAction[sizeof(gateCommandIngress.lastAction) - 1] = '\0';
  portEXIT_CRITICAL_SAFE(&gateCommandIngressMux);
  return true;
}

// === FIX #3: Background task for deferred config saves. ===
// Triggered by a binary semaphore; saves at most once per 500 ms.
static void configSaveTask(void* pvParameters) {
  const TickType_t kMinIntervalTicks = pdMS_TO_TICKS(500);
  TickType_t lastSaveTick = xTaskGetTickCount() - kMinIntervalTicks;
  while (1) {
    xSemaphoreTake(configSaveSem, portMAX_DELAY);  // wait for signal
    const TickType_t nowTick = xTaskGetTickCount();
    const TickType_t elapsed = nowTick - lastSaveTick;
    if (elapsed < kMinIntervalTicks) {
      vTaskDelay(kMinIntervalTicks - elapsed);
    }
    config.processDeferredSave();
    lastSaveTick = xTaskGetTickCount();
  }
}

void gateTask(void* pvParameters) {
  const bool wdtEnabled = config.safetyConfig.watchdogEnabled;
  if (wdtEnabled) {
    esp_task_wdt_add(NULL);
  }
  while (1) {
    gateTaskHeartbeatMs = millis();
    const uint32_t nowMs = (uint32_t)gateTaskHeartbeatMs;

    // FIX A1: cooperative pause for processPendingRuntimeConfigApply().
    // When main loop sets configApplyPausing, we park here resetting the WDT
    // so the watchdog never fires during a config reload.  This replaces the
    // previous vTaskSuspend(gateTaskHandle) which starved the WDT.
    if (configApplyPausing) {
      configApplyPaused = true;
      portMEMORY_BARRIER();
      while (configApplyPausing) {
        if (wdtEnabled) esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      configApplyPaused = false;
      portMEMORY_BARRIER();
      gateTaskHeartbeatMs = millis(); // refresh heartbeat after pause
      continue;                       // restart the loop (re-drain notifications)
    }

    // === v2.1: Drain all pending task notifications (non-blocking, bits OR-accumulated). ===
    // xTaskNotify(gateTaskHandle, bit, eSetBits) from any context; processed here in
    // priority order before any gate/motor mutation this cycle.
    uint32_t notifyVal = 0;
    xTaskNotifyWait(0, UINT32_MAX, &notifyVal, 0);

    // FIX A: Emergency stop from OTA callback — with ack handshake.
    // OTA sets GATE_NOTIFY_EMERGENCY and waits (≤50 ms) on emergencyStopAckSem.
    // ack is given AFTER gate->stop() + motor->stopHard() + hoverDisarm() complete.
    if (notifyVal & GATE_NOTIFY_EMERGENCY) {
      if (gate) gate->stop(GATE_STOP_USER);
      if (motor) {
        motor->stopHard();
        if (motor->isHoverUart()) motor->hoverDisarm();
      }
      if (emergencyStopAckSem) xSemaphoreGive(emergencyStopAckSem);  // ack after full stop
    }

    // FIX B: Stop from web/MQTT / stop-button — no ack needed, ≤10 ms latency.
    // Also handles gate->onStopInput() routed from handleInputs().
    if (notifyVal & GATE_NOTIFY_STOP) {
      if (gate) gate->stop(GATE_STOP_USER);
    }

    // Safety: both limits active simultaneously → gate error + stop.
    // Safety override.
    if (notifyVal & GATE_NOTIFY_LIMITS_INVALID) {
      if (gate) gate->onLimitsInvalid();
    }

    // Safety: obstacle sensor rising/falling edge — gate->onObstacle() mutates state machine.
    // Safety override.
    // obstacleNotifyActive written by main loop before xTaskNotify; volatile guarantees visibility.
    if (notifyVal & GATE_NOTIFY_OBSTACLE) {
      if (gate) gate->onObstacle(obstacleNotifyActive);
    }

    // Limit rising-edge events routed from handleInputs() via xTaskNotify.
    // onLimitOpen/Close mutate position + state machine — runs in GateTask only.
    if (notifyVal & GATE_NOTIFY_LIMIT_OPEN) {
      if (gate) gate->onLimitOpen();
      positionTracker.requestResyncOpen();
    }
    if (notifyVal & GATE_NOTIFY_LIMIT_CLOSE) {
      if (gate) gate->onLimitClose();
      positionTracker.requestResyncClose();
    }

    // Homing runs exclusively in GateTask (sole motor/gate owner).
    if (startupHomingEnabled && (!startupPositionCertain || homingActive)) {
      runGateTaskHoming(nowMs);
    }

    // Drain command queue (skipped during active homing to prevent commands
    // from interfering with the homing motor drive).
    if (!homingActive && gateCommandQueue && gate) {
      GateCmdItem item;
      uint8_t drained = 0;
      while (drained < kGateCommandDrainPerCycle &&
             xQueueReceive(gateCommandQueue, &item, 0) == pdTRUE) {
        handleControlCmd(item.action);
        drained++;
      }
    }

    // gate->loop(): level-based stop, over-current, stall, status publish.
    if (gate) gate->loop();

    if (wdtEnabled) {
      esp_task_wdt_reset();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  Serial.begin(115200);
  delay(10);
  bootResetReason = esp_reset_reason();
  prevRtcResetReason = rtcLastResetReason;
  strncpy(bootRestartReason, rtcLastRestartReason, sizeof(bootRestartReason) - 1);
  bootRestartReason[sizeof(bootRestartReason) - 1] = '\0';
  if (bootResetReason != ESP_RST_SW) {
    bootRestartReason[0] = '\0';
  }

  rtcBootCount++;
  bootCount = rtcBootCount;
  rtcLastResetReason = (uint32_t)bootResetReason;
  Serial.printf("[BOOT] boot=%lu reset_reason=%s (%d) scheduled_restart=%s\n",
                (unsigned long)bootCount,
                resetReasonToString(bootResetReason),
                (int)bootResetReason,
                bootRestartReason[0] ? bootRestartReason : "");
  startupBootMs = millis();
  mainLoopHeartbeatMs = startupBootMs;
  gateTaskHeartbeatMs = startupBootMs;
  startupHomingEnabled = true;
  if (!startupHomingEnabled) {
    setStartupSafetyState(true, false, "non_cold_boot");
    setHomingResult(HOMING_RESULT_SKIP, HOMING_REASON_NON_COLD_BOOT);
    homingChecked = true;
  } else {
    setStartupSafetyState(false, false, "cold_boot_pending");
    setHomingResult(HOMING_RESULT_PENDING, HOMING_REASON_COLD_BOOT_PENDING);
    homingChecked = false;
  }

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
  config.setSaveAllowedCallback(isSafeToSaveConfig);
  led.init(config.ledConfig);
  led.setOverride("boot", 1200);
  led.setMqttEnabled(config.mqttConfig.enabled);

  motor = new MotorController();
  motor->applyConfig(config.motorConfig, config.gpioConfig, config.hoverUartConfig);
  // Direction source of truth: motor.invertDir only.
  // gpio.dirInvert is kept in config for backward compatibility but is not applied to runtime inversion.
  motor->setInvertDir(config.motorConfig.invertDir);
  Serial.printf("[DIR] runtime invert source=motor.invertDir motor=%d gpio.dirInvert(legacy_ignored)=%d applied=%d\n",
                config.motorConfig.invertDir ? 1 : 0,
                config.gpioConfig.dirInvert ? 1 : 0,
                config.motorConfig.invertDir ? 1 : 0);
  motor->setMotionProfile(config.motionProfile());
  motor->begin();

  gate = new GateController(motor, &config);
  gate->begin();
  gate->setStatusCallback(onGateStatusChanged, nullptr);
  eventQueue = xQueueCreate(64, sizeof(EventEntry));
  // FIX #2: gate command queue (32 slots × 32-byte action string)
  gateCommandQueue = xQueueCreate(32, sizeof(GateCmdItem));
  // FIX #3: config save semaphore + background task
  configSaveSem = xSemaphoreCreateBinary();
  xTaskCreate(configSaveTask, "ConfigSave", 4096, nullptr, 1, &configSaveTaskHandle);
  // v2.1 FIX A: emergency stop ack semaphore (OTA callback waits for GateTask ack)
  emergencyStopAckSem = xSemaphoreCreateBinary();
  positionTracker.begin(&config, motor, gate);
  positionTracker.initializeFromConfig();
  syncLegacyPositionState();
  xTaskCreatePinnedToCore(gateTask, "GateTask", 6144, NULL, 2, &gateTaskHandle, 0);

  setupInputs();
  updateHallAttachment();
  WiFiManager.begin(&config);
  Serial.printf("[OTA] cfg enabled=%d host=%s port=%d password=%s\n",
                config.otaConfig.enabled ? 1 : 0,
                config.deviceConfig.hostname.c_str(),
                config.otaConfig.port,
                config.otaConfig.password.length() > 0 ? "set" : "empty");

  if (config.gpioConfig.hcsPin >= 0) {
    hcs = new HCS301Receiver(config.gpioConfig.hcsPin);
    hcs->begin();
    hcs->setCallback(onHcsReceived);
  } else {
    hcs = nullptr;
    Serial.println("[HCS] disabled (pin < 0)");
  }

  mqtt.setCallback(mqttCallback);
  mqtt.begin(config.mqttConfig);

  webserver.setStatusCallback(fillStatus);
  webserver.setStatusLiteCallback(fillStatusLite);
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
  webserver.setOtaActiveCallback(isOtaActive);
  webserver.begin();

  pushEvent("info", "setup done");
}

void loop() {
  mainLoopHeartbeatMs = millis();
  WiFiManager.loop();
  if (hcs) hcs->loop();

  if (learnMode && learnModeUntilMs != 0 && (int32_t)(millis() - learnModeUntilMs) >= 0) {
    learnCallback(false);
  }

  handleInputs();
  syncLegacyPositionState();
  logSummary1Hz();
  updateHallAttachment();
  updatePositionPercent();
  unsigned long now = millis();
  updateHallStats(now);
  // v2: runStartupHoming() removed from main loop — homing now runs in GateTask
  // (runGateTaskHoming) as the sole owner of MotorController and GateController.
  updateFsStats(now);
  processPendingRuntimeConfigApply(now);

  mqtt.setConnectAllowed(!homingActive && (!gate || !gate->isMoving()));
  mqtt.loop();
  bool nowConnected = mqtt.connected();
  bool wifiConnected = WiFiManager.isConnected();
  bool apMode = strcmp(WiFiManager.getModeCString(), "AP") == 0;
  if (gate) gate->setConnectivity(wifiConnected, nowConnected, apMode);
  led.setMqttEnabled(mqtt.enabled());
  led.setHomingActive(homingActive);
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
    // v2.2: Split MQTT publish burst to reduce LwIP tcpip_task contention.
    // Motion-critical topics (position, state) at 1s — automation needs these.
    // Hover telemetry at 5s — hover not always connected, large JSON, low urgency.
    if (now - lastMqttTelemetryMs > 1000) {
      lastMqttTelemetryMs = now;
      mqttPublishPosition();
      mqttPublishMotionState();
      mqttPublishMotionPosition();
    }
    if (now - lastMqttHoverTelMs > 5000) {
      lastMqttHoverTelMs = now;
      mqttPublishTelemetry();
    }
  }

  if (config.otaConfig.enabled && WiFiManager.isConnected()) {
    if (!otaNetDiagLogged) {
      Serial.printf("[OTA] net ip=%s host=%s port=%d ready=%d\n",
                    WiFi.localIP().toString().c_str(),
                    config.deviceConfig.hostname.c_str(),
                    config.otaConfig.port,
                    otaReady ? 1 : 0);
      otaNetDiagLogged = true;
    }
    setupOta();
    if (otaReady) ArduinoOTA.handle();
  } else if (!config.otaConfig.enabled) {
    otaNetDiagLogged = false;
  }

  // Push status frequently while moving (smooth UI), slower when idle.
  uint32_t statusIntervalMs = 1000;
  if (gate && gate->isMoving()) statusIntervalMs = 500;
  if (now - lastStatusMs > statusIntervalMs) {
    lastStatusMs = now;
    webserver.broadcastStatus();
  }
  static uint32_t lastWebMaintenanceMs = 0;
  if (now - lastWebMaintenanceMs >= 250) {
    lastWebMaintenanceMs = now;
    webserver.maintenance();
  }
  drainEventQueue();
  static bool saveWasPending = false;
  bool savePending = config.hasPendingSave();
  if (savePending && !saveWasPending) {
    pushEvent("warn", isSafeToSaveConfig() ? "config save deferred" : "config save deferred (moving)");
  }
  if (!savePending && saveWasPending) {
    pushEvent("info", "config save completed");
  }
  saveWasPending = savePending;
  // FIX #3: Signal background ConfigSaveTask — no more blocking LittleFS I/O
  // in main loop. configSaveTask rate-limits to ≤ once per 500 ms.
  if (savePending && configSaveSem) {
    xSemaphoreGive(configSaveSem);
  }
  if (factoryResetPending && (int32_t)(now - factoryResetAtMs) >= 0) {
    factoryResetPending = false;
    config.resetToDefaults();
    config.save(nullptr);
  }
  if (restartPending && (int32_t)(now - restartAtMs) >= 0) {
    restartPending = false;
    ESP.restart();
  }
  vTaskDelay(pdMS_TO_TICKS(1));
}


