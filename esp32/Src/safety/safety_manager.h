#pragma once
/**
 * @file safety_manager.h
 * @brief Centralized safety management for gate controller
 * 
 * Handles:
 * - Photocell/obstacle detection with debouncing
 * - Limit switch monitoring
 * - Emergency stop
 * - Fault states and recovery
 * - Watchdog monitoring
 */

#include <Arduino.h>
#include "config_manager.h"

// Safety event types
typedef enum {
    SAFETY_EVENT_NONE = 0,
    SAFETY_EVENT_OBSTACLE_DETECTED,
    SAFETY_EVENT_OBSTACLE_CLEARED,
    SAFETY_EVENT_LIMIT_OPEN_HIT,
    SAFETY_EVENT_LIMIT_CLOSE_HIT,
    SAFETY_EVENT_STOP_PRESSED,
    SAFETY_EVENT_FAULT_DETECTED,
    SAFETY_EVENT_FAULT_CLEARED,
    SAFETY_EVENT_WATCHDOG_TIMEOUT,
    SAFETY_EVENT_OVER_CURRENT
} SafetyEventType;

// Fault types
typedef enum {
    SAFETY_FAULT_NONE = 0,
    SAFETY_FAULT_OBSTACLE,
    SAFETY_FAULT_LIMIT_INVALID,
    SAFETY_FAULT_TELEMETRY_LOST,
    SAFETY_FAULT_MOTOR_FAULT,
    SAFETY_FAULT_OVER_CURRENT,
    SAFETY_FAULT_WATCHDOG,
    SAFETY_FAULT_COMMUNICATION
} SafetyFaultType;

// Safety state callback
typedef void (*SafetyCallback)(SafetyEventType event, void* ctx);

class SafetyManager {
public:
    SafetyManager();
    
    void begin(ConfigManager* cfg);
    void update();
    
    // Input state updates (called from main loop or ISRs)
    void updateObstacle(bool active);
    void updateLimitOpen(bool active);
    void updateLimitClose(bool active);
    void updateStopButton(bool active);
    void updateMotorFault(bool faultActive);
    void updateTelemetryStatus(bool valid);
    void updateCurrent(float currentA);
    
    // State queries
    bool isSafeToMove() const;
    bool hasActiveFault() const { return currentFault != SAFETY_FAULT_NONE; }
    SafetyFaultType getActiveFault() const { return currentFault; }
    bool isObstacleActive() const { return obstacleActive; }
    bool isLimitOpenActive() const { return limitOpenActive; }
    bool isLimitCloseActive() const { return limitCloseActive; }
    bool isStopPressed() const { return stopPressed; }
    bool isWatchdogOk() const { return watchdogOk; }
    
    // Fault management
    void clearFault();
    void setFault(SafetyFaultType fault);
    const char* getFaultString() const;
    
    // Callbacks
    void setCallback(SafetyCallback cb, void* ctx);
    
    // Watchdog
    void feedWatchdog();
    bool checkWatchdog(uint32_t timeoutMs);
    
    // Statistics
    uint32_t getObstacleTriggers() const { return obstacleTriggerCount; }
    uint32_t getFaultCount() const { return faultCount; }
    uint32_t getLastFaultMs() const { return lastFaultMs; }
    float getLastOverCurrent() const { return lastOverCurrentA; }
    uint32_t getLastOverCurrentMs() const { return lastOverCurrentMs; }

private:
    void handleObstacleEdge(bool rising);
    void handleLimitOpenEdge(bool rising);
    void handleLimitCloseEdge(bool rising);
    void handleStopEdge(bool rising);
    void dispatchEvent(SafetyEventType event);
    bool validateLimits() const;
    
    ConfigManager* config;
    
    // Input states (debounced)
    bool obstacleActive;
    bool obstacleActivePrev;
    bool limitOpenActive;
    bool limitOpenActivePrev;
    bool limitCloseActive;
    bool limitCloseActivePrev;
    bool stopPressed;
    bool stopPressedPrev;
    
    // Debounce timing
    uint32_t obstacleDebounceMs;
    uint32_t limitDebounceMs;
    uint32_t stopDebounceMs;
    uint32_t lastObstacleChangeMs;
    uint32_t lastLimitOpenChangeMs;
    uint32_t lastLimitCloseChangeMs;
    uint32_t lastStopChangeMs;
    
    // Fault state
    SafetyFaultType currentFault;
    uint32_t faultStartTimeMs;
    uint32_t lastFaultMs;
    uint32_t faultCount;
    bool faultLatched;
    
    // Watchdog
    uint32_t lastWatchdogFeedMs;
    bool watchdogOk;
    uint32_t watchdogTimeoutMs;
    
    // Over-current monitoring
    float overCurrentThresholdA;
    uint32_t overCurrentDurationMs;
    uint32_t overCurrentStartMs;
    float lastOverCurrentA;
    uint32_t lastOverCurrentMs;
    
    // Callbacks
    SafetyCallback callback;
    void* callbackCtx;
    
    // Statistics
    uint32_t obstacleTriggerCount;
};
