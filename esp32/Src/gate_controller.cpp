// ============================================================
//  GateOS — gate_controller.cpp  (PATCHED 2026-04-17)
//  Fixes applied:
//    B-03  bypassCooldown -> bypassOCCooldown (narrower bypass)
//    B-08  millis() cast to long removed from stop()
//    B-11  stopConfirmCount min_frames 3->8 (≥160 ms at 50 Hz)
//    B-13  NaN/Inf guard in handleCommand() goto:/goto_mm:
//  NOTE: B-04 (WDT registration) is fixed in app_main.cpp gateTask().
//        The begin() WDT init block is unchanged intentionally.
// ============================================================
#include "gate_controller.h"
#include <esp_task_wdt.h>
#include <string.h>
#include <math.h>

GateController::GateController(MotorController* motor_, ConfigManager* cfg_) : motor(motor_), cfg(cfg_) {
  moving = false;
  moveStart = 0;
  state = GATE_STOPPED;
  lastDirection = 1;
  status.state = state;
  status.error = errorCode;
  status.lastStopReason = GATE_STOP_NONE;
}

GateDecisionContext GateController::decisionContext() const {
  GateDecisionContext ctx;
  ctx.moving = moving;
  ctx.pendingStop = pendingStop;
  ctx.limitOpenActive = limitOpenActive;
  ctx.limitCloseActive = limitCloseActive;
  ctx.terminalState = terminalState;
  ctx.position = status.position;
  ctx.maxDistance = configuredMaxDistance();
  ctx.lastDirection = lastDirection;
  ctx.userStoppedDuringMove = userStoppedDuringMove_;
  return ctx;
}

void GateController::setTerminalState(GateTerminalState next) {
  if (terminalState == next) return;
  terminalState = next;
#if defined(GATE_DEBUG_UART)
  Serial.printf("[GATE] terminal_state=%d pos=%.3fm moving=%d pendingStop=%d limitOpen=%d limitClose=%d\n",
                (int)terminalState,
                status.position,
                moving ? 1 : 0,
                pendingStop ? 1 : 0,
                limitOpenActive ? 1 : 0,
                limitCloseActive ? 1 : 0);
#endif
}

void GateController::refreshTerminalStateFromPosition() {
  if (limitOpenActive && !limitCloseActive) {
    setTerminalState(GateTerminalState::FullyOpen);
    return;
  }
  if (limitCloseActive && !limitOpenActive) {
    setTerminalState(GateTerminalState::FullyClosed);
    return;
  }
}

void GateController::begin() {
  if (cfg->safetyConfig.watchdogEnabled) {
    // Init system WDT — task registration (esp_task_wdt_add) happens
    // in gateTask() in app_main.cpp so the correct task handle is used.
    esp_task_wdt_init(10, true);
  }
  status.lastStateChangeMs = millis();
  if (motor) {
    motor->setMotionProfile(cfg->motionProfile());
  }
  setPosition(0.0f, cfg->gateConfig.maxDistance);
}

void GateController::loop() {
  (void)onObstacle(lastObstacle);

  if (motor) {
    motor->tick(millis(), controlPosition);
  }
  if (userStopBoostUntilMs != 0 && millis() >= userStopBoostUntilMs) {
    if (motor && motor->isHoverUart()) {
      motor->setForceDecelBoost(false);
    }
    userStopBoostUntilMs = 0;
  }
  // auto-clear over-current error after cooldown
  if (state == GATE_ERROR && errorCode == GATE_ERR_OVER_CURRENT && overCurrentCooldownUntilMs != 0 && millis() >= overCurrentCooldownUntilMs) {
    overCurrentCooldownUntilMs = 0;
    errorCode = GATE_ERR_NONE;
    status.error = errorCode;
    moving = false;
    pendingStop = false;
    setState(GATE_STOPPED);
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

  // === FIX #1: Level-based limit safety check ===
  // Runs in GateTask every 10 ms. Stops the gate within ≤10 ms of a limit
  // switch activation, independent of main-loop timing jitter.
  // limitOpenActive / limitCloseActive are volatile — written atomically from
  // main loop (updateLimitState / onLimitOpen / onLimitClose), read here.
  if (moving && !pendingStop) {
    const bool relevantLimitActive =
        (state == GATE_OPENING && limitOpenActive) ||
        (state == GATE_CLOSING && limitCloseActive);
    if (relevantLimitActive) {
      const uint32_t nowMs = millis();
      if (limitActiveStartMs_ == 0) limitActiveStartMs_ = nowMs;
      limitActiveWhileMovingMs_ = nowMs - limitActiveStartMs_;
      const GateStopReason safetyReason =
          (state == GATE_OPENING) ? GATE_STOP_LIMIT_OPEN : GATE_STOP_LIMIT_CLOSE;
      limitSafetyStopCount_++;
      Serial.printf("[GATE] SAFETY level-stop: %s=1 while %s pos=%.3fm activeMs=%lu stopCount=%lu\n",
                    (state == GATE_OPENING) ? "limitOpen" : "limitClose",
                    (state == GATE_OPENING) ? "OPENING" : "CLOSING",
                    status.position,
                    (unsigned long)limitActiveWhileMovingMs_,
                    (unsigned long)limitSafetyStopCount_);
      stop(safetyReason);
      limitActiveStartMs_ = 0;
      limitActiveWhileMovingMs_ = 0;
      return;
    } else {
      // Limit inactive or wrong direction — reset diagnostic timer
      limitActiveStartMs_ = 0;
      limitActiveWhileMovingMs_ = 0;
    }
  } else if (!moving) {
    limitActiveStartMs_ = 0;
    limitActiveWhileMovingMs_ = 0;
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
    // Hover motors need a larger margin: after emergencyStop the motor still decelerates
    // over ~10-20 cm before stopping. 20 cm gives room to stop at the physical limit switch.
    const bool isHoverMotor = motor && motor->isHoverUart() && motor->hoverEnabled();
    const float softLimitEpsM = isHoverMotor ? 0.20f : 0.02f;
    // Use raw controlPosition (less filter lag) so the check fires closer to true position.
    const float posCheck = controlPosition;
        if (!pendingStop && softLimitsEnabled && state == GATE_OPENING && status.maxDistance > 0.0f && posCheck >= (status.maxDistance - softLimitEpsM)) {
#if defined(GATE_DEBUG_UART)
      Serial.printf("[GATE] soft-limit OPEN reached raw=%.3fm filt=%.3fm max=%.3fm eps=%.3fm -> stop()\n", controlPosition, status.position, status.maxDistance, softLimitEpsM);
#endif
      stop(GATE_STOP_SOFT_LIMIT);
    }
        if (!pendingStop && softLimitsEnabled && state == GATE_CLOSING && posCheck <= softLimitEpsM) {
#if defined(GATE_DEBUG_UART)
      Serial.printf("[GATE] soft-limit CLOSE reached raw=%.3fm filt=%.3fm eps=%.3fm -> stop()\n", controlPosition, status.position, softLimitEpsM);
#endif
      stop(GATE_STOP_SOFT_LIMIT);
    }
    if (!pendingStop &&
        cfg &&
        (cfg->gateConfig.stallTimeoutMs > 0 || cfg->gateConfig.movementTimeout > 0) &&
        lastProgressMs != 0) {
      const unsigned long timeoutMs = (unsigned long)(cfg->gateConfig.stallTimeoutMs > 0 ? cfg->gateConfig.stallTimeoutMs : cfg->gateConfig.movementTimeout);
      const unsigned long graceMs   = (unsigned long)(cfg->gateConfig.telemetryGraceMs > 0 ? cfg->gateConfig.telemetryGraceMs : 0);
      if (timeoutMs > 0 && (millis() - moveStart) > graceMs && (millis() - lastProgressMs) > timeoutMs) {
        setError(GATE_ERR_TIMEOUT);
        stop(GATE_STOP_TELEMETRY_STALL);
        return;
      }
    }
  }
  if (motor && motor->isHoverUart()) {
    const uint32_t kHoverTelTimeoutMs = cfg ? cfg->gateConfig.telemetryTimeoutMs : 1200;
    const uint32_t kHoverTelGraceMs   = cfg ? cfg->gateConfig.telemetryGraceMs   : 1500;
    const uint32_t kHoverTelTimeoutHystMs = 250;
    const uint32_t kHoverRecoveryStableMs = 1500;
    const HoverTelemetry& tel = motor->hoverTelemetry();

    // Auto-recovery for COMM-only hover errors
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
            lastOverCurrentMs = millis();
            lastOverCurrentA = currentA;
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
              bool reverseStarted = false;
              if (lastDirection >= 0) {
                target = status.position - reverseM;
                if (target < 0.0f) target = 0.0f;
                // FIX B-03: bypassOCCooldown=true only bypasses OC cooldown,
                //           not the general canMove() safety checks.
                reverseStarted = startMoveTo(target, false, GATE_CLOSING, /*bypassOCCooldown=*/true);
              } else {
                target = status.position + reverseM;
                if (maxDistance > 0.0f && target > maxDistance) target = maxDistance;
                reverseStarted = startMoveTo(target, true, GATE_OPENING, /*bypassOCCooldown=*/true);
              }
              if (!reverseStarted) {
                setError(GATE_ERR_OVER_CURRENT, GATE_STOP_OVER_CURRENT);
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
      if (state == GATE_ERROR && errorCode == GATE_ERR_HOVER_FAULT) {
        return;
      }
#if defined(GATE_DEBUG_UART)
      Serial.printf("[GATE] HOVER FAULT=%d -> STOP(hover_fault) + ERROR\n", tel.fault);
#endif
      stop(GATE_STOP_HOVER_FAULT);
      setError(GATE_ERR_HOVER_FAULT, GATE_STOP_HOVER_FAULT);
      return;
    } else if (moving && tel.lastTelMs == 0 && millis() - moveStart > 1500) {
#if defined(GATE_DEBUG_UART)
      Serial.printf("[GATE] HOVER TEL missing after start (%lums) -> stopHard + TEL_TIMEOUT\n", (unsigned long)(millis() - moveStart));
#endif
      motor->stopHard();
      lastHoverLossMs = millis();
      setError(GATE_ERR_HOVER_TEL_TIMEOUT);
      stop(GATE_STOP_TELEMETRY_TIMEOUT);
      return;
    } else {
        if (moving && (millis() - moveStart) > kHoverTelGraceMs &&
          motor->hoverTelemetryTimedOut(millis(), kHoverTelTimeoutMs + kHoverTelTimeoutHystMs)) {
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
        if (pendingStop && motor->hoverTelemetryTimedOut(millis(), kHoverTelTimeoutMs + kHoverTelTimeoutHystMs)) {
#if defined(GATE_DEBUG_UART)
      Serial.printf("[GATE] HOVER TEL timeout during pendingStop -> force stop\n");
#endif
      motor->stopHard();
      motor->hoverDisarm();
      moving = false;
      pendingStop = false;
      pendingStopStartMs = 0;
      pendingStopStableSinceMs = 0;
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
      const uint32_t minStableMs = 160;
      const uint32_t maxPendingStopMs = 2500;
      if (pendingStopStartMs == 0) pendingStopStartMs = millis();

      if (tel.lastTelMs != 0 && tel.lastTelMs != lastStopTelMs) {
        lastStopTelMs = tel.lastTelMs;
        int16_t cmdSpeed = motor->hoverLastCmdSpeed();
        if (abs(tel.rpm) <= rpm_deadband && cmdSpeed == 0) {
          if (pendingStopStableSinceMs == 0) pendingStopStableSinceMs = millis();
        } else {
          pendingStopStableSinceMs = 0;
        }
      }

      const bool stable = pendingStopStableSinceMs != 0 && (millis() - pendingStopStableSinceMs) >= minStableMs;
      const bool waitedLongEnough = (millis() - pendingStopStartMs) >= maxPendingStopMs;
      if (!stable && !waitedLongEnough) {
        return;
      }
    }
    #if defined(DEBUG)
    float err_m = status.targetPosition - controlPosition;
    Serial.printf("Gate stop: err=%.1fmm pos=%.3fm target=%.3fm\n", err_m * 1000.0f, status.position, status.targetPosition);
    #endif
    lastFinalErrorMm = (int)lroundf((status.targetPosition - controlPosition) * 1000.0f);

    if (motor && motor->isHoverUart()) {
      motor->hoverDisarm();
#if defined(GATE_DEBUG_UART)
      Serial.printf("[GATE] Motion finished -> DISARM sent to hover firmware\n");
#endif
    }

    moving = false;
    pendingStop = false;
    pendingStopStartMs = 0;
    pendingStopStableSinceMs = 0;
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
  return startMove(GateMoveDirection::Open, target, GATE_OPENING);
}

bool GateController::close() {
  return startMove(GateMoveDirection::Close, 0.0f, GATE_CLOSING);
}

bool GateController::moveTo(float targetMeters) {
  float target = targetMeters;
  if (target < 0.0f) target = 0.0f;
  float maxDistance = configuredMaxDistance();
  if (maxDistance > 0.0f && target > maxDistance) target = maxDistance;

  const float eps = 0.010f;
  float pos = status.position;
  if (fabsf(target - pos) <= eps) {
    status.targetPosition = pos;
    publishStatusIfChanged();
    return true;
  }

  const bool forward = target > pos;
  return startMove(forward ? GateMoveDirection::Open : GateMoveDirection::Close,
                   target,
                   forward ? GATE_OPENING : GATE_CLOSING);
}

void GateController::stop() {
  stop(GATE_STOP_USER);
}

// FIX B-03: bypassOCCooldown replaces the old generic bypassCooldown.
//           Only skips the over-current cooldown window; all other
//           canMove() checks (hover fault, telemetry, OTA, etc.) remain active.
bool GateController::startMoveTo(float target, bool forward, GateState nextState, bool bypassOCCooldown) {
  return startMove(forward ? GateMoveDirection::Open : GateMoveDirection::Close, target, nextState, bypassOCCooldown);
}

bool GateController::startMove(GateMoveDirection dir, float target, GateState nextState, bool bypassOCCooldown) {
  const GateDecisionContext ctx = decisionContext();
  const GateMoveBlockReason block = validateMoveDirection(ctx, dir);
  if (block != GateMoveBlockReason::None) {
    Serial.printf("[GATE] startMove blocked: reason=%d target=%.3fm\n", (int)block, target);
    return false;
  }
  if (!canMove(bypassOCCooldown)) {
    Serial.printf("[GATE] startMove blocked: canMove=false target=%.3fm\n", target);
    return false;
  }
  if (motor && motor->isHoverUart() && !motor->hoverEnabled()) {
    Serial.printf("[GATE] startMove blocked: hover offline target=%.3fm\n", target);
    return false;
  }
  const bool forward = dir == GateMoveDirection::Open;
  moving = true;
  pendingStop = false;
  // Clear the user-stop flag — a new movement is starting.
  userStoppedDuringMove_ = false;
  setState(nextState);
  lastDirection = forward ? 1 : -1;
  setTerminalState(GateTerminalState::Unknown);
  stopConfirmCount = 0;
  lastStopTelMs = 0;
  pendingStopStartMs = 0;
  pendingStopStableSinceMs = 0;
  moveStart = millis();
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
  if (!moving && !pendingStop && state != GATE_ERROR) {
    return;
  }
  if ((pendingStop || !moving) && reason != GATE_STOP_NONE && status.lastStopReason == reason) {
    return;
  }

  // Track whether the user manually stopped the gate during movement.
  // This flag is used by resolveToggleDirection() to force direction
  // reversal on the next toggle — even if the gate is near an endpoint.
  if (reason == GATE_STOP_USER && moving) {
    userStoppedDuringMove_ = true;
    Serial.printf("[GATE] userStoppedDuringMove set, lastDir=%d\n", lastDirection);
  }

#if defined(GATE_DEBUG_UART)
  Serial.printf("[GATE] STOP requested reason=%s state=%s pos=%.3fm target=%.3fm\n",
                getStopReasonString(reason),
                getStateString(),
                status.position,
                status.targetPosition);
#endif
  // FIX B-08: removed (long) casts — keep arithmetic as uint32_t to survive millis() overflow.
  uint32_t telAgeMs = 0;
  bool hasTelAge = false;
  if (motor && motor->isHoverUart()) {
    const HoverTelemetry& tel = motor->hoverTelemetry();
    if (tel.lastTelMs != 0 && millis() >= tel.lastTelMs) {
      telAgeMs = millis() - tel.lastTelMs;
      hasTelAge = true;
    }
  }
  uint32_t stallAgeMs = 0;
  bool hasStallAge = false;
  if (lastProgressMs != 0 && millis() >= lastProgressMs) {
    stallAgeMs = millis() - lastProgressMs;
    hasStallAge = true;
  }
  Serial.printf("[GATE] STOP reason=%s telAgeMs=%ld stallAgeMs=%ld\n",
                getStopReasonString(reason),
                hasTelAge   ? (long)telAgeMs   : -1L,
                hasStallAge ? (long)stallAgeMs : -1L);

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
    pendingStopStartMs = 0;
    pendingStopStableSinceMs = 0;
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
    // Gate reached a physical limit — position is authoritative.
    userStoppedDuringMove_ = false;
    moving = false;
    pendingStop = false;
    pendingStopStartMs = 0;
    pendingStopStableSinceMs = 0;
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

  if (reason == GATE_STOP_SOFT_LIMIT) {
    if (motor) {
      // Use stopHard (emergencyStop → speed=0, regenerative braking) instead of
      // stopSoft+hoverDisarm. Disarming causes coasting; regenerative braking stops
      // the gate much faster, preventing overshoot past the physical limit switch.
      motor->stopHard();
    }
    const GateDecisionContext ctx = decisionContext();
    if (state == GATE_OPENING && (limitOpenActive || gateNearOpen(ctx, 0.02f))) {
      setTerminalState(GateTerminalState::FullyOpen);
    } else if (state == GATE_CLOSING && (limitCloseActive || gateNearClosed(ctx, 0.02f))) {
      setTerminalState(GateTerminalState::FullyClosed);
    }
    // Gate reached an endpoint — position is authoritative, clear user-stop flag.
    userStoppedDuringMove_ = false;
    moving = false;
    pendingStop = false;
    pendingStopStartMs = 0;
    pendingStopStableSinceMs = 0;
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
    pendingStopStartMs = millis();
    pendingStopStableSinceMs = 0;
    stopConfirmCount = 0;
    lastStopTelMs = 0;
    return;
  }
  moving = false;
  pendingStop = false;
  pendingStopStartMs = 0;
  pendingStopStableSinceMs = 0;
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
  pendingStopStartMs = 0;
  pendingStopStableSinceMs = 0;
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
  // FIX B-08: uint32_t arithmetic — no (long) cast.
  uint32_t telAgeMs = 0;
  bool hasTelAge = false;
  if (motor && motor->isHoverUart()) {
    const HoverTelemetry& tel = motor->hoverTelemetry();
    if (tel.lastTelMs != 0 && millis() >= tel.lastTelMs) {
      telAgeMs = millis() - tel.lastTelMs;
      hasTelAge = true;
    }
  }
  uint32_t stallAgeMs = 0;
  bool hasStallAge = false;
  if (lastProgressMs != 0 && millis() >= lastProgressMs) {
    stallAgeMs = millis() - lastProgressMs;
    hasStallAge = true;
  }
  Serial.printf("[GATE] ERROR reason=%s telAgeMs=%ld stallAgeMs=%ld\n",
                getStopReasonString(reason),
                hasTelAge   ? (long)telAgeMs   : -1L,
                hasStallAge ? (long)stallAgeMs : -1L);
  if (motor) {
    motor->stopHard();
    if (motor->isHoverUart()) {
      motor->hoverDisarm();
    }
  }
  moving = false;
  pendingStop = false;
  pendingStopStartMs = 0;
  pendingStopStableSinceMs = 0;
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
    case GATE_ERROR:   return "error";
    case GATE_STOPPED:
    default:
      return "stopped";
  }
}

const char* GateController::getStopReasonString(GateStopReason reason) const {
  switch (reason) {
    case GATE_STOP_USER:               return "user";
    case GATE_STOP_SOFT_LIMIT:         return "soft_limit";
    case GATE_STOP_TELEMETRY_TIMEOUT:  return "telemetry_timeout";
    case GATE_STOP_TELEMETRY_STALL:    return "telemetry_stall";
    case GATE_STOP_HOVER_FAULT:        return "hover_fault";
    case GATE_STOP_LIMIT_OPEN:         return "limit_open";
    case GATE_STOP_LIMIT_CLOSE:        return "limit_close";
    case GATE_STOP_OBSTACLE:           return "obstacle";
    case GATE_STOP_ERROR:              return "error";
    case GATE_STOP_OVER_CURRENT:       return "over_current";
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
  refreshTerminalStateFromPosition();
  if (!moving && !pendingStop) {
    status.targetPosition = status.position;
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

  refreshTerminalStateFromPosition();

  if (moving) {
    const bool softLimitsEnabled = cfg ? cfg->gateConfig.softLimitsEnabled : true;
    const bool isHoverMotor = motor && motor->isHoverUart() && motor->hoverEnabled();
    const float softLimitEpsM = isHoverMotor ? 0.20f : 0.02f;
    if (softLimitsEnabled && state == GATE_OPENING && status.maxDistance > 0.0f && status.position >= (status.maxDistance - softLimitEpsM)) {
      stop(GATE_STOP_SOFT_LIMIT);
      return;
    }
    if (softLimitsEnabled && state == GATE_CLOSING && status.position <= softLimitEpsM) {
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
  statusCb  = cb;
  statusCtx = ctx;
  publishStatusIfChanged();
}

void GateController::updateLimitState(bool openActive, bool closeActive) {
  limitOpenActive  = openActive;
  limitCloseActive = closeActive;
  refreshTerminalStateFromPosition();
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

// FIX B-03: bypassOCCooldown only skips OC cooldown — does NOT skip hover/fault checks.
bool GateController::canMove(bool bypassOCCooldown) const {
  if (state == GATE_ERROR || errorCode != GATE_ERR_NONE) return false;
  if (status.otaInProgress) return false;
  // Only skip OC cooldown, not everything else.
  if (!bypassOCCooldown && overCurrentCooldownUntilMs != 0 && millis() < overCurrentCooldownUntilMs) return false;
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
  limitOpenActive  = true;
  limitCloseActive = false;
  setTerminalState(GateTerminalState::FullyOpen);
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
  limitCloseActive = true;
  limitOpenActive  = false;
  setTerminalState(GateTerminalState::FullyClosed);
  status.position = 0.0f;
  status.targetPosition = 0.0f;
  controlPosition = 0.0f;
  if (state == GATE_CLOSING || moving) {
    stop(GATE_STOP_LIMIT_CLOSE);
  } else {
    publishStatusIfChanged();
  }
}

GateCommandResponse GateController::handleObstacleTrip(const char* actionOverride, bool immediateFollowUp) {
  GateCommandResponse resp;
  resp.cmd    = GATE_CMD_STOP;
  resp.result = GATE_CMD_OK;
  resp.applied = false;

  if (!isMoving() || state != GATE_CLOSING) {
    return resp;
  }

  stop(GATE_STOP_OBSTACLE);
  resp.applied = true;

  String action = actionOverride ? String(actionOverride)
                                 : (cfg ? cfg->safetyConfig.obstacleAction : String("open"));
  action.toLowerCase();
  if (!immediateFollowUp || action != "open") {
    return resp;
  }

  resp.followUpCmd = GATE_CMD_OPEN;
  const bool ok = open();
  if (ok) {
    resp.cmd = GATE_CMD_OPEN;
  } else {
    resp.followUpBlocked = true;
    Serial.println("[GATE] obstacle follow-up reopen blocked after successful stop");
  }
  return resp;
}

GateCommandResponse GateController::onObstacle(bool active) {
  GateCommandResponse resp;
  resp.cmd    = GATE_CMD_NONE;
  resp.result = GATE_CMD_OK;
  resp.applied = false;
  static constexpr uint32_t kObstacleRefractoryMs = 800;
  const uint32_t nowMs = millis();

  const bool closingTrip = active && isMoving() && state == GATE_CLOSING;
  const bool obstacleLatchedBefore = status.obstacle;
  setObstacle(closingTrip);
  const bool shouldTrigger = closingTrip && (!lastObstacle || !obstacleLatchedBefore);
  lastObstacle = active;
  if (!shouldTrigger) return resp;

  if (obstacleRefractoryUntilMs != 0 && nowMs < obstacleRefractoryUntilMs) {
    return resp;
  }
  obstacleRefractoryUntilMs = nowMs + kObstacleRefractoryMs;

  return handleObstacleTrip();
}

void GateController::onStopInput() {
  stop(GATE_STOP_USER);
}

void GateController::onLimitsInvalid() {
  if (getErrorCode() != GATE_ERR_LIMITS_INVALID) {
    setError(GATE_ERR_LIMITS_INVALID);
  }
  stop(GATE_STOP_ERROR);
}

GateCommandResponse GateController::handleCommand(const char* cmd) {
  GateCommandResponse resp;
  resp.cmd    = GATE_CMD_NONE;
  resp.result = GATE_CMD_OK;
  resp.applied = false;

  if (!cmd) {
    resp.result = GATE_CMD_UNKNOWN;
    return resp;
  }

  char buf[16];
  size_t n = strlen(cmd);
  if (n >= sizeof(buf)) n = sizeof(buf) - 1;
  for (size_t i = 0; i < n; ++i) {
    char c = cmd[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    buf[i] = c;
  }
  buf[n] = '\0';

  if (strcmp(buf, "open") == 0 || strcmp(buf, "otwórz") == 0) {
    resp.cmd    = GATE_CMD_OPEN;
    bool ok     = open();
    resp.applied = ok;
    resp.result = ok ? GATE_CMD_OK : GATE_CMD_BLOCKED;
    return resp;
  }

  if (strcmp(buf, "close") == 0 || strcmp(buf, "zamknij") == 0) {
    resp.cmd    = GATE_CMD_CLOSE;
    bool ok     = close();
    resp.applied = ok;
    resp.result = ok ? GATE_CMD_OK : GATE_CMD_BLOCKED;
    return resp;
  }

  // FIX B-13: NaN / Inf guard on atof() result for goto: and goto_mm:
  if (strncmp(buf, "goto:", 5) == 0) {
    const float raw = (float)atof(buf + 5);
    if (!isfinite(raw)) {
      resp.result = GATE_CMD_UNKNOWN;
      return resp;
    }
    const float posBefore = getPosition();
    const bool forward = raw > posBefore;
    bool ok = moveTo(raw);
    resp.cmd    = forward ? GATE_CMD_OPEN : GATE_CMD_CLOSE;
    resp.applied = ok;
    resp.result = ok ? GATE_CMD_OK : GATE_CMD_BLOCKED;
    return resp;
  }

  if (strncmp(buf, "goto_mm:", 8) == 0) {
    const float rawMm = (float)atof(buf + 8);
    if (!isfinite(rawMm)) {
      resp.result = GATE_CMD_UNKNOWN;
      return resp;
    }
    const float target = rawMm / 1000.0f;
    const float posBefore = getPosition();
    const bool forward = target > posBefore;
    bool ok = moveTo(target);
    resp.cmd    = forward ? GATE_CMD_OPEN : GATE_CMD_CLOSE;
    resp.applied = ok;
    resp.result = ok ? GATE_CMD_OK : GATE_CMD_BLOCKED;
    return resp;
  }

  if (strcmp(buf, "stop") == 0) {
    resp.cmd = GATE_CMD_STOP;
    bool wasMoving = isMoving();
    stop(GATE_STOP_USER);
    resp.applied = wasMoving;
    resp.result  = GATE_CMD_OK;
    return resp;
  }

  if (strcmp(buf, "reset") == 0 || strcmp(buf, "ack") == 0 || strcmp(buf, "clear_error") == 0) {
    resp.cmd = GATE_CMD_STOP;
    bool ok  = clearError();
    resp.applied = ok;
    resp.result  = ok ? GATE_CMD_OK : GATE_CMD_BLOCKED;
    return resp;
  }

  if (strcmp(buf, "toggle") == 0) {
    resp.cmd = GATE_CMD_TOGGLE;
    if (isMoving()) {
      stop(GATE_STOP_USER);
      resp.applied = true;
      resp.result  = GATE_CMD_OK;
      return resp;
    }

    const GateDecisionContext ctx = decisionContext();
    const GateMoveDirection dir = resolveToggleDirection(ctx);
    Serial.printf("[GATE] toggle: pos=%.3f max=%.3f lastDir=%d userStopped=%d terminal=%d -> dir=%d\n",
                  ctx.position, ctx.maxDistance, ctx.lastDirection,
                  ctx.userStoppedDuringMove ? 1 : 0,
                  (int)ctx.terminalState, (int)dir);
    bool ok = false;
    if (dir == GateMoveDirection::Open) {
      ok = open();
    } else if (dir == GateMoveDirection::Close) {
      ok = close();
    }
    resp.applied = ok;
    resp.result  = ok ? GATE_CMD_OK : GATE_CMD_BLOCKED;
    return resp;
  }

  resp.result = GATE_CMD_UNKNOWN;
  return resp;
}
