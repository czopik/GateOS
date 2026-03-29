#pragma once

#include <Arduino.h>
#include "config_manager.h"

struct HoverTelemetry {
  int dir = 0;
  int rpm = 0;
  long distMm = 0;
  long rawBat = -1;
  // raw battery in centi-volts when provided by new firmware (bat_cV)
  int bat_cV = -1;
  float batV = 0.0f;
  uint8_t batScale = 0; // 0=unknown, 1=V, 2=deciV, 3=mV
  bool batValid = false;

  // current in centi-amperes (iA * 100). -1 = unknown
  int iA_x100 = -1;

  // armed flag reported by hoverboard telemetry
  bool armed = false;
  int cmd_age_ms = -1; // age of last valid command in ms (reported by hoverboard)
  int fault = 0;
  unsigned long lastTelMs = 0;
};

class HoverUartDriver {
public:
  HoverUartDriver();

  void configure(const HoverUartConfig& cfg);
  void begin();
  void update(uint32_t nowMs);
  void stop();
  // Emergency stop: bypass ramp and send immediate 0 command
  void emergencyStop();
  void setTargetSpeed(int16_t speed);
  // ASCII control commands
  void arm();
  void disarm();
  void zero();
  void get();

  bool isEnabled() const { return enabled; }
  int16_t getTargetSpeed() const { return targetSpeed; }
  int16_t getCurrentSpeed() const { return currentSpeed; }
  const HoverTelemetry& telemetry() const { return tel; }
  bool faultActive() const { return tel.fault != 0; }
  bool telemetryTimedOut(uint32_t nowMs, uint32_t timeoutMs) const;

private:
  void sendCommand(int16_t steer, int16_t speed);
  void updateRamp();
  void handleRx();

  HoverUartConfig cfg;
  bool enabled = false;
  int16_t targetSpeed = 0;
  int16_t currentSpeed = 0;
  int16_t steer = 0;
  uint32_t lastKeepaliveMs = 0;

  HoverTelemetry tel;
  char lineBuf[160] = {0};
  uint16_t lineLen = 0;
  // requested armed state by ESP32 (true if we asked to arm)
  bool armedRequested = false;
  unsigned long lastArmCmdMs = 0;
  unsigned long lastDisarmCmdMs = 0;
  // helper timestamps to decide disarm after stop
  unsigned long lastTargetNonZeroMs = 0;
  unsigned long lastZeroStopMs = 0;
};