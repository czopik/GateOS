#pragma once
#include <Arduino.h>
#include "ConfigManager.h"
#include "MotorController.h"
#include "PositionTracker.h"

enum class GateState {
    STOPPED,
    OPENING,
    CLOSING,
    HOMING_OPEN,
    HOMING_CLOSE,
    ERROR
};

class GateController {
public:
    GateController(const ConfigManager& cfg, MotorController& motor, PositionTracker& pos);
    void begin();
    void loop();

    void open();
    void close();
    void toggle();
    void stop();
    void home();

    GateState getState() const;
    const char* getStateString() const;
    String getLastError() const;

private:
    const ConfigManager& cfg;
    MotorController& motor;
    PositionTracker& pos;

    GateState state = GateState::STOPPED;
    String lastError = "";

    unsigned long stateStartMs = 0;

    bool readLimitOpen();
    bool readLimitClose();
    bool readPhotocell();

    void setState(GateState newState);
    void handleMotion();
    void handleSafety();
};
