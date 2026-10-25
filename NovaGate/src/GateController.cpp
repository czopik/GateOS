#include "GateController.h"

GateController::GateController(const ConfigManager& config, MotorController& m, PositionTracker& p)
    : cfg(config), motor(m), pos(p) {}

void GateController::begin() {
    pinMode(cfg.getConfig().pin_limit_open, INPUT_PULLUP);
    pinMode(cfg.getConfig().pin_limit_close, INPUT_PULLUP);
    pinMode(cfg.getConfig().pin_photocell, INPUT_PULLUP);
    setState(GateState::STOPPED);
}

void GateController::loop() {
    handleSafety();
    handleMotion();
}

bool GateController::readLimitOpen() {
    return digitalRead(cfg.getConfig().pin_limit_open) == LOW; // Active low
}

bool GateController::readLimitClose() {
    return digitalRead(cfg.getConfig().pin_limit_close) == LOW; // Active low
}

bool GateController::readPhotocell() {
    return digitalRead(cfg.getConfig().pin_photocell) == LOW; // Active low means blocked
}

void GateController::setState(GateState newState) {
    state = newState;
    stateStartMs = millis();

    if (state == GateState::STOPPED || state == GateState::ERROR) {
        motor.stopHard();
    }
}

GateState GateController::getState() const { return state; }
const char* GateController::getStateString() const {
    switch(state) {
        case GateState::STOPPED: return "STOPPED";
        case GateState::OPENING: return "OPENING";
        case GateState::CLOSING: return "CLOSING";
        case GateState::HOMING_OPEN: return "HOMING_OPEN";
        case GateState::HOMING_CLOSE: return "HOMING_CLOSE";
        case GateState::ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

String GateController::getLastError() const { return lastError; }

void GateController::open() {
    if (state == GateState::ERROR) return;
    if (!pos.isPositionKnown()) {
        home();
        return;
    }
    if (readLimitOpen()) return; // Already open

    setState(GateState::OPENING);
}

void GateController::close() {
    if (state == GateState::ERROR) return;
    if (!pos.isPositionKnown()) {
        home();
        return;
    }
    if (readLimitClose()) return; // Already closed
    if (readPhotocell()) return; // Blocked

    setState(GateState::CLOSING);
}

void GateController::toggle() {
    if (state == GateState::OPENING || state == GateState::CLOSING || state == GateState::HOMING_OPEN || state == GateState::HOMING_CLOSE) {
        stop();
    } else {
        if (pos.getPositionPercent() > 50.0f) {
            close();
        } else {
            open();
        }
    }
}

void GateController::stop() {
    setState(GateState::STOPPED);
}

void GateController::home() {
    if (state == GateState::ERROR) return;
    setState(GateState::HOMING_CLOSE);
}

void GateController::handleSafety() {
    if (state == GateState::ERROR) return;

    // Check overcurrent
    if (motor.getTelemetry().online) {
        // batVoltage in hover is 100 * V
        // Actually hover telemetries usually send current in other fields, but if not available we rely on RPM stall detection
        // For this task, if rpm command is high but actual is 0 for > 1s, it's a stall (overcurrent/blocked)
        if (abs(motor.getCurrentSpeed()) > 50 && abs(motor.getTelemetry().rpm1) < 5 && millis() - stateStartMs > 1500) {
            lastError = "STALL_DETECTED";
            setState(GateState::ERROR);
            return;
        }
    }

    // Check photocell during close
    if (state == GateState::CLOSING && readPhotocell()) {
        setState(GateState::OPENING); // Auto reverse
        return;
    }
}

void GateController::handleMotion() {
    int targetRpm = 0;
    const auto& c = cfg.getConfig();

    switch(state) {
        case GateState::STOPPED:
        case GateState::ERROR:
            targetRpm = 0;
            break;

        case GateState::OPENING:
            if (readLimitOpen()) {
                pos.resetToOpened();
                setState(GateState::STOPPED);
            } else {
                float distLeft = c.gate_length_m - pos.getPositionMeters();
                if (distLeft < c.soft_stop_distance_m) {
                    targetRpm = c.min_speed_rpm;
                } else {
                    targetRpm = c.max_speed_rpm;
                }
            }
            break;

        case GateState::CLOSING:
            if (readLimitClose()) {
                pos.resetToClosed();
                setState(GateState::STOPPED);
            } else {
                float distLeft = pos.getPositionMeters();
                if (distLeft < c.soft_stop_distance_m) {
                    targetRpm = -c.min_speed_rpm;
                } else {
                    targetRpm = -c.max_speed_rpm;
                }
            }
            break;

        case GateState::HOMING_CLOSE:
            if (readLimitClose()) {
                pos.resetToClosed();
                setState(GateState::HOMING_OPEN);
            } else {
                targetRpm = -c.min_speed_rpm;
                if (millis() - stateStartMs > 60000) {
                    lastError = "HOMING_TIMEOUT";
                    setState(GateState::ERROR);
                }
            }
            break;

        case GateState::HOMING_OPEN:
            if (readLimitOpen()) {
                pos.resetToOpened();
                // Optional: measure length here = pos.getPositionMeters()
                setState(GateState::STOPPED);
            } else {
                targetRpm = c.min_speed_rpm;
                if (millis() - stateStartMs > 60000) {
                    lastError = "HOMING_TIMEOUT";
                    setState(GateState::ERROR);
                }
            }
            break;
    }

    motor.setTargetSpeed(targetRpm);
}
