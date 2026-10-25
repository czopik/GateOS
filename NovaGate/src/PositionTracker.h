#pragma once
#include <Arduino.h>
#include "ConfigManager.h"
#include "MotorController.h"

class PositionTracker {
public:
    PositionTracker(const ConfigManager& configManager, const MotorController& motor);
    void begin();
    void loop();

    float getPositionMeters() const;
    float getPositionPercent() const; // 0.0 (closed) to 1.0 (open)

    void resetToClosed();
    void resetToOpened();

    bool isPositionKnown() const;
    void setPositionKnown(bool known);

private:
    const ConfigManager& cfg;
    const MotorController& motor;

    float currentPosM = 0.0f;
    unsigned long lastCalcMs = 0;
    bool positionKnown = false;

    // Mechanical specs (Hoverboard wheels typically 6.5 inch = 165mm diameter -> 518mm circumference)
    // 90 poles -> 1 revolution. Telemetry gives RPM. We can calculate m/s.
    const float WHEEL_CIRCUMFERENCE_M = 0.518f;
};
