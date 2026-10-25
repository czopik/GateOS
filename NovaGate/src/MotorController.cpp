#include "MotorController.h"

#define HOVER_START_FRAME 0xABCD

// Simple packet structure for serial comms matching hoverboard
typedef struct {
   uint16_t start;
   int16_t  steer;
   int16_t  speed;
   uint16_t checksum;
} SerialCommand;

typedef struct {
   uint16_t start;
   int16_t  cmd1;
   int16_t  cmd2;
   int16_t  speedR_meas;
   int16_t  speedL_meas;
   int16_t  batVoltage;
   int16_t  boardTemp;
   uint16_t cmdLed;
   uint16_t checksum;
} SerialFeedback;

MotorController::MotorController(const ConfigManager& configManager) : cfg(configManager) {
    telemetry.online = false;
}

void MotorController::begin(HardwareSerial& serial, int rxPin, int txPin) {
    hoverSerial = &serial;
    hoverSerial->begin(115200, SERIAL_8N1, rxPin, txPin);
}

void MotorController::setTargetSpeed(int rpm) {
    currentTargetRpm = rpm;
}

void MotorController::stopHard() {
    currentTargetRpm = 0;
    actualCommandRpm = 0;
    sendCommand(0, 0);
}

void MotorController::loop() {
    // 1. Read telemetry
    while (hoverSerial->available()) {
        rxBuffer[rxIndex++] = hoverSerial->read();
        if (rxIndex >= sizeof(SerialFeedback)) {
            parseTelemetry();
            rxIndex = 0; // simple unbuffered reset, should use better sliding window if needed
        }
    }

    // Smooth ramp logic
    if (millis() - lastSendMs > 20) { // Send every 20ms = 50Hz

        int step = (cfg.getConfig().max_speed_rpm * 20) / max(1, cfg.getConfig().ramp_time_ms);
        if (step < 1) step = 1;

        if (actualCommandRpm < currentTargetRpm) {
            actualCommandRpm += step;
            if (actualCommandRpm > currentTargetRpm) actualCommandRpm = currentTargetRpm;
        } else if (actualCommandRpm > currentTargetRpm) {
            actualCommandRpm -= step;
            if (actualCommandRpm < currentTargetRpm) actualCommandRpm = currentTargetRpm;
        }

        sendCommand(0, actualCommandRpm);
        lastSendMs = millis();
    }

    if (millis() - telemetry.lastUpdateMs > 500) {
        telemetry.online = false;
    }
}

int MotorController::getCurrentSpeed() const {
    return actualCommandRpm;
}

const HoverTelemetry& MotorController::getTelemetry() const {
    return telemetry;
}

void MotorController::sendCommand(int16_t steer, int16_t speed) {
    SerialCommand cmd;
    cmd.start = HOVER_START_FRAME;
    cmd.steer = steer;
    cmd.speed = speed;
    cmd.checksum = (uint16_t)(cmd.start ^ cmd.steer ^ cmd.speed);
    hoverSerial->write((uint8_t*)&cmd, sizeof(cmd));
}

void MotorController::parseTelemetry() {
    SerialFeedback* fb = (SerialFeedback*)rxBuffer;
    if (fb->start == HOVER_START_FRAME) {
        uint16_t checksum = (uint16_t)(fb->start ^ fb->cmd1 ^ fb->cmd2 ^ fb->speedR_meas ^ fb->speedL_meas ^ fb->batVoltage ^ fb->boardTemp ^ fb->cmdLed);
        if (checksum == fb->checksum) {
            telemetry.rpm1 = fb->speedR_meas;
            telemetry.rpm2 = fb->speedL_meas;
            telemetry.batVoltage = fb->batVoltage;
            telemetry.boardTemp = fb->boardTemp;
            telemetry.cmdLed = fb->cmdLed;
            telemetry.lastUpdateMs = millis();
            telemetry.online = true;
        }
    } else {
        // shift buffer if desync
        for (int i=1; i<rxIndex; i++) rxBuffer[i-1] = rxBuffer[i];
        rxIndex--;
    }
}
