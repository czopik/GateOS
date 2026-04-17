/**
 * @file safety_manager.cpp
 * @brief Centralized safety management implementation
 *
 * FIX B-05: Corrected debounce algorithm in updateObstacle(), updateLimitOpen(),
 *           updateLimitClose(), updateStopButton().
 *
 *           OLD (broken): timer was reset on EVERY call when state == stable state,
 *           meaning it never accumulated stable time for a NEW state.
 *
 *           NEW (correct): timer is reset only when the RAW signal CHANGES.
 *           Acceptance happens after the raw signal has been STABLE for debounceMs.
 *           This is the standard "sticky timer" debounce pattern.
 */

#include "safety_manager.h"

SafetyManager::SafetyManager()
    : config(nullptr)
    , obstacleActive(false)
    , obstacleActivePrev(false)
    , limitOpenActive(false)
    , limitOpenActivePrev(false)
    , limitCloseActive(false)
    , limitCloseActivePrev(false)
    , stopPressed(false)
    , stopPressedPrev(false)
    , obstacleDebounceMs(30)
    , limitDebounceMs(30)
    , stopDebounceMs(30)
    , lastObstacleChangeMs(0)
    , lastLimitOpenChangeMs(0)
    , lastLimitCloseChangeMs(0)
    , lastStopChangeMs(0)
    // --- debounce pending raw values (NEW for B-05 fix) ---
    , obstacleRawPending(false)
    , limitOpenRawPending(false)
    , limitCloseRawPending(false)
    , stopRawPending(false)
    // ---
    , currentFault(SAFETY_FAULT_NONE)
    , faultStartTimeMs(0)
    , lastFaultMs(0)
    , faultCount(0)
    , faultLatched(false)
    , lastWatchdogFeedMs(0)
    , watchdogOk(true)
    , watchdogTimeoutMs(5000)
    , overCurrentThresholdA(10.0f)
    , overCurrentDurationMs(500)
    , overCurrentStartMs(0)
    , lastOverCurrentA(0.0f)
    , lastOverCurrentMs(0)
    , callback(nullptr)
    , callbackCtx(nullptr)
    , obstacleTriggerCount(0)
{
}

void SafetyManager::begin(ConfigManager* cfg) {
    config = cfg;
    
    if (cfg) {
        obstacleDebounceMs = cfg->sensorsConfig.photocell.debounceMs > 0 
            ? cfg->sensorsConfig.photocell.debounceMs : 30;
        limitDebounceMs = 30;
        stopDebounceMs  = 30;
        
        overCurrentThresholdA = cfg->motorConfig.overCurrentThreshold > 0
            ? cfg->motorConfig.overCurrentThreshold : 10.0f;
        overCurrentDurationMs = cfg->motorConfig.overCurrentDurationMs > 0
            ? cfg->motorConfig.overCurrentDurationMs : 500;
    }
    
    lastWatchdogFeedMs = millis();
    watchdogOk = true;

    // Seed pending-raw values with current state to avoid spurious edge on first poll
    obstacleRawPending  = obstacleActive;
    limitOpenRawPending = limitOpenActive;
    limitCloseRawPending= limitCloseActive;
    stopRawPending      = stopPressed;
    
    Serial.println("[SAFETY] Manager initialized");
}

void SafetyManager::update() {
    uint32_t now = millis();
    
    obstacleActivePrev   = obstacleActive;
    limitOpenActivePrev  = limitOpenActive;
    limitCloseActivePrev = limitCloseActive;
    stopPressedPrev      = stopPressed;
    
    // Watchdog check
    if (!watchdogOk && (now - lastWatchdogFeedMs < watchdogTimeoutMs)) {
        watchdogOk = true;
        clearFault();  // recover from watchdog fault, dispatch FAULT_CLEARED
    } else if (watchdogOk && (now - lastWatchdogFeedMs >= watchdogTimeoutMs)) {
        watchdogOk = false;
        setFault(SAFETY_FAULT_WATCHDOG);
    }
    
    // Validate limits
    if (!validateLimits() && currentFault == SAFETY_FAULT_NONE) {
        setFault(SAFETY_FAULT_LIMIT_INVALID);
    } else if (validateLimits() && currentFault == SAFETY_FAULT_LIMIT_INVALID) {
        clearFault();
    }
}

// ---------------------------------------------------------------------------
//  FIX B-05 — correct debounce (sticky-timer pattern)
//
//  Rule: when rawActive differs from the last accepted (stable) state,
//        record the time of the first such change (lastXxxChangeMs) and
//        wait debounceMs before accepting it.  If the raw signal bounces
//        back before debounceMs expires, reset the timer.
// ---------------------------------------------------------------------------

void SafetyManager::updateObstacle(bool rawActive) {
    uint32_t now = millis();

    if (rawActive != obstacleRawPending) {
        // Raw signal changed — restart the stability timer
        obstacleRawPending   = rawActive;
        lastObstacleChangeMs = now;
        return;                     // not stable yet
    }

    // Raw is stable — check if it's been stable long enough AND differs from accepted
    if (rawActive != obstacleActive &&
        (now - lastObstacleChangeMs) >= obstacleDebounceMs) {
        bool rising      = rawActive && !obstacleActive;
        obstacleActive   = rawActive;
        if (rising) {
            obstacleTriggerCount++;
            handleObstacleEdge(true);
        } else {
            handleObstacleEdge(false);
        }
    }
}

void SafetyManager::updateLimitOpen(bool rawActive) {
    uint32_t now = millis();

    if (rawActive != limitOpenRawPending) {
        limitOpenRawPending   = rawActive;
        lastLimitOpenChangeMs = now;
        return;
    }

    if (rawActive != limitOpenActive &&
        (now - lastLimitOpenChangeMs) >= limitDebounceMs) {
        bool rising      = rawActive && !limitOpenActive;
        limitOpenActive  = rawActive;
        if (rising) {
            handleLimitOpenEdge(true);
        } else {
            handleLimitOpenEdge(false);
        }
    }
}

void SafetyManager::updateLimitClose(bool rawActive) {
    uint32_t now = millis();

    if (rawActive != limitCloseRawPending) {
        limitCloseRawPending   = rawActive;
        lastLimitCloseChangeMs = now;
        return;
    }

    if (rawActive != limitCloseActive &&
        (now - lastLimitCloseChangeMs) >= limitDebounceMs) {
        bool rising       = rawActive && !limitCloseActive;
        limitCloseActive  = rawActive;
        if (rising) {
            handleLimitCloseEdge(true);
        } else {
            handleLimitCloseEdge(false);
        }
    }
}

void SafetyManager::updateStopButton(bool rawActive) {
    uint32_t now = millis();

    if (rawActive != stopRawPending) {
        stopRawPending   = rawActive;
        lastStopChangeMs = now;
        return;
    }

    if (rawActive != stopPressed &&
        (now - lastStopChangeMs) >= stopDebounceMs) {
        bool rising  = rawActive && !stopPressed;
        stopPressed  = rawActive;
        if (rising) {
            handleStopEdge(true);
        } else {
            handleStopEdge(false);
        }
    }
}

void SafetyManager::updateMotorFault(bool faultActive) {
    if (faultActive && currentFault == SAFETY_FAULT_NONE) {
        setFault(SAFETY_FAULT_MOTOR_FAULT);
    } else if (!faultActive && currentFault == SAFETY_FAULT_MOTOR_FAULT) {
        clearFault();
    }
}

void SafetyManager::updateTelemetryStatus(bool valid) {
    if (!valid && currentFault == SAFETY_FAULT_NONE) {
        setFault(SAFETY_FAULT_TELEMETRY_LOST);
    } else if (valid && currentFault == SAFETY_FAULT_TELEMETRY_LOST) {
        clearFault();
    }
}

void SafetyManager::updateCurrent(float currentA) {
    uint32_t now = millis();
    
    if (currentA >= overCurrentThresholdA) {
        if (overCurrentStartMs == 0) {
            overCurrentStartMs = now;
        } else if (now - overCurrentStartMs >= overCurrentDurationMs) {
            lastOverCurrentA  = currentA;
            lastOverCurrentMs = now;
            setFault(SAFETY_FAULT_OVER_CURRENT);
        }
    } else {
        overCurrentStartMs = 0;
    }
}

bool SafetyManager::isSafeToMove() const {
    if (currentFault != SAFETY_FAULT_NONE) return false;
    if (obstacleActive)  return false;
    if (stopPressed)     return false;
    if (!watchdogOk)     return false;
    if (!validateLimits()) return false;
    return true;
}

void SafetyManager::clearFault() {
    if (currentFault != SAFETY_FAULT_NONE) {
        currentFault  = SAFETY_FAULT_NONE;
        faultLatched  = false;
        dispatchEvent(SAFETY_EVENT_FAULT_CLEARED);
        Serial.printf("[SAFETY] Fault cleared: %s\n", getFaultString());
    }
}

void SafetyManager::setFault(SafetyFaultType fault) {
    if (currentFault != fault) {
        currentFault      = fault;
        faultStartTimeMs  = millis();
        lastFaultMs       = faultStartTimeMs;
        faultCount++;
        faultLatched      = true;
        dispatchEvent(SAFETY_EVENT_FAULT_DETECTED);
        Serial.printf("[SAFETY] Fault set: %s\n", getFaultString());
    }
}

const char* SafetyManager::getFaultString() const {
    switch (currentFault) {
        case SAFETY_FAULT_NONE:              return "NONE";
        case SAFETY_FAULT_OBSTACLE:          return "OBSTACLE";
        case SAFETY_FAULT_LIMIT_INVALID:     return "LIMIT_INVALID";
        case SAFETY_FAULT_TELEMETRY_LOST:    return "TELEMETRY_LOST";
        case SAFETY_FAULT_MOTOR_FAULT:       return "MOTOR_FAULT";
        case SAFETY_FAULT_OVER_CURRENT:      return "OVER_CURRENT";
        case SAFETY_FAULT_WATCHDOG:          return "WATCHDOG";
        case SAFETY_FAULT_COMMUNICATION:     return "COMMUNICATION";
        default:                             return "UNKNOWN";
    }
}

void SafetyManager::setCallback(SafetyCallback cb, void* ctx) {
    callback    = cb;
    callbackCtx = ctx;
}

void SafetyManager::feedWatchdog() {
    lastWatchdogFeedMs = millis();
    if (!watchdogOk) {
        watchdogOk = true;
    }
}

bool SafetyManager::checkWatchdog(uint32_t timeoutMs) {
    watchdogTimeoutMs = timeoutMs;
    return watchdogOk;
}

void SafetyManager::handleObstacleEdge(bool rising) {
    if (rising) {
        dispatchEvent(SAFETY_EVENT_OBSTACLE_DETECTED);
        setFault(SAFETY_FAULT_OBSTACLE);
    } else {
        dispatchEvent(SAFETY_EVENT_OBSTACLE_CLEARED);
        if (currentFault == SAFETY_FAULT_OBSTACLE) {
            clearFault();
        }
    }
}

void SafetyManager::handleLimitOpenEdge(bool rising) {
    if (rising) {
        dispatchEvent(SAFETY_EVENT_LIMIT_OPEN_HIT);
    }
}

void SafetyManager::handleLimitCloseEdge(bool rising) {
    if (rising) {
        dispatchEvent(SAFETY_EVENT_LIMIT_CLOSE_HIT);
    }
}

void SafetyManager::handleStopEdge(bool rising) {
    if (rising) {
        dispatchEvent(SAFETY_EVENT_STOP_PRESSED);
    }
}

void SafetyManager::dispatchEvent(SafetyEventType event) {
    if (callback) {
        callback(event, callbackCtx);
    }
    
#if defined(GATE_DEBUG_SAFETY)
    const char* eventStr;
    switch (event) {
        case SAFETY_EVENT_OBSTACLE_DETECTED: eventStr = "OBSTACLE_DETECTED"; break;
        case SAFETY_EVENT_OBSTACLE_CLEARED:  eventStr = "OBSTACLE_CLEARED";  break;
        case SAFETY_EVENT_LIMIT_OPEN_HIT:    eventStr = "LIMIT_OPEN_HIT";    break;
        case SAFETY_EVENT_LIMIT_CLOSE_HIT:   eventStr = "LIMIT_CLOSE_HIT";   break;
        case SAFETY_EVENT_STOP_PRESSED:      eventStr = "STOP_PRESSED";       break;
        case SAFETY_EVENT_FAULT_DETECTED:    eventStr = "FAULT_DETECTED";     break;
        case SAFETY_EVENT_FAULT_CLEARED:     eventStr = "FAULT_CLEARED";      break;
        case SAFETY_EVENT_WATCHDOG_TIMEOUT:  eventStr = "WATCHDOG_TIMEOUT";   break;
        case SAFETY_EVENT_OVER_CURRENT:      eventStr = "OVER_CURRENT";       break;
        default:                             eventStr = "UNKNOWN";             break;
    }
    Serial.printf("[SAFETY] Event: %s\n", eventStr);
#endif
}

bool SafetyManager::validateLimits() const {
    return !(limitOpenActive && limitCloseActive);
}
