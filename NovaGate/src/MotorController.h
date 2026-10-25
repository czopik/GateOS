#pragma once
#include <Arduino.h>
#include <HardwareSerial.h>
#include "ConfigManager.h"

struct HoverTelemetry {
    int16_t rpm1;
    int16_t rpm2;
    int16_t batVoltage;
    int16_t boardTemp;
    uint16_t cmdLed;
    uint32_t lastUpdateMs;
    bool online;
};

class MotorController {
public:
    MotorController(const ConfigManager& configManager);
    void begin(HardwareSerial& serial, int rxPin, int txPin);
    void loop();

    void setTargetSpeed(int rpm); // positive = open, negative = close
    void stopHard();

    int getCurrentSpeed() const;
    const HoverTelemetry& getTelemetry() const;

private:
    void sendCommand(int16_t steer, int16_t speed);
    void parseTelemetry();

    const ConfigManager& cfg;
    HardwareSerial* hoverSerial;

    int currentTargetRpm = 0;
    int actualCommandRpm = 0;
    unsigned long lastSendMs = 0;

    HoverTelemetry telemetry;

    // Protocol buffers
    uint8_t rxBuffer[32];
    uint8_t rxIndex = 0;
};
