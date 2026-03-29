#pragma once
#include <Arduino.h>
#include "config_manager.h"
#include "hover_uart_driver.h"

// MotorController - PWM with smooth ramping and direction control, or hover UART driver
class MotorController {
public:
  MotorController();
  MotorController(int pwmPin, int in1Pin, int channel = 0, int freq = 5000, int resolution = 8);

  void applyConfig(const MotorConfig& motorCfg, const GpioConfig& gpioCfg, const HoverUartConfig& hoverCfg);
  void begin();

  void setDuty(int duty);
  void rampTo(int targetDuty, unsigned long durationMs);
  void update();
  void stop();

  void setDirection(bool forward);
  bool getDirection() const { return directionForward; }
  void setInvertDir(bool invert) { dirInverted = invert; }
  void setMotionProfile(const MotionAdvancedConfig& profile);
  void setTarget(bool forward, float targetDistance);
  void stopSoft();
  void stopHard();
  void tick(uint32_t nowMs, float currentDistance);
  bool isMotionActive() const { return motionActive; }

  bool isHoverUart() const { return driverKind == DRIVER_HOVER_UART; }
  bool hoverEnabled() const { return hover.isEnabled(); }
  const HoverTelemetry& hoverTelemetry() const { return hover.telemetry(); }
  int16_t hoverLastCmdSpeed() const { return hover.getCurrentSpeed(); }
  bool hoverTelemetryTimedOut(uint32_t nowMs, uint32_t timeoutMs) const { return hover.telemetryTimedOut(nowMs, timeoutMs); }
  // direct hover speed setter (raw hover units)
  void setHoverTargetSpeed(int16_t speed) { if (driverKind == DRIVER_HOVER_UART) hover.setTargetSpeed(speed); }
  // proxy hover commands
  void hoverArm();
  void hoverDisarm();
  void hoverZero();
  void hoverGet();

private:
  enum DriverKind {
    DRIVER_PWM_DIR = 0,
    DRIVER_HOVER_UART = 1
  };

  void configurePwmPins();

  DriverKind driverKind = DRIVER_PWM_DIR;
  MotorConfig motorConfig;
  GpioConfig gpioConfig;
  HoverUartConfig hoverConfig;

  int pwmPin = -1;
  int in1Pin = -1;
  int channel = 0;
  int frequency = 5000;
  int resolution = 8;
  int maxDuty = 255;
  int pwmMaxSetting = 255;

  int currentDuty = 0;
  int targetDuty = 0;
  unsigned long rampStart = 0;
  unsigned long rampDuration = 0;
  bool dirInverted = false;
  bool directionForward = true;

  HoverUartDriver hover;
  enum MotionStage {
    MOTION_IDLE,
    MOTION_ACCEL,
    MOTION_CRUISE,
    MOTION_DECEL
  };
  MotionStage motionStage = MOTION_IDLE;
  bool motionActive = false;
  bool motionDirectionForward = true;
  float motionTargetDistance = 0.0f;
  float motionRampStartDistance = -1.0f;
  uint32_t motionRampStartMs = 0;
  float motionLastDistance = 0.0f;
  bool softStopping = false;
  MotionAdvancedConfig motionProfile;
  int convertSpeedToDuty(int speed) const;
  int accelSpeed(uint32_t nowMs, float currentDistance, int maxSpeed);
  int decelSpeed(float remaining, int maxSpeed);
};
