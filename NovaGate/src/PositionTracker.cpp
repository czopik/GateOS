#include "PositionTracker.h"

PositionTracker::PositionTracker(const ConfigManager& configManager, const MotorController& motorCtrl)
    : cfg(configManager), motor(motorCtrl) {}

void PositionTracker::begin() {
    lastCalcMs = millis();
}

void PositionTracker::loop() {
    unsigned long now = millis();
    unsigned long dt = now - lastCalcMs;
    if (dt == 0) return;

    // Average RPM from both wheels
    float rpm = (motor.getTelemetry().rpm1 + motor.getTelemetry().rpm2) / 2.0f;

    // speed (m/s) = rpm / 60 * circumference
    float speed_m_s = (rpm / 60.0f) * WHEEL_CIRCUMFERENCE_M;

    float ds = speed_m_s * (dt / 1000.0f);

    // Apply deadband
    if (abs(ds) > 0.0001) {
        currentPosM += ds;
    }

    // Clamp to logical bounds optionally, but allow over-travel to detect limits physically
    if (currentPosM < -0.5f) currentPosM = -0.5f;
    if (currentPosM > cfg.getConfig().gate_length_m + 0.5f) currentPosM = cfg.getConfig().gate_length_m + 0.5f;

    lastCalcMs = now;
}

float PositionTracker::getPositionMeters() const {
    return currentPosM;
}

float PositionTracker::getPositionPercent() const {
    if (cfg.getConfig().gate_length_m <= 0.01f) return 0.0f;
    float pct = (currentPosM / cfg.getConfig().gate_length_m) * 100.0f;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

void PositionTracker::resetToClosed() {
    currentPosM = 0.0f;
    positionKnown = true;
}

void PositionTracker::resetToOpened() {
    currentPosM = cfg.getConfig().gate_length_m;
    positionKnown = true;
}

bool PositionTracker::isPositionKnown() const {
    return positionKnown;
}

void PositionTracker::setPositionKnown(bool known) {
    positionKnown = known;
}
