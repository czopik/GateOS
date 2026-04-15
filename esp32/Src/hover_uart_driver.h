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
  // command age reported by hoverboard telemetry (ms). -1 = unknown
  int cmdAgeMs = -1;

  // armed flag reported by hoverboard telemetry
  bool armed = false;
  // charger state from STM32 telemetry: 1=charger present, 0=not present, -1=unknown
  int charger = -1;
  int fault = 0;
  unsigned long lastTelMs = 0;

  // --- Motor wiring / Hall diagnostics (optional; provided by patched STM32 firmware) ---
  // raw hall state 0..7 (ABC bits). -1 = unknown
  int hall = -1;
  // 1 = OK, 0 = FAIL, -1 = unknown
  int diag_ok = -1;
  // 0 = OK, 1 = no edges, 2 = bad hall state (000/111), 3 = bad sequence. -1 = unknown
  int diag_reason = -1;
  // counters (monotonic) from STM32; -1 = unknown
  int diag_edges = -1;
  int diag_bad_state = -1;
  int diag_bad_seq = -1;
  // direction inferred from hall sequence: -1/0/1. 0 = unknown
  int diag_dir = 0;
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
  void setDecelBoost(bool enable);

  bool isEnabled() const { return enabled; }
  int16_t getTargetSpeed() const { return targetSpeed; }
  int16_t getCurrentSpeed() const { return currentSpeed; }
  const HoverTelemetry& telemetry() const { return tel; }
  bool faultActive() const { return tel.fault != 0; }
  bool telemetryTimedOut(uint32_t nowMs, uint32_t timeoutMs) const;

  // RX statistics (useful for diagnosing lost/invalid telemetry)
  uint32_t getRxLines() const { return rxLines; }
  uint32_t getRxTelLines() const { return rxTelLines; }
  uint32_t getRxBadLines() const { return rxBadLines; }

private:
  void sendCommand(int16_t steer, int16_t speed);
  void updateRamp();
  void handleRx();

  HoverUartConfig cfg;
  bool enabled = false;
  int16_t targetSpeed = 0;
  int16_t currentSpeed = 0;
  bool decelBoost = false;
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
  uint32_t rxLines = 0;
  uint32_t rxTelLines = 0;
  uint32_t rxBadLines = 0;
  uint32_t lastStatsMs = 0;
  uint32_t lastTelLines = 0;
  uint32_t lastBadLines = 0;

  // fault-latch and auto-rearm policy (configured from HoverUartConfig)
  bool keepArmedCfg = false;
  uint32_t faultCooldownMsCfg = 1000;
  uint8_t maxAutoRearmCfg = 0;
  bool faultLatched = false;
  uint32_t faultLatchedMs = 0;
  uint8_t faultRearmAttempts = 0;

  // terminal-friendly motor diagnostic prints
  uint32_t lastDiagPrintMs = 0;
  int lastDiagOk = -2; // sentinel to force first print when diag appears
};
