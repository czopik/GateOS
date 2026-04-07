/**
 * @file safety_manager.cpp
 * @brief Centralized safety management implementation
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
        limitDebounceMs = 30;  // Could be configurable
        stopDebounceMs = 30;
        
        overCurrentThresholdA = cfg->motorConfig.overCurrentThreshold > 0
            ? cfg->motorConfig.overCurrentThreshold : 10.0f;
        overCurrentDurationMs = cfg->motorConfig.overCurrentDurationMs > 0
            ? cfg->motorConfig.overCurrentDurationMs : 500;
    }
    
    lastWatchdogFeedMs = millis();
    watchdogOk = true;
    
    Serial.println("[SAFETY] Manager initialized");
}

void SafetyManager::update() {
    uint32_t now = millis();
    
    // Update previous states
    obstacleActivePrev = obstacleActive;
    limitOpenActivePrev = limitOpenActive;
    limitCloseActivePrev = limitCloseActive;
    stopPressedPrev = stopPressed;
    
    // Check watchdog
    if (!watchdogOk && (now - lastWatchdogFeedMs < watchdogTimeoutMs)) {
        watchdogOk = true;
        dispatchEvent(SAFETY_EVENT_WATCHDOG_TIMEOUT);  // Cleared
    } else if (watchdogOk && (now - lastWatchdogFeedMs >= watchdogTimeoutMs)) {
        watchdogOk = false;
        setFault(SAFETY_FAULT_WATCHDOG);
    }
    
    // Validate limits (both active = invalid state)
    if (!validateLimits() && currentFault == SAFETY_FAULT_NONE) {
        setFault(SAFETY_FAULT_LIMIT_INVALID);
    } else if (validateLimits() && currentFault == SAFETY_FAULT_LIMIT_INVALID) {
        clearFault();
    }
}

void SafetyManager::updateObstacle(bool rawActive) {
    uint32_t now = millis();
    
    // Simple debouncing
    if (rawActive != obstacleActive) {
        if (now - lastObstacleChangeMs >= obstacleDebounceMs) {
            bool rising = rawActive && !obstacleActive;
            obstacleActive = rawActive;
            lastObstacleChangeMs = now;
            
            if (rising) {
                obstacleTriggerCount++;
                handleObstacleEdge(true);
            } else {
                handleObstacleEdge(false);
            }
        }
    } else {
        lastObstacleChangeMs = now;
    }
}

void SafetyManager::updateLimitOpen(bool rawActive) {
    uint32_t now = millis();
    
    if (rawActive != limitOpenActive) {
        if (now - lastLimitOpenChangeMs >= limitDebounceMs) {
            bool rising = rawActive && !limitOpenActive;
            limitOpenActive = rawActive;
            lastLimitOpenChangeMs = now;
            
            if (rising) {
                handleLimitOpenEdge(true);
            } else {
                handleLimitOpenEdge(false);
            }
        }
    } else {
        lastLimitOpenChangeMs = now;
    }
}

void SafetyManager::updateLimitClose(bool rawActive) {
    uint32_t now = millis();
    
    if (rawActive != limitCloseActive) {
        if (now - lastLimitCloseChangeMs >= limitDebounceMs) {
            bool rising = rawActive && !limitCloseActive;
            limitCloseActive = rawActive;
            lastLimitCloseChangeMs = now;
            
            if (rising) {
                handleLimitCloseEdge(true);
            } else {
                handleLimitCloseEdge(false);
            }
        }
    } else {
        lastLimitCloseChangeMs = now;
    }
}

void SafetyManager::updateStopButton(bool rawActive) {
    uint32_t now = millis();
    
    if (rawActive != stopPressed) {
        if (now - lastStopChangeMs >= stopDebounceMs) {
            bool rising = rawActive && !stopPressed;
            stopPressed = rawActive;
            lastStopChangeMs = now;
            
            if (rising) {
                handleStopEdge(true);
            } else {
                handleStopEdge(false);
            }
        }
    } else {
        lastStopChangeMs = now;
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
            // Over-current confirmed
            lastOverCurrentA = currentA;
            lastOverCurrentMs = now;
            setFault(SAFETY_FAULT_OVER_CURRENT);
        }
    } else {
        overCurrentStartMs = 0;
    }
}

bool SafetyManager::isSafeToMove() const {
    // Not safe if:
    // - Active fault
    // - Obstacle detected
    // - Stop pressed
    // - Watchdog timeout
    // - Invalid limits
    
    if (currentFault != SAFETY_FAULT_NONE) return false;
    if (obstacleActive) return false;
    if (stopPressed) return false;
    if (!watchdogOk) return false;
    if (!validateLimits()) return false;
    
    return true;
}

void SafetyManager::clearFault() {
    if (currentFault != SAFETY_FAULT_NONE) {
        SafetyFaultType prev = currentFault;
        currentFault = SAFETY_FAULT_NONE;
        faultLatched = false;
        dispatchEvent(SAFETY_EVENT_FAULT_CLEARED);
        Serial.printf("[SAFETY] Fault cleared: %s\n", getFaultString());
    }
}

void SafetyManager::setFault(SafetyFaultType fault) {
    if (currentFault != fault) {
        currentFault = fault;
        faultStartTimeMs = millis();
        lastFaultMs = faultStartTimeMs;
        faultCount++;
        faultLatched = true;
        dispatchEvent(SAFETY_EVENT_FAULT_DETECTED);
        Serial.printf("[SAFETY] Fault set: %s\n", getFaultString());
    }
}

const char* SafetyManager::getFaultString() const {
    switch (currentFault) {
        case SAFETY_FAULT_NONE: return "NONE";
        case SAFETY_FAULT_OBSTACLE: return "OBSTACLE";
        case SAFETY_FAULT_LIMIT_INVALID: return "LIMIT_INVALID";
        case SAFETY_FAULT_TELEMETRY_LOST: return "TELEMETRY_LOST";
        case SAFETY_FAULT_MOTOR_FAULT: return "MOTOR_FAULT";
        case SAFETY_FAULT_OVER_CURRENT: return "OVER_CURRENT";
        case SAFETY_FAULT_WATCHDOG: return "WATCHDOG";
        case SAFETY_FAULT_COMMUNICATION: return "COMMUNICATION";
        default: return "UNKNOWN";
    }
}

void SafetyManager::setCallback(SafetyCallback cb, void* ctx) {
    callback = cb;
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
        case SAFETY_EVENT_OBSTACLE_CLEARED: eventStr = "OBSTACLE_CLEARED"; break;
        case SAFETY_EVENT_LIMIT_OPEN_HIT: eventStr = "LIMIT_OPEN_HIT"; break;
        case SAFETY_EVENT_LIMIT_CLOSE_HIT: eventStr = "LIMIT_CLOSE_HIT"; break;
        case SAFETY_EVENT_STOP_PRESSED: eventStr = "STOP_PRESSED"; break;
        case SAFETY_EVENT_FAULT_DETECTED: eventStr = "FAULT_DETECTED"; break;
        case SAFETY_EVENT_FAULT_CLEARED: eventStr = "FAULT_CLEARED"; break;
        case SAFETY_EVENT_WATCHDOG_TIMEOUT: eventStr = "WATCHDOG_TIMEOUT"; break;
        case SAFETY_EVENT_OVER_CURRENT: eventStr = "OVER_CURRENT"; break;
        default: eventStr = "UNKNOWN"; break;
    }
    Serial.printf("[SAFETY] Event: %s\n", eventStr);
#endif
}

bool SafetyManager::validateLimits() const {
    // Both limits active simultaneously is invalid
    return !(limitOpenActive && limitCloseActive);
}
