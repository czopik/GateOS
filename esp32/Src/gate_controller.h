#pragma once
#include <Arduino.h>
#include <math.h>
#include "motor_controller.h"
#include "config_manager.h"
#include "gate_logic_rules.h"

enum GateState {
  GATE_STOPPED,
  GATE_OPENING,
  GATE_CLOSING,
  GATE_ERROR
};

enum GateErrorCode {
  GATE_ERR_NONE = 0,
  GATE_ERR_TIMEOUT = 1,
  GATE_ERR_OBSTACLE = 2,
  GATE_ERR_HOVER_FAULT = 3,
  GATE_ERR_HOVER_TEL_TIMEOUT = 4,
  GATE_ERR_LIMITS_INVALID = 5,
  GATE_ERR_HOVER_OFFLINE = 6,
  GATE_ERR_OVER_CURRENT = 7,
  GATE_ERR_UNKNOWN = 99
};

enum GateStopReason {
  GATE_STOP_NONE = 0,
  GATE_STOP_USER = 1,
  GATE_STOP_SOFT_LIMIT = 2,
  GATE_STOP_TELEMETRY_TIMEOUT = 3,
  GATE_STOP_TELEMETRY_STALL = 4,
  GATE_STOP_HOVER_FAULT = 5,
  GATE_STOP_LIMIT_OPEN = 6,
  GATE_STOP_LIMIT_CLOSE = 7,
  GATE_STOP_OBSTACLE = 8,
  GATE_STOP_ERROR = 9,
  GATE_STOP_OVER_CURRENT = 10
};

struct GateStatus {
  GateState state = GATE_STOPPED;
  GateErrorCode error = GATE_ERR_NONE;
  GateStopReason lastStopReason = GATE_STOP_NONE;
  float position = 0.0f;
  float maxDistance = 0.0f;
  float targetPosition = 0.0f;
  int positionPercent = -1;
  bool obstacle = false;
  bool wifiConnected = false;
  bool mqttConnected = false;
  bool apMode = false;
  bool otaInProgress = false;
  uint32_t lastMoveMs = 0;
  uint32_t lastStateChangeMs = 0;

  static bool approxEqual(float a, float b) {
    return fabsf(a - b) < 0.001f;
  }

  bool equals(const GateStatus& other) const {
    return state == other.state &&
           error == other.error &&
           lastStopReason == other.lastStopReason &&
           approxEqual(position, other.position) &&
           approxEqual(maxDistance, other.maxDistance) &&
           approxEqual(targetPosition, other.targetPosition) &&
           positionPercent == other.positionPercent &&
           obstacle == other.obstacle &&
           wifiConnected == other.wifiConnected &&
           mqttConnected == other.mqttConnected &&
           apMode == other.apMode &&
           otaInProgress == other.otaInProgress &&
           lastMoveMs == other.lastMoveMs &&
           lastStateChangeMs == other.lastStateChangeMs;
  }
};

using GateStatusCallback = void(*)(const GateStatus& status, void* ctx);

enum GateCommand {
  GATE_CMD_NONE = 0,
  GATE_CMD_OPEN,
  GATE_CMD_CLOSE,
  GATE_CMD_STOP,
  GATE_CMD_TOGGLE
};

enum GateCommandResult {
  GATE_CMD_OK = 0,
  GATE_CMD_BLOCKED,
  GATE_CMD_UNKNOWN
};

struct GateCommandResponse {
  GateCommand cmd = GATE_CMD_NONE;
  GateCommandResult result = GATE_CMD_OK;
  // True if the command resulted in an actual state change (movement start/stop).
  bool applied = false;
  // Optional follow-up action attempted after the primary state change.
  GateCommand followUpCmd = GATE_CMD_NONE;
  bool followUpBlocked = false;
};

// GateController - gate logic, timeouts, watchdog
class GateController {
public:
  GateController(MotorController* motor, ConfigManager* cfg);
  void begin();
  void loop();

  bool open();
  bool close();
  bool moveTo(float targetMeters);
  void stop();
  void stop(GateStopReason reason);
  void setError(GateErrorCode code = GATE_ERR_UNKNOWN);
  void setError(GateErrorCode code, GateStopReason reason);
  bool clearError();
  const char* getStopReasonString(GateStopReason reason) const;

  GateState getState() const { return state; }
  const char* getStateString() const;
  bool isMoving() const { return moving; }
  int getLastDirection() const { return lastDirection; }
  GateErrorCode getErrorCode() const { return errorCode; }
  GateStopReason getLastStopReason() const { return status.lastStopReason; }
  const GateStatus& getStatus() const { return status; }
  int getLastFinalErrorMm() const { return lastFinalErrorMm; }
  float getControlPosition() const { return controlPosition; }
  uint8_t getStopConfirmCount() const { return stopConfirmCount; }
  // Over-current diagnostics
  float getLastOverCurrentA() const { return lastOverCurrentA; }
  uint32_t getLastOverCurrentMs() const { return lastOverCurrentMs; }
  uint32_t getOverCurrentCooldownUntilMs() const { return overCurrentCooldownUntilMs; }
  int getOverCurrentAutoRearmCount() const { return overCurrentAutoRearmCount; }
  bool isHoverRecoveryActive() const { return hoverRecoveryActive; }
  uint32_t getLastHoverRestoreMs() const { return lastHoverRestoreMs; }
  uint32_t getLastHoverLossMs() const { return lastHoverLossMs; }

  float getPosition() const { return status.position; }
  float getMaxDistance() const { return status.maxDistance; }
  float getTargetPosition() const { return status.targetPosition; }

  void setPosition(float position, float maxDistance);
  void setControlPosition(float position);
  void setPositionPercent(int percent);
  void setObstacle(bool active);
  void setConnectivity(bool wifi, bool mqtt, bool apMode);
  void setOtaActive(bool active);
  void setStatusCallback(GateStatusCallback cb, void* ctx);
  void updateLimitState(bool limitOpenActive, bool limitCloseActive);

  // Unified safety/event entry points
  void onLimitOpen();
  void onLimitClose();
  // Photocell logic:
  // - active only while closing
  // - when triggered during closing: hard stop + immediate open
  GateCommandResponse onObstacle(bool active);
  GateCommandResponse handleObstacleTrip(const char* actionOverride = nullptr, bool immediateFollowUp = true);
  void onStopInput();
  void onLimitsInvalid();

  // Centralized command handler for web/mqtt/button.
  GateCommandResponse handleCommand(const char* cmd);

private:
  bool startMoveTo(float target, bool forward, GateState nextState, bool bypassCooldown = false);
  bool startMove(GateMoveDirection dir, float target, GateState nextState, bool bypassCooldown = false);
  void setState(GateState next);
  void publishStatusIfChanged();
  float configuredMaxDistance() const;
  bool canMove(bool bypassCooldown = false) const;
  GateDecisionContext decisionContext() const;
  void setTerminalState(GateTerminalState next);
  void refreshTerminalStateFromPosition();

  MotorController* motor;
  ConfigManager* cfg;

  unsigned long moveStart;
  unsigned long lastProgressMs = 0;
  bool moving;
  bool pendingStop = false;
  GateState state;
  int lastDirection;
  GateErrorCode errorCode = GATE_ERR_NONE;
  float lastProgressPos = 0.0f;
  float controlPosition = 0.0f;
  // Set when the user manually stops the gate during movement.
  // Cleared when a new movement starts or gate reaches an endpoint.
  bool userStoppedDuringMove_ = false;
  uint8_t stopConfirmCount = 0;
  uint32_t lastStopTelMs = 0;
  int lastFinalErrorMm = 0;
  uint32_t overCurrentSinceMs = 0;
  uint32_t overCurrentCooldownUntilMs = 0;
  int overCurrentAutoRearmCount = 0;
  uint32_t lastOverCurrentMs = 0;
  float lastOverCurrentA = 0.0f;
  uint32_t userStopBoostUntilMs = 0;
  bool hoverRecoveryActive = false;
  uint32_t hoverRecoverySinceMs = 0;
  uint32_t lastHoverRestoreMs = 0;
  uint32_t lastHoverLossMs = 0;

  GateStatus status;
  GateStatus lastPublished;
  bool publishedOnce = false;
  GateStatusCallback statusCb = nullptr;
  void* statusCtx = nullptr;

  bool lastObstacle = false;
  uint32_t obstacleRefractoryUntilMs = 0;
  bool limitOpenActive = false;
  bool limitCloseActive = false;
  GateTerminalState terminalState = GateTerminalState::Unknown;
};
