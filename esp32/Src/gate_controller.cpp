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
  status.lastStopReason = GATE_STOP_NONE;
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
    motor->tick(millis(), controlPosition);
  }
  if (userStopBoostUntilMs != 0 && millis() >= userStopBoostUntilMs) {
    if (motor && motor->isHoverUart()) {
      motor->setForceDecelBoost(false);
    }
    userStopBoostUntilMs = 0;
  }
  // auto-clear over-current error after cooldown, optionally auto-rearm hoverboard
  if (state == GATE_ERROR && errorCode == GATE_ERR_OVER_CURRENT && overCurrentCooldownUntilMs != 0 && millis() >= overCurrentCooldownUntilMs) {
    overCurrentCooldownUntilMs = 0;
    // clear error and return to STOPPED
    errorCode = GATE_ERR_NONE;
    status.error = errorCode;
    moving = false;
    pendingStop = false;
    setState(GATE_STOPPED);
    // optional auto-rearm (hover UART only)
    if (cfg && cfg->gateConfig.overCurrentAutoRearm) {
      const int maxTries = cfg->gateConfig.overCurrentMaxAutoRearm;
      if (maxTries <= 0 || overCurrentAutoRearmCount < maxTries) {
        if (motor && motor->isHoverUart()) {
          motor->hoverArm();
          overCurrentAutoRearmCount++;
        }
      }
    }
    status.lastMoveMs = millis();
    publishStatusIfChanged();
  }

  if (moving) {
    float progressPos = status.position;
    if (motor && motor->isHoverUart()) {
      const HoverTelemetry& tel = motor->hoverTelemetry();
      if (tel.lastTelMs != 0) {
        progressPos = (float)tel.distMm / 1000.0f;
      } else {
        progressPos = controlPosition;
      }
    }
    if (fabsf(progressPos - lastProgressPos) >= 0.01f) {
      lastProgressPos = progressPos;
      lastProgressMs = millis();
    }
    const bool softLimitsEnabled = cfg ? cfg->gateConfig.softLimitsEnabled : true;
    if (!pendingStop && softLimitsEnabled && state == GATE_OPENING && status.maxDistance > 0.0f && status.position >= status.maxDistance) {
#if defined(GATE_DEBUG_UART)
      Serial.printf("[GATE] soft-limit OPEN reached pos=%.3fm max=%.3fm -> stop()\n", status.position, status.maxDistance);
#endif
      stop(GATE_STOP_SOFT_LIMIT);
    }
    if (!pendingStop && softLimitsEnabled && state == GATE_CLOSING && status.position <= 0.0f) {
#if defined(GATE_DEBUG_UART)
      Serial.printf("[GATE] soft-limit CLOSE reached pos=%.3fm -> stop()\n", status.position);
#endif
      stop(GATE_STOP_SOFT_LIMIT);
    }
    if (!pendingStop &&
        cfg &&
        (cfg->gateConfig.stallTimeoutMs > 0 || cfg->gateConfig.movementTimeout > 0) &&
        lastProgressMs != 0) {
      const unsigned long timeoutMs = (unsigned long)(cfg->gateConfig.stallTimeoutMs > 0 ? cfg->gateConfig.stallTimeoutMs : cfg->gateConfig.movementTimeout);
      const unsigned long graceMs = (unsigned long)(cfg->gateConfig.telemetryGraceMs > 0 ? cfg->gateConfig.telemetryGraceMs : 0);
      if (timeoutMs > 0 && (millis() - moveStart) > graceMs && (millis() - lastProgressMs) > timeoutMs) {
      setError(GATE_ERR_TIMEOUT);
      stop(GATE_STOP_TELEMETRY_STALL);
      return;
      }
    }
  }
  if (motor && motor->isHoverUart()) {
    const uint32_t kHoverTelTimeoutMs = cfg ? cfg->gateConfig.telemetryTimeoutMs : 1200;
    const uint32_t kHoverTelGraceMs   = cfg ? cfg->gateConfig.telemetryGraceMs : 1500;
    const uint32_t kHoverRecoveryStableMs = 1500;
    const HoverTelemetry& tel = motor->hoverTelemetry();

    // Auto-recovery path for COMM-only hover errors (telemetry/power loss).
    // Safety gates:
    // - telemetry present and fresh
    // - fault == 0
    // - rpm ~ 0
    // - no motion auto-resume (we only clear comm error -> STOPPED)
    const bool commError = (state == GATE_ERROR) &&
                           (errorCode == GATE_ERR_HOVER_TEL_TIMEOUT || errorCode == GATE_ERR_HOVER_OFFLINE);
    const bool telFresh = (tel.lastTelMs != 0) && !motor->hoverTelemetryTimedOut(millis(), kHoverTelTimeoutMs);
    const bool telStableForRecovery = telFresh && tel.fault == 0 && abs(tel.rpm) <= 8;
    if (commError) {
      if (telStableForRecovery) {
        if (!hoverRecoveryActive) {
          hoverRecoveryActive = true;
          hoverRecoverySinceMs = millis();
          Serial.printf("[RECOVERY] hover comm candidate: tel restored, waiting stable window %lums\n",
                        (unsigned long)kHoverRecoveryStableMs);
        }
        if (millis() - hoverRecoverySinceMs >= kHoverRecoveryStableMs) {
          moving = false;
          pendingStop = false;
          errorCode = GATE_ERR_NONE;
          status.error = errorCode;
          status.lastStopReason = GATE_STOP_TELEMETRY_TIMEOUT;
          status.targetPosition = status.position;
          status.lastMoveMs = millis();
          setState(GATE_STOPPED);
          lastHoverRestoreMs = millis();
          hoverRecoveryActive = false;
          hoverRecoverySinceMs = 0;
          Serial.printf("[RECOVERY] hover comm restored -> STOPPED (no auto-move)\n");
          publishStatusIfChanged();
        }
      } else {
        if (hoverRecoveryActive) {
          Serial.printf("[RECOVERY] hover comm candidate reset (tel unstable/fault/rpm)\n");
        }
        hoverRecoveryActive = false;
        hoverRecoverySinceMs = 0;
      }
    } else {
      hoverRecoveryActive = false;
      hoverRecoverySinceMs = 0;
    }

    if (moving && cfg) {
      const float limitA = cfg->gateConfig.currentLimitA;
      if (limitA > 0.0f && tel.iA_x100 >= 0) {
        const float currentA = ((float)tel.iA_x100) / 100.0f;
        if (currentA > limitA) {
          if (overCurrentSinceMs == 0) overCurrentSinceMs = millis();
          const uint32_t tripMs = (cfg ? cfg->gateConfig.overCurrentTripMs : 100);
          if (millis() - overCurrentSinceMs > tripMs) {
#if defined(GATE_DEBUG_UART)
            Serial.printf("[GATE] OVER_CURRENT %.2fA > %.2fA -> stopHard\n", currentA, limitA);
#endif
            const String action = cfg->gateConfig.overCurrentAction;
            // latch diagnostic info
            lastOverCurrentMs = millis();
            lastOverCurrentA = currentA;
            // enforce cooldown (blocks motion)
            const uint32_t cdMs = (cfg ? cfg->gateConfig.overCurrentCooldownMs : 4000);
            overCurrentCooldownUntilMs = (cdMs > 0 ? (millis() + cdMs) : 0);
            motor->stopHard();
            if (motor && motor->isHoverUart()) motor->hoverDisarm();
            overCurrentSinceMs = 0;
            if (action == "reverse") {
              status.lastStopReason = GATE_STOP_OVER_CURRENT;
              status.lastMoveMs = millis();
              publishStatusIfChanged();
              float reverseM = 0.5f;
              if (cfg) {
                reverseM = (float)cfg->safetyConfig.obstacleReverseCm / 100.0f;
              }
              float target = status.position;
              const float maxDistance = configuredMaxDistance();
              if (lastDirection >= 0) {
                target = status.position - reverseM;
                if (target < 0.0f) target = 0.0f;
                startMoveTo(target, false, GATE_CLOSING);
              } else {
                target = status.position + reverseM;
                if (maxDistance > 0.0f && target > maxDistance) target = maxDistance;
                startMoveTo(target, true, GATE_OPENING);
              }
            } else {
              setError(GATE_ERR_OVER_CURRENT, GATE_STOP_OVER_CURRENT);
            }
            return;
          }
        } else {
          overCurrentSinceMs = 0;
        }
      } else {
        overCurrentSinceMs = 0;
      }
    }
    if (tel.fault != 0) {
      // Hoverboard reported a fault -> stop immediately.
      // NOTE: Avoid STOP log spam: update() runs every loop, so latch the fault once.
      if (state == GATE_ERROR && errorCode == GATE_ERR_HOVER_FAULT) {
        return;
      }
#if defined(GATE_DEBUG_UART)
      Serial.printf("[GATE] HOVER FAULT=%d -> STOP(hover_fault) + ERROR\n", tel.fault);
#endif
      // Log STOP once (with telAgeMs/stallAgeMs), then transition to ERROR.
      stop(GATE_STOP_HOVER_FAULT);
      setError(GATE_ERR_HOVER_FAULT, GATE_STOP_HOVER_FAULT);
      return;
    } else if (moving && tel.lastTelMs == 0 && millis() - moveStart > 1500) {
      // We started moving but never received telemetry -> unsafe to continue.
#if defined(GATE_DEBUG_UART)
      Serial.printf("[GATE] HOVER TEL missing after start (%lums) -> stopHard + TEL_TIMEOUT\n", (unsigned long)(millis() - moveStart));
#endif
      motor->stopHard();
      lastHoverLossMs = millis();
      setError(GATE_ERR_HOVER_TEL_TIMEOUT);
      stop(GATE_STOP_TELEMETRY_TIMEOUT);
      return;
    } else {
      // Telemetry watchdog while moving.
      // Some hover firmwares send TEL at ~2-3 Hz when idle and speed up when moving.
      // A short timeout (e.g. 250ms) causes false errors right after issuing a command.
      if (moving && (millis() - moveStart) > kHoverTelGraceMs && motor->hoverTelemetryTimedOut(millis(), kHoverTelTimeoutMs)) {
        // Telemetry lost while moving -> stop immediately.
#if defined(GATE_DEBUG_UART)
        long telAge = (tel.lastTelMs == 0) ? -1 : (long)(millis() - tel.lastTelMs);
        Serial.printf("[GATE] HOVER TEL timeout while moving (telAgeMs=%ld, grace=%lums, timeout=%lums) -> stopHard\n",
                      telAge,
                      (unsigned long)kHoverTelGraceMs,
                      (unsigned long)kHoverTelTimeoutMs);
#endif
        motor->stopHard();
        lastHoverLossMs = millis();
        setError(GATE_ERR_HOVER_TEL_TIMEOUT);
        stop(GATE_STOP_TELEMETRY_TIMEOUT);
        return;
      }
    }
    // If we requested stop but telemetry is gone, finalize stop to avoid hanging in "closing/opening".
    if (pendingStop && motor->hoverTelemetryTimedOut(millis(), kHoverTelTimeoutMs)) {
#if defined(GATE_DEBUG_UART)
      Serial.printf("[GATE] HOVER TEL timeout during pendingStop -> force stop\n");
#endif
      motor->stopHard();
      moving = false;
      pendingStop = false;
      setState(GATE_STOPPED);
      status.lastStopReason = GATE_STOP_TELEMETRY_TIMEOUT;
      status.targetPosition = status.position;
      status.lastMoveMs = millis();
      publishStatusIfChanged();
      return;
    }
  }
  motor->update();
  if (moving && motor && !motor->isMotionActive()) {
    if (motor->isHoverUart()) {
      const HoverTelemetry& tel = motor->hoverTelemetry();
      const int rpm_deadband = 10;
      const int min_frames = 3;
      if (tel.lastTelMs != 0 && tel.lastTelMs != lastStopTelMs) {
        lastStopTelMs = tel.lastTelMs;
        int16_t cmdSpeed = motor->hoverLastCmdSpeed();
        if (abs(tel.rpm) <= rpm_deadband && cmdSpeed == 0) {
          if (stopConfirmCount < 250) stopConfirmCount++;
        } else {
          stopConfirmCount = 0;
        }
      }
      if (stopConfirmCount < min_frames) {
        return;
      }
    }
    #if defined(DEBUG)
    float err_m = status.targetPosition - controlPosition;
    Serial.printf("Gate stop: err=%.1fmm pos=%.3fm target=%.3fm\n", err_m * 1000.0f, status.position, status.targetPosition);
    #endif
    lastFinalErrorMm = (int)lroundf((status.targetPosition - controlPosition) * 1000.0f);

    // When motion is finished, explicitly DISARM the hoverboard firmware.
    // This prevents nuisance beeps if the ESP32 is briefly busy (e.g. flash writes)
    // and also reduces power draw/heat while idle.
    if (motor && motor->isHoverUart()) {
      motor->hoverDisarm();
#if defined(GATE_DEBUG_UART)
      Serial.printf("[GATE] Motion finished -> DISARM sent to hover firmware\n");
#endif
    }

    moving = false;
    pendingStop = false;
    setState(GATE_STOPPED);
    if (status.lastStopReason == GATE_STOP_USER) {
      status.targetPosition = status.position;
      lastFinalErrorMm = 0;
    } else {
      status.targetPosition = status.position;
    }
    publishStatusIfChanged();
  }
  if (cfg->safetyConfig.watchdogEnabled) {
    esp_task_wdt_reset();
  }
}

bool GateController::open() {
  float target = configuredMaxDistance();
  return startMoveTo(target, true, GATE_OPENING);
}

bool GateController::close() {
  return startMoveTo(0.0f, false, GATE_CLOSING);
}

bool GateController::moveTo(float targetMeters) {
  float target = targetMeters;
  if (target < 0.0f) target = 0.0f;
  float maxDistance = configuredMaxDistance();
  if (maxDistance > 0.0f && target > maxDistance) target = maxDistance;

  const float eps = 0.010f; // 10mm
  float pos = status.position;
  if (fabsf(target - pos) <= eps) {
    status.targetPosition = pos;
    publishStatusIfChanged();
    return true;
  }

  const bool forward = target > pos;
  return startMoveTo(target, forward, forward ? GATE_OPENING : GATE_CLOSING);
}

void GateController::stop() {
  stop(GATE_STOP_USER);
}

bool GateController::startMoveTo(float target, bool forward, GateState nextState) {
  if (!canMove()) {
    Serial.printf("[GATE] startMove blocked: canMove=false target=%.3fm\n", target);
    return false;
  }
  if (motor && motor->isHoverUart() && !motor->hoverEnabled()) {
    Serial.printf("[GATE] startMove blocked: hover offline target=%.3fm\n", target);
    return false;
  }
  moving = true;
  pendingStop = false;
  setState(nextState);
  lastDirection = forward ? 1 : -1;
  stopConfirmCount = 0;
  lastStopTelMs = 0;
  moveStart = millis();
  // reset OC auto-rearm counter at the start of a fresh move
  overCurrentAutoRearmCount = 0;
  lastProgressMs = moveStart;
  if (motor && motor->isHoverUart()) {
    const HoverTelemetry& tel = motor->hoverTelemetry();
    lastProgressPos = (tel.lastTelMs != 0) ? ((float)tel.distMm / 1000.0f) : controlPosition;
  } else {
    lastProgressPos = status.position;
  }
  status.lastMoveMs = moveStart;
  errorCode = GATE_ERR_NONE;
  status.error = errorCode;
  status.lastStopReason = GATE_STOP_NONE;
  status.targetPosition = target;
  if (motor) {
    motor->setDirection(forward);
    motor->setTarget(forward, target);
  }
#if defined(GATE_DEBUG_UART)
  Serial.printf("[GATE] MOVE cmd: startPos=%.3fm target=%.3fm dir=%s\n",
                status.position,
                target,
                forward ? "open" : "close");
#endif
  publishStatusIfChanged();
  return true;
}

void GateController::stop(GateStopReason reason) {
  // De-duplicate STOP spam: soft-limit / safety checks can call stop() repeatedly
  // while a stop is already pending (hover UART) or after we already stopped.
  if ((pendingStop || !moving) && reason != GATE_STOP_NONE && status.lastStopReason == reason) {
    return;
  }
#if defined(GATE_DEBUG_UART)
  Serial.printf("[GATE] STOP requested reason=%s state=%s pos=%.3fm target=%.3fm\n",
                getStopReasonString(reason),
                getStateString(),
                status.position,
                status.targetPosition);
#endif
  long telAgeMs = -1;
  if (motor && motor->isHoverUart()) {
    const HoverTelemetry& tel = motor->hoverTelemetry();
    if (tel.lastTelMs != 0 && millis() >= tel.lastTelMs) {
      telAgeMs = (long)(millis() - tel.lastTelMs);
    }
  }
  long stallAgeMs = (lastProgressMs != 0 && millis() >= lastProgressMs) ? (long)(millis() - lastProgressMs) : -1;
  Serial.printf("[GATE] STOP reason=%s telAgeMs=%ld stallAgeMs=%ld\n",
                getStopReasonString(reason),
                telAgeMs,
                stallAgeMs);

  // Hard-immediate stop reasons:
  // - USER: remote/button/toggle while moving
  // - OBSTACLE: photocell / safety obstacle
  // - ERROR/FAULT/TEL timeout/stall/overcurrent: safety-critical
  const bool hardImmediate =
      (reason == GATE_STOP_USER) ||
      (reason == GATE_STOP_OBSTACLE) ||
      (reason == GATE_STOP_ERROR) ||
      (reason == GATE_STOP_HOVER_FAULT) ||
      (reason == GATE_STOP_TELEMETRY_TIMEOUT) ||
      (reason == GATE_STOP_TELEMETRY_STALL) ||
      (reason == GATE_STOP_OVER_CURRENT);

  if (hardImmediate) {
    Serial.printf("[GATE] STOP mode=hard_immediate reason=%s\n", getStopReasonString(reason));
    userStopBoostUntilMs = 0;
    if (motor) {
      motor->setForceDecelBoost(false);
      motor->stopHard();
      const bool disarmNeeded =
          (reason == GATE_STOP_ERROR) ||
          (reason == GATE_STOP_HOVER_FAULT) ||
          (reason == GATE_STOP_TELEMETRY_TIMEOUT) ||
          (reason == GATE_STOP_TELEMETRY_STALL) ||
          (reason == GATE_STOP_OVER_CURRENT);
      if (motor->isHoverUart() && disarmNeeded) {
        motor->hoverDisarm();
      }
    }
    moving = false;
    pendingStop = false;
    lastProgressMs = 0;
    if (state != GATE_ERROR) {
      setState(GATE_STOPPED);
      errorCode = GATE_ERR_NONE;
      status.error = errorCode;
    }
    status.lastStopReason = reason;
    status.targetPosition = status.position;
    status.lastMoveMs = millis();
    stopConfirmCount = 0;
    lastStopTelMs = 0;
    publishStatusIfChanged();
    return;
  }

  if (reason == GATE_STOP_LIMIT_OPEN || reason == GATE_STOP_LIMIT_CLOSE) {
    if (motor) {
      motor->stopHard();
    }
    moving = false;
    pendingStop = false;
    lastProgressMs = 0;
    if (state != GATE_ERROR) {
      setState(GATE_STOPPED);
      errorCode = GATE_ERR_NONE;
      status.error = errorCode;
    }
    status.lastStopReason = reason;
    status.targetPosition = status.position;
    status.lastMoveMs = millis();
    stopConfirmCount = 0;
    lastStopTelMs = 0;
    publishStatusIfChanged();
    return;
  }

  // Soft-limits are also a "terminal" condition for motion.
  // Previously we left the controller in a moving state (pendingStop), which caused
  // the soft-limit check to call stop() every loop -> log spam and UI showing "closing".
  // For soft-limits we do a soft stop, disarm (hover UART) and immediately transition to STOPPED.
  if (reason == GATE_STOP_SOFT_LIMIT) {
    if (motor) {
      motor->stopSoft();
      if (motor->isHoverUart()) {
        motor->hoverDisarm();
      }
    }
    moving = false;
    pendingStop = false;
    lastProgressMs = 0;
    if (state != GATE_ERROR) {
      setState(GATE_STOPPED);
      errorCode = GATE_ERR_NONE;
      status.error = errorCode;
    }
    status.lastStopReason = reason;
    status.targetPosition = status.position;
    status.lastMoveMs = millis();
    stopConfirmCount = 0;
    lastStopTelMs = 0;
    publishStatusIfChanged();
    return;
  }
  if (motor) {
    motor->stopSoft();
  }
#if defined(GATE_DEBUG_UART)
  Serial.printf("[GATE] STOP cmd (soft) state=%s pos=%.3fm target=%.3fm moving=%d pendingStop=%d\n",
                getStateString(),
                status.position,
                status.targetPosition,
                moving ? 1 : 0,
                pendingStop ? 1 : 0);
#endif
  status.lastStopReason = reason;
  if (motor && motor->isHoverUart()) {
    if (reason == GATE_STOP_TELEMETRY_TIMEOUT ||
        reason == GATE_STOP_TELEMETRY_STALL ||
        reason == GATE_STOP_HOVER_FAULT ||
        reason == GATE_STOP_OVER_CURRENT) {
      motor->hoverDisarm();
    }
    pendingStop = true;
    stopConfirmCount = 0;
    lastStopTelMs = 0;
    return;
  }
  moving = false;
  pendingStop = false;
  lastProgressMs = 0;
  if (state != GATE_ERROR) {
    setState(GATE_STOPPED);
  }
  stopConfirmCount = 0;
  lastStopTelMs = 0;
  if (state != GATE_ERROR) {
    errorCode = GATE_ERR_NONE;
    status.error = errorCode;
  }
  if (motor && motor->isHoverUart()) {
    if (reason == GATE_STOP_TELEMETRY_TIMEOUT ||
        reason == GATE_STOP_TELEMETRY_STALL ||
        reason == GATE_STOP_HOVER_FAULT) {
      motor->hoverDisarm();
    }
  }
  status.targetPosition = status.position;
  status.lastMoveMs = millis();
  publishStatusIfChanged();
}

void GateController::setError(GateErrorCode code) {
#if defined(GATE_DEBUG_UART)
  Serial.printf("[GATE] ERROR code=%d state=%s pos=%.3fm target=%.3fm\n", (int)code, getStateString(), status.position, status.targetPosition);
#endif
  if (motor) {
    motor->stopHard();
  }
#if defined(GATE_DEBUG_UART)
  Serial.printf("[GATE] ERROR=%d state=%s pos=%.3fm target=%.3fm\n", (int)code, getStateString(), status.position, status.targetPosition);
#endif
  moving = false;
  pendingStop = false;
  setState(GATE_ERROR);
  errorCode = code;
  status.error = errorCode;
  if (code == GATE_ERR_HOVER_TEL_TIMEOUT || code == GATE_ERR_HOVER_OFFLINE) {
    lastHoverLossMs = millis();
  }
  hoverRecoveryActive = false;
  hoverRecoverySinceMs = 0;
  status.lastStopReason = GATE_STOP_ERROR;
  status.targetPosition = status.position;
  status.lastMoveMs = millis();
  publishStatusIfChanged();
}

void GateController::setError(GateErrorCode code, GateStopReason reason) {
#if defined(GATE_DEBUG_UART)
  Serial.printf("[GATE] ERROR code=%d reason=%s state=%s pos=%.3fm target=%.3fm\n",
                (int)code,
                getStopReasonString(reason),
                getStateString(),
                status.position,
                status.targetPosition);
#endif
  long telAgeMs = -1;
  if (motor && motor->isHoverUart()) {
    const HoverTelemetry& tel = motor->hoverTelemetry();
    if (tel.lastTelMs != 0 && millis() >= tel.lastTelMs) {
      telAgeMs = (long)(millis() - tel.lastTelMs);
    }
  }
  long stallAgeMs = (lastProgressMs != 0 && millis() >= lastProgressMs) ? (long)(millis() - lastProgressMs) : -1;
  Serial.printf("[GATE] ERROR reason=%s telAgeMs=%ld stallAgeMs=%ld\n",
                getStopReasonString(reason),
                telAgeMs,
                stallAgeMs);
  if (motor) {
    motor->stopHard();
    if (motor->isHoverUart()) {
      motor->hoverDisarm();
    }
  }
  moving = false;
  pendingStop = false;
  setState(GATE_ERROR);
  errorCode = code;
  status.error = errorCode;
  if (code == GATE_ERR_HOVER_TEL_TIMEOUT || code == GATE_ERR_HOVER_OFFLINE) {
    lastHoverLossMs = millis();
  }
  hoverRecoveryActive = false;
  hoverRecoverySinceMs = 0;
  status.lastStopReason = reason;
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

const char* GateController::getStopReasonString(GateStopReason reason) const {
  switch (reason) {
    case GATE_STOP_USER: return "user";
    case GATE_STOP_SOFT_LIMIT: return "soft_limit";
    case GATE_STOP_TELEMETRY_TIMEOUT: return "telemetry_timeout";
    case GATE_STOP_TELEMETRY_STALL: return "telemetry_stall";
    case GATE_STOP_HOVER_FAULT: return "hover_fault";
    case GATE_STOP_LIMIT_OPEN: return "limit_open";
    case GATE_STOP_LIMIT_CLOSE: return "limit_close";
    case GATE_STOP_OBSTACLE: return "obstacle";
    case GATE_STOP_ERROR: return "error";
    case GATE_STOP_OVER_CURRENT: return "over_current";
    case GATE_STOP_NONE:
    default:
      return "none";
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
    controlPosition = status.position;
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
  controlPosition = nextPos;
  if (nextMax > 0.0f) {
    int pct = (int)((nextPos * 100.0f) / nextMax + 0.5f);
    if (pct > 100) pct = 100;
    if (pct < 0) pct = 0;
    status.positionPercent = pct;
  } else {
    status.positionPercent = -1;
  }

  if (!moving && !pendingStop) {
    if (state == GATE_OPENING) {
      status.targetPosition = status.maxDistance;
    } else if (state == GATE_CLOSING) {
      status.targetPosition = 0.0f;
    } else {
      status.targetPosition = status.position;
    }
  }

  if (moving) {
    const bool softLimitsEnabled = cfg ? cfg->gateConfig.softLimitsEnabled : true;
    if (softLimitsEnabled && state == GATE_OPENING && status.maxDistance > 0.0f && status.position >= status.maxDistance) {
      stop(GATE_STOP_SOFT_LIMIT);
      return;
    }
    if (softLimitsEnabled && state == GATE_CLOSING && status.position <= 0.0f) {
      stop(GATE_STOP_SOFT_LIMIT);
      return;
    }
  }

  publishStatusIfChanged();
}

void GateController::setControlPosition(float position) {
  float nextPos = position;
  if (nextPos < 0.0f) nextPos = 0.0f;
  if (status.maxDistance > 0.0f && nextPos > status.maxDistance) nextPos = status.maxDistance;
  controlPosition = nextPos;
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
  if (state == GATE_ERROR || errorCode != GATE_ERR_NONE) return false;
  if (status.otaInProgress) return false;
  // Block motion during over-current cooldown window.
  if (overCurrentCooldownUntilMs != 0 && millis() < overCurrentCooldownUntilMs) return false;
  if (motor && motor->isHoverUart()) {
    if (!motor->hoverEnabled()) return false;
    const HoverTelemetry& tel = motor->hoverTelemetry();
    if (tel.fault != 0) return false;
    const uint32_t kHoverTelTimeoutMs = cfg ? cfg->gateConfig.telemetryTimeoutMs : 1200;
    if (tel.lastTelMs != 0 && motor->hoverTelemetryTimedOut(millis(), kHoverTelTimeoutMs)) {
      return false;
    }
  }
  float maxDistance = configuredMaxDistance();
  if (maxDistance > 0.0f) return true;
  if (cfg && cfg->gateConfig.allowMoveWithoutLimits) return true;
  return false;
}

bool GateController::clearError() {
  if (moving) return false;
  if (state != GATE_ERROR && errorCode == GATE_ERR_NONE) return true;
  errorCode = GATE_ERR_NONE;
  status.error = errorCode;
  status.lastStopReason = GATE_STOP_NONE;
  status.targetPosition = status.position;
  overCurrentSinceMs = 0;
  overCurrentCooldownUntilMs = 0;
  hoverRecoveryActive = false;
  hoverRecoverySinceMs = 0;
  setState(GATE_STOPPED);
  status.lastMoveMs = millis();
  publishStatusIfChanged();
  return true;
}

void GateController::onLimitOpen() {
  // Hard resync position at open limit
  float md = status.maxDistance > 0.0f ? status.maxDistance : configuredMaxDistance();
  if (md > 0.0f) {
    status.position = md;
    status.targetPosition = md;
    controlPosition = md;
  }
  if (state == GATE_OPENING || moving) {
    stop(GATE_STOP_LIMIT_OPEN);
  } else {
    publishStatusIfChanged();
  }
}

void GateController::onLimitClose() {
  // Hard resync position at close limit
  status.position = 0.0f;
  status.targetPosition = 0.0f;
  controlPosition = 0.0f;
  if (state == GATE_CLOSING || moving) {
    stop(GATE_STOP_LIMIT_CLOSE);
  } else {
    publishStatusIfChanged();
  }
}

GateCommandResponse GateController::onObstacle(bool active) {
  GateCommandResponse resp;
  resp.cmd = GATE_CMD_NONE;
  resp.result = GATE_CMD_OK;
  resp.applied = false;
  static constexpr uint32_t kObstacleRefractoryMs = 800;
  const uint32_t nowMs = millis();

  // Track obstacle state in status.
  setObstacle(active);

  // Only trigger an action on rising edge.
  const bool rising = active && !lastObstacle;
  lastObstacle = active;
  if (!rising) return resp;

  // Photocell is only active while the gate is closing.
  // Ignore obstacle edges while idle or opening.
  if (!isMoving() || state != GATE_CLOSING) {
    return resp;
  }

  // Ignore repeated obstacle edges for a short window to avoid flap/retrigger spam.
  if (obstacleRefractoryUntilMs != 0 && nowMs < obstacleRefractoryUntilMs) {
    return resp;
  }
  obstacleRefractoryUntilMs = nowMs + kObstacleRefractoryMs;

  const String action = cfg ? cfg->safetyConfig.obstacleAction : String("open");

  stop(GATE_STOP_OBSTACLE);
  if (action == "stop") {
    resp.cmd = GATE_CMD_STOP;
    resp.applied = true;
    resp.result = GATE_CMD_OK;
    return resp;
  }

  // "open" and "reverse" are equivalent while closing.
  resp.cmd = GATE_CMD_OPEN;
  const bool ok = open();
  resp.applied = ok;
  resp.result = ok ? GATE_CMD_OK : GATE_CMD_BLOCKED;
  return resp;
}

void GateController::onStopInput() {
  stop(GATE_STOP_USER);
}

void GateController::onLimitsInvalid() {
  // Both limits active -> error condition.
  if (getErrorCode() != GATE_ERR_LIMITS_INVALID) {
    setError(GATE_ERR_LIMITS_INVALID);
  }
  stop(GATE_STOP_ERROR);
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

  if (strncmp(buf, "goto:", 5) == 0) {
    const float target = (float)atof(buf + 5);
    const float posBefore = getPosition();
    const bool forward = target > posBefore;
    bool ok = moveTo(target);
    resp.cmd = forward ? GATE_CMD_OPEN : GATE_CMD_CLOSE;
    resp.applied = ok;
    resp.result = ok ? GATE_CMD_OK : GATE_CMD_BLOCKED;
    return resp;
  }

  if (strncmp(buf, "goto_mm:", 8) == 0) {
    const float targetMm = (float)atof(buf + 8);
    const float target = targetMm / 1000.0f;
    const float posBefore = getPosition();
    const bool forward = target > posBefore;
    bool ok = moveTo(target);
    resp.cmd = forward ? GATE_CMD_OPEN : GATE_CMD_CLOSE;
    resp.applied = ok;
    resp.result = ok ? GATE_CMD_OK : GATE_CMD_BLOCKED;
    return resp;
  }

  if (strcmp(buf, "stop") == 0) {
    resp.cmd = GATE_CMD_STOP;
    bool wasMoving = isMoving();
    stop(GATE_STOP_USER);
    resp.applied = wasMoving;
    resp.result = GATE_CMD_OK;
    return resp;
  }

  if (strcmp(buf, "reset") == 0 || strcmp(buf, "ack") == 0 || strcmp(buf, "clear_error") == 0) {
    resp.cmd = GATE_CMD_STOP;
    bool ok = clearError();
    resp.applied = ok;
    resp.result = ok ? GATE_CMD_OK : GATE_CMD_BLOCKED;
    return resp;
  }

  if (strcmp(buf, "toggle") == 0) {
    resp.cmd = GATE_CMD_TOGGLE;
    if (isMoving()) {
      stop(GATE_STOP_USER);
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
