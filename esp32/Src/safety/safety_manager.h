#pragma once
#include <Arduino.h>
#include "config_manager.h"

// FIX B-05: rawPending fields added to support correct sticky-timer debounce.

enum SafetyFaultType {
    SAFETY_FAULT_NONE           = 0,
    SAFETY_FAULT_OBSTACLE       = 1,
    SAFETY_FAULT_LIMIT_INVALID  = 2,
    SAFETY_FAULT_TELEMETRY_LOST = 3,
    SAFETY_FAULT_MOTOR_FAULT    = 4,
    SAFETY_FAULT_OVER_CURRENT   = 5,
    SAFETY_FAULT_WATCHDOG       = 6,
    SAFETY_FAULT_COMMUNICATION  = 7
};

enum SafetyEventType {
    SAFETY_EVENT_OBSTACLE_DETECTED = 0,
    SAFETY_EVENT_OBSTACLE_CLEARED,
    SAFETY_EVENT_LIMIT_OPEN_HIT,
    SAFETY_EVENT_LIMIT_CLOSE_HIT,
    SAFETY_EVENT_STOP_PRESSED,
    SAFETY_EVENT_FAULT_DETECTED,
    SAFETY_EVENT_FAULT_CLEARED,
    SAFETY_EVENT_WATCHDOG_TIMEOUT,
    SAFETY_EVENT_OVER_CURRENT
};

using SafetyCallback = void(*)(SafetyEventType event, void* ctx);

class SafetyManager {
public:
    SafetyManager();

    void begin(ConfigManager* cfg);
    void update();

    void updateObstacle(bool rawActive);
    void updateLimitOpen(bool rawActive);
    void updateLimitClose(bool rawActive);
    void updateStopButton(bool rawActive);
    void updateMotorFault(bool faultActive);
    void updateTelemetryStatus(bool valid);
    void updateCurrent(float currentA);

    bool isSafeToMove() const;
    bool isObstacleActive() const    { return obstacleActive; }
    bool isLimitOpenActive() const   { return limitOpenActive; }
    bool isLimitCloseActive() const  { return limitCloseActive; }
    bool isStopPressed() const       { return stopPressed; }

    SafetyFaultType getCurrentFault() const { return currentFault; }
    const char* getFaultString() const;
    bool isFaultLatched() const  { return faultLatched; }
    int  getFaultCount() const   { return faultCount; }

    void setCallback(SafetyCallback cb, void* ctx);
    void feedWatchdog();
    bool checkWatchdog(uint32_t timeoutMs);

    int  getObstacleTriggerCount() const { return obstacleTriggerCount; }
    float getLastOverCurrentA() const    { return lastOverCurrentA; }
    uint32_t getLastOverCurrentMs() const { return lastOverCurrentMs; }

private:
    ConfigManager* config;

    // Accepted (debounced) states
    bool obstacleActive;
    bool obstacleActivePrev;
    bool limitOpenActive;
    bool limitOpenActivePrev;
    bool limitCloseActive;
    bool limitCloseActivePrev;
    bool stopPressed;
    bool stopPressedPrev;

    // FIX B-05: pending raw values (last seen raw before acceptance)
    bool obstacleRawPending;
    bool limitOpenRawPending;
    bool limitCloseRawPending;
    bool stopRawPending;

    uint32_t obstacleDebounceMs;
    uint32_t limitDebounceMs;
    uint32_t stopDebounceMs;

    // Timestamps of the LAST RAW CHANGE (reset on every raw toggle)
    uint32_t lastObstacleChangeMs;
    uint32_t lastLimitOpenChangeMs;
    uint32_t lastLimitCloseChangeMs;
    uint32_t lastStopChangeMs;

    SafetyFaultType currentFault;
    uint32_t faultStartTimeMs;
    uint32_t lastFaultMs;
    int      faultCount;
    bool     faultLatched;

    uint32_t lastWatchdogFeedMs;
    bool     watchdogOk;
    uint32_t watchdogTimeoutMs;

    float    overCurrentThresholdA;
    uint32_t overCurrentDurationMs;
    uint32_t overCurrentStartMs;
    float    lastOverCurrentA;
    uint32_t lastOverCurrentMs;

    SafetyCallback callback;
    void*          callbackCtx;

    int obstacleTriggerCount;

    void setFault(SafetyFaultType fault);
    void clearFault();
    bool validateLimits() const;

    void handleObstacleEdge(bool rising);
    void handleLimitOpenEdge(bool rising);
    void handleLimitCloseEdge(bool rising);
    void handleStopEdge(bool rising);
    void dispatchEvent(SafetyEventType event);
};
