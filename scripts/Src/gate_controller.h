#pragma once
#include <Arduino.h>
#include <math.h>
#include "motor_controller.h"
#include "config_manager.h"

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
  GATE_ERR_UNKNOWN = 99
};

struct GateStatus {
  GateState state = GATE_STOPPED;
  GateErrorCode error = GATE_ERR_NONE;
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
};

// GateController - gate logic, timeouts, watchdog
class GateController {
public:
  GateController(MotorController* motor, ConfigManager* cfg);
  void begin();
  void loop();

  bool open();
  bool close();
  void stop();
  void setError(GateErrorCode code = GATE_ERR_UNKNOWN);

  GateState getState() const { return state; }
  const char* getStateString() const;
  bool isMoving() const { return moving; }
  int getLastDirection() const { return lastDirection; }
  GateErrorCode getErrorCode() const { return errorCode; }
  const GateStatus& getStatus() const { return status; }

  float getPosition() const { return status.position; }
  float getMaxDistance() const { return status.maxDistance; }
  float getTargetPosition() const { return status.targetPosition; }

  void setPosition(float position, float maxDistance);
  void setPositionPercent(int percent);
  void setObstacle(bool active);
  void setConnectivity(bool wifi, bool mqtt, bool apMode);
  void setOtaActive(bool active);
  void setStatusCallback(GateStatusCallback cb, void* ctx);

  // Unified safety/event entry points
  void onLimitOpen();
  void onLimitClose();
  // Obstacle state update; on rising edge will apply obstacleAction from config.
  GateCommandResponse onObstacle(bool active);
  void onStopInput();
  void onLimitsInvalid();

  // Centralized command handler for web/mqtt/button.
  GateCommandResponse handleCommand(const char* cmd);

private:
  void setState(GateState next);
  void publishStatusIfChanged();
  float configuredMaxDistance() const;
  bool canMove() const;

  MotorController* motor;
  ConfigManager* cfg;

  unsigned long moveStart;
  unsigned long lastProgressMs = 0;
  bool moving;
  GateState state;
  int lastDirection;
  GateErrorCode errorCode = GATE_ERR_NONE;
  float lastProgressPos = 0.0f;

  GateStatus status;
  GateStatus lastPublished;
  bool publishedOnce = false;
  GateStatusCallback statusCb = nullptr;
  void* statusCtx = nullptr;

  bool lastObstacle = false;
};
