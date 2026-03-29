#include "gate_controller.h"
#include <esp_task_wdt.h>
#include <string.h>

GateController::GateController(MotorController* motor_, ConfigManager* cfg_) : motor(motor_), cfg(cfg_) {
  moving = false;
  moveStart = 0;
  state = GATE_STOPPED;
  lastDirection = 1;
  status.state = state;
  status.error = errorCode;
}

void GateController::begin() {
  if (cfg->safetyConfig.watchdogEnabled) {
    esp_task_wdt_init(10, true);
  }
  status.lastStateChangeMs = millis();
  if (motor) {
    motor->setMotionProfile(cfg->motionProfile());
  }
  setPosition(cfg->gateConfig.position, cfg->gateConfig.maxDistance);
}

void GateController::loop() {
  if (motor) {
    motor->tick(millis(), status.position);
  }
  if (moving) {
    if (fabsf(status.position - lastProgressPos) >= 0.01f) {
      lastProgressPos = status.position;
      lastProgressMs = millis();
    }
    if (state == GATE_OPENING && status.maxDistance > 0.0f && status.position >= status.maxDistance) {
      stop();
      return;
    }
    if (state == GATE_CLOSING && status.position <= 0.0f) {
      stop();
      return;
    }
    if (cfg->gateConfig.movementTimeout > 0 &&
        lastProgressMs != 0 &&
        millis() - lastProgressMs > (unsigned long)cfg->gateConfig.movementTimeout) {
      setError(GATE_ERR_TIMEOUT);
    }
  }
  if (motor && motor->isHoverUart()) {
    const HoverTelemetry& tel = motor->hoverTelemetry();
    if (tel.fault != 0) {
      // Hoverboard reported a fault -> stop immediately.
      motor->stopHard();
      setError(GATE_ERR_HOVER_FAULT);
      stop();
      return;
    } else if (moving && tel.lastTelMs == 0 && millis() - moveStart > 1500) {
      // We started moving but never received telemetry -> unsafe to continue.
      motor->stopHard();
      setError(GATE_ERR_HOVER_TEL_TIMEOUT);
      stop();
      return;
    } else if (motor->hoverTelemetryTimedOut(millis(), 250)) {
      // Telemetry lost while moving -> stop immediately.
      motor->stopHard();
      setError(GATE_ERR_HOVER_TEL_TIMEOUT);
      stop();
      return;
    }
  }
  motor->update();
  if (moving && motor && !motor->isMotionActive()) {
    moving = false;
    setState(GATE_STOPPED);
    status.targetPosition = status.position;
    publishStatusIfChanged();
  }
  if (cfg->safetyConfig.watchdogEnabled) {
    esp_task_wdt_reset();
  }
}

bool GateController::open() {
  if (!canMove()) {
    setError(GATE_ERR_UNKNOWN);
    return false;
  }
  if (motor && motor->isHoverUart() && !motor->hoverEnabled()) {
    setError(GATE_ERR_HOVER_OFFLINE);
    return false;
  }
  float target = configuredMaxDistance();
  moving = true;
  setState(GATE_OPENING);
  lastDirection = 1;
  moveStart = millis();
  lastProgressMs = moveStart;
  lastProgressPos = status.position;
  status.lastMoveMs = moveStart;
  errorCode = GATE_ERR_NONE;
  status.error = errorCode;
  status.targetPosition = target;
  if (motor) {
    motor->setDirection(true);
    motor->setTarget(true, target);
  }
  publishStatusIfChanged();
  return true;
}

bool GateController::close() {
  if (!canMove()) {
    setError(GATE_ERR_UNKNOWN);
    return false;
  }
  if (motor && motor->isHoverUart() && !motor->hoverEnabled()) {
    setError(GATE_ERR_HOVER_OFFLINE);
    return false;
  }
  moving = true;
  setState(GATE_CLOSING);
  lastDirection = -1;
  moveStart = millis();
  lastProgressMs = moveStart;
  lastProgressPos = status.position;
  status.lastMoveMs = moveStart;
  errorCode = GATE_ERR_NONE;
  status.error = errorCode;
  status.targetPosition = 0.0f;
  if (motor) {
    motor->setDirection(false);
    motor->setTarget(false, 0.0f);
  }
  publishStatusIfChanged();
  return true;
}

void GateController::stop() {
  if (motor) {
    motor->stopSoft();
  }
  moving = false;
  lastProgressMs = 0;
  setState(GATE_STOPPED);
  errorCode = GATE_ERR_NONE;
  status.error = errorCode;
  status.targetPosition = status.position;
  status.lastMoveMs = millis();
  publishStatusIfChanged();
}

void GateController::setError(GateErrorCode code) {
  if (motor) {
    motor->stopHard();
  }
  moving = false;
  setState(GATE_ERROR);
  errorCode = code;
  status.error = errorCode;
  status.targetPosition = status.position;
  status.lastMoveMs = millis();
  publishStatusIfChanged();
}

const char* GateController::getStateString() const {
  switch (state) {
    case GATE_OPENING: return "opening";
    case GATE_CLOSING: return "closing";
    case GATE_ERROR: return "error";
    case GATE_STOPPED:
    default:
      return "stopped";
  }
}

void GateController::setPositionPercent(int percent) {
  int next = percent;
  if (percent < 0) next = -1;
  if (percent > 100) next = 100;
  if (status.positionPercent == next) return;
  status.positionPercent = next;
  if (status.maxDistance > 0.0f && next >= 0) {
    status.position = (status.maxDistance * next) / 100.0f;
  }
  publishStatusIfChanged();
}

void GateController::setPosition(float position, float maxDistance) {
  float nextMax = maxDistance;
  if (nextMax < 0.0f) nextMax = 0.0f;
  float nextPos = position;
  if (nextPos < 0.0f) nextPos = 0.0f;
  if (nextMax > 0.0f && nextPos > nextMax) nextPos = nextMax;

  status.position = nextPos;
  status.maxDistance = nextMax;
  if (nextMax > 0.0f) {
    int pct = (int)((nextPos * 100.0f) / nextMax + 0.5f);
    if (pct > 100) pct = 100;
    if (pct < 0) pct = 0;
    status.positionPercent = pct;
  } else {
    status.positionPercent = -1;
  }

  if (state == GATE_OPENING) {
    status.targetPosition = status.maxDistance;
  } else if (state == GATE_CLOSING) {
    status.targetPosition = 0.0f;
  } else {
    status.targetPosition = status.position;
  }

  if (moving) {
    if (state == GATE_OPENING && status.maxDistance > 0.0f && status.position >= status.maxDistance) {
      stop();
      return;
    }
    if (state == GATE_CLOSING && status.position <= 0.0f) {
      stop();
      return;
    }
  }

  publishStatusIfChanged();
}

void GateController::setObstacle(bool active) {
  if (status.obstacle == active) return;
  status.obstacle = active;
  publishStatusIfChanged();
}

void GateController::setConnectivity(bool wifi, bool mqtt, bool apMode) {
  if (status.wifiConnected == wifi &&
      status.mqttConnected == mqtt &&
      status.apMode == apMode) {
    return;
  }
  status.wifiConnected = wifi;
  status.mqttConnected = mqtt;
  status.apMode = apMode;
  publishStatusIfChanged();
}

void GateController::setOtaActive(bool active) {
  if (status.otaInProgress == active) return;
  status.otaInProgress = active;
  publishStatusIfChanged();
}

void GateController::setStatusCallback(GateStatusCallback cb, void* ctx) {
  statusCb = cb;
  statusCtx = ctx;
  publishStatusIfChanged();
}

void GateController::setState(GateState next) {
  if (state == next) return;
  state = next;
  status.state = next;
  status.lastStateChangeMs = millis();
}

void GateController::publishStatusIfChanged() {
  if (publishedOnce && status.equals(lastPublished)) return;
  lastPublished = status;
  publishedOnce = true;
  if (statusCb) statusCb(status, statusCtx);
}

float GateController::configuredMaxDistance() const {
  float maxDistance = status.maxDistance;
  if (maxDistance > 0.0f) return maxDistance;
  if (!cfg) return 0.0f;
  maxDistance = cfg->gateConfig.maxDistance > 0.0f ? cfg->gateConfig.maxDistance : cfg->gateConfig.totalDistance;
  return maxDistance;
}

bool GateController::canMove() const {
  float maxDistance = configuredMaxDistance();
  if (maxDistance > 0.0f) return true;
  if (cfg && cfg->gateConfig.allowMoveWithoutLimits) return true;
  return false;
}

void GateController::onLimitOpen() {
  // Hard resync position at open limit
  float md = status.maxDistance > 0.0f ? status.maxDistance : configuredMaxDistance();
  if (md > 0.0f) {
    status.position = md;
    status.targetPosition = md;
  }
  if (state == GATE_OPENING || moving) {
    stop();
  } else {
    publishStatusIfChanged();
  }
}

void GateController::onLimitClose() {
  // Hard resync position at close limit
  status.position = 0.0f;
  status.targetPosition = 0.0f;
  if (state == GATE_CLOSING || moving) {
    stop();
  } else {
    publishStatusIfChanged();
  }
}

GateCommandResponse GateController::onObstacle(bool active) {
  GateCommandResponse resp;
  resp.cmd = GATE_CMD_NONE;
  resp.result = GATE_CMD_OK;
  resp.applied = false;

  // Track obstacle state in status.
  setObstacle(active);

  // Only trigger an action on rising edge.
  const bool rising = active && !lastObstacle;
  lastObstacle = active;
  if (!rising) return resp;

  // Apply configured obstacle action.
  const String action = cfg ? cfg->safetyConfig.obstacleAction : String("stop");

  if (action == "reverse") {
    // Reverse current direction if moving; otherwise open.
    if (isMoving()) {
      if (getLastDirection() >= 0) {
        resp = handleCommand("close");
      } else {
        resp = handleCommand("open");
      }
    } else {
      resp = handleCommand("open");
    }
    return resp;
  }

  if (action == "open") {
    resp = handleCommand("open");
    return resp;
  }

  // Default: stop
  resp = handleCommand("stop");
  return resp;
}

void GateController::onStopInput() {
  stop();
}

void GateController::onLimitsInvalid() {
  // Both limits active -> error condition.
  if (getErrorCode() != GATE_ERR_LIMITS_INVALID) {
    setError(GATE_ERR_LIMITS_INVALID);
  }
  stop();
}

GateCommandResponse GateController::handleCommand(const char* cmd) {
  GateCommandResponse resp;
  resp.cmd = GATE_CMD_NONE;
  resp.result = GATE_CMD_OK;
  resp.applied = false;

  if (!cmd) {
    resp.result = GATE_CMD_UNKNOWN;
    return resp;
  }

  // Normalize to lowercase into a small buffer.
  char buf[16];
  size_t n = strlen(cmd);
  if (n >= sizeof(buf)) n = sizeof(buf) - 1;
  for (size_t i = 0; i < n; ++i) {
    char c = cmd[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    buf[i] = c;
  }
  buf[n] = '\0';

  if (strcmp(buf, "open") == 0 || strcmp(buf, "otworz") == 0) {
    resp.cmd = GATE_CMD_OPEN;
    bool ok = open();
    resp.applied = ok;
    resp.result = ok ? GATE_CMD_OK : GATE_CMD_BLOCKED;
    return resp;
  }

  if (strcmp(buf, "close") == 0 || strcmp(buf, "zamknij") == 0) {
    resp.cmd = GATE_CMD_CLOSE;
    bool ok = close();
    resp.applied = ok;
    resp.result = ok ? GATE_CMD_OK : GATE_CMD_BLOCKED;
    return resp;
  }

  if (strcmp(buf, "stop") == 0) {
    resp.cmd = GATE_CMD_STOP;
    bool wasMoving = isMoving();
    stop();
    resp.applied = wasMoving;
    resp.result = GATE_CMD_OK;
    return resp;
  }

  if (strcmp(buf, "toggle") == 0) {
    resp.cmd = GATE_CMD_TOGGLE;
    if (isMoving()) {
      stop();
      resp.applied = true;
      resp.result = GATE_CMD_OK;
      return resp;
    }

    const float pos = getPosition();
    const float maxDist = getMaxDistance();

    // If position is unknown (maxDist==0) or very close to close end -> open.
    if (pos <= 0.01f || maxDist <= 0.0f) {
      bool ok = open();
      resp.applied = ok;
      resp.result = ok ? GATE_CMD_OK : GATE_CMD_BLOCKED;
      return resp;
    }

    // If close to open end -> close.
    if (maxDist > 0.0f && pos >= maxDist - 0.01f) {
      bool ok = close();
      resp.applied = ok;
      resp.result = ok ? GATE_CMD_OK : GATE_CMD_BLOCKED;
      return resp;
    }

    // Otherwise, invert last direction.
    if (getLastDirection() >= 0) {
      bool ok = close();
      resp.applied = ok;
      resp.result = ok ? GATE_CMD_OK : GATE_CMD_BLOCKED;
      return resp;
    }

    bool ok = open();
    resp.applied = ok;
    resp.result = ok ? GATE_CMD_OK : GATE_CMD_BLOCKED;
    return resp;
  }

  resp.result = GATE_CMD_UNKNOWN;
  return resp;
}