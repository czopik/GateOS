#include "motor_controller.h"
#include <algorithm>
#include <math.h>

namespace {
bool isHoverDriver(const String& type) {
  String t = type;
  t.toLowerCase();
  return t == "hover_uart" || t == "hover";
}

int16_t signFor(bool forward, bool inverted) {
  int sign = forward ? 1 : -1;
  return inverted ? -sign : sign;
}
} // namespace

MotorController::MotorController() {}

MotorController::MotorController(int pwmPin_, int in1Pin_, int channel_, int freq_, int resolution_)
  : pwmPin(pwmPin_), in1Pin(in1Pin_), channel(channel_), frequency(freq_), resolution(resolution_) {
  maxDuty = (1 << resolution) - 1;
}

void MotorController::applyConfig(const MotorConfig& motorCfg, const GpioConfig& gpioCfg, const HoverUartConfig& hoverCfg) {
  motorConfig = motorCfg;
  gpioConfig = gpioCfg;
  hoverConfig = hoverCfg;

  pwmPin = gpioCfg.pwmPin;
  in1Pin = gpioCfg.dirPin;
  frequency = motorCfg.pwmFreq;
  resolution = motorCfg.pwmResolution;
  if (resolution < 1) resolution = 1;
  if (resolution > 16) resolution = 16;
  maxDuty = (1 << resolution) - 1;
  pwmMaxSetting = motorCfg.pwmMax > 0 ? motorCfg.pwmMax : maxDuty;

  driverKind = isHoverDriver(motorCfg.driverType) ? DRIVER_HOVER_UART : DRIVER_PWM_DIR;
  hover.configure(hoverCfg);
}

void MotorController::begin() {
  if (driverKind == DRIVER_HOVER_UART) {
    hover.begin();
    return;
  }
  configurePwmPins();
  pinMode(in1Pin, OUTPUT);
  setDirection(true);
  setDuty(0);
}

void MotorController::configurePwmPins() {
#if defined(ESP32)
  if (pwmPin >= 0) {
    ledcSetup(channel, frequency, resolution);
    ledcAttachPin(pwmPin, channel);
  }
#endif
}

void MotorController::setDuty(int duty) {
  if (driverKind == DRIVER_HOVER_UART) {
    int16_t speed = static_cast<int16_t>((static_cast<long>(duty) * hoverConfig.maxSpeed) / max(1, pwmMaxSetting));
    speed = static_cast<int16_t>(speed * signFor(directionForward, dirInverted));
    hover.setTargetSpeed(speed);
    return;
  }

  duty = constrain(duty, 0, maxDuty);
  currentDuty = duty;
#if defined(ESP32)
  if (pwmPin >= 0) ledcWrite(channel, duty);
#else
  analogWrite(pwmPin, map(duty, 0, maxDuty, 0, 255));
#endif
}

void MotorController::rampTo(int targetDuty_, unsigned long durationMs) {
  if (driverKind == DRIVER_HOVER_UART) {
    int16_t speed = 0;
    if (targetDuty_ > 0) {
      speed = static_cast<int16_t>((static_cast<long>(targetDuty_) * hoverConfig.maxSpeed) / max(1, pwmMaxSetting));
      speed = static_cast<int16_t>(speed * signFor(directionForward, dirInverted));
    }
    hover.setTargetSpeed(speed);
    return;
  }

  targetDuty = constrain(targetDuty_, 0, maxDuty);
  rampStart = millis();
  rampDuration = durationMs;
}

void MotorController::update() {
  if (driverKind == DRIVER_HOVER_UART) {
    hover.update(millis());
    return;
  }
  if (rampDuration == 0) return;
  unsigned long now = millis();
  unsigned long elapsed = now - rampStart;
  if (elapsed >= rampDuration) {
    setDuty(targetDuty);
    rampDuration = 0;
    return;
  }
  float t = (float)elapsed / (float)rampDuration;
  int newDuty = currentDuty + (int)((targetDuty - currentDuty) * t);
  setDuty(newDuty);
}

void MotorController::stop() {
  if (driverKind == DRIVER_HOVER_UART) {
    hover.stop();
    return;
  }
  setDuty(0);
}

void MotorController::setDirection(bool forward) {
  directionForward = forward;
  if (driverKind == DRIVER_HOVER_UART) return;
  bool level = forward;
  if (dirInverted) level = !level;
  if (in1Pin >= 0) {
    digitalWrite(in1Pin, level ? HIGH : LOW);
  }
}

void MotorController::setMotionProfile(const MotionAdvancedConfig& profile) {
  motionProfile = profile;
}

void MotorController::setTarget(bool forward, float targetDistance) {
  motionDirectionForward = forward;
  motionTargetDistance = targetDistance;
  motionStage = MOTION_ACCEL;
  motionActive = true;
  softStopping = false;
  motionRampStartMs = millis();
  motionRampStartDistance = -1.0f;
}

void MotorController::stopSoft() {
  if (!motionActive) {
    stop();
    motionStage = MOTION_IDLE;
    return;
  }
  softStopping = true;
  motionStage = MOTION_DECEL;
  motionTargetDistance = motionLastDistance;
}

void MotorController::stopHard() {
  motionActive = false;
  motionStage = MOTION_IDLE;
  softStopping = false;
  if (driverKind == DRIVER_HOVER_UART) {
    hover.emergencyStop();
    return;
  }
  stop();
}

void MotorController::tick(uint32_t nowMs, float currentDistance) {
  if (!motionActive) return;
  motionLastDistance = currentDistance;

  // --- Estimate velocity from distance changes (m/s), light EMA to reduce jitter ---
  static uint32_t lastTickMs = 0;
  static float lastDist = 0.0f;
  static float v_est = 0.0f;
  if (lastTickMs != 0 && nowMs > lastTickMs) {
    float dt = (float)(nowMs - lastTickMs) / 1000.0f;
    if (dt > 0.0f && dt < 0.5f) {
      float v_raw = (currentDistance - lastDist) / dt;
      const float alphaV = 0.25f;
      v_est = v_est + alphaV * (v_raw - v_est);
    }
  }
  lastTickMs = nowMs;
  lastDist = currentDistance;

  // Remaining distance to target (m)
  float remaining = motionDirectionForward ? motionTargetDistance - currentDistance : currentDistance - motionTargetDistance;

  // --- Latency compensation (telemetry + control loop) ---
  float telAge_s = 0.0f;
  if (driverKind == DRIVER_HOVER_UART) {
    const HoverTelemetry& tel = hover.telemetry();
    if (tel.lastTelMs != 0 && nowMs >= (uint32_t)tel.lastTelMs) {
      telAge_s = (float)(nowMs - (uint32_t)tel.lastTelMs) / 1000.0f;
    }
  }
  // Add a small buffer for jitter + command scheduling (tune if needed)
  const float extraLatency_s = 0.02f;
  const float d_latency = fabsf(v_est) * (telAge_s + extraLatency_s);

  float remaining_eff = remaining - d_latency;

  // Deadband: if we're close enough, stop cleanly (prevents oscillation/early overshoot correction)
  const float deadband_m = 0.005f; // 5mm (tune)
  if (remaining_eff <= deadband_m) {
    motionActive = false;
    motionStage = MOTION_IDLE;
    setDuty(0);
    return;
  }

  int targetSpeed = motionDirectionForward ? motionProfile.maxSpeedOpen : motionProfile.maxSpeedClose;
  int minSpeed = motionProfile.minSpeed;
  float brakeDistance = motionDirectionForward ? motionProfile.braking.startDistanceOpen : motionProfile.braking.startDistanceClose;
  if ((brakeDistance > 0.0f && remaining_eff <= brakeDistance) || softStopping) {
    motionStage = MOTION_DECEL;
  }
  int desired = minSpeed;
  if (motionStage == MOTION_ACCEL) {
    desired = accelSpeed(nowMs, currentDistance, targetSpeed);
  } else if (motionStage == MOTION_CRUISE) {
    desired = targetSpeed;
  } else if (motionStage == MOTION_DECEL) {
    desired = decelSpeed(remaining_eff, targetSpeed);
  }
  desired = constrain(desired, minSpeed, targetSpeed);
  int duty = convertSpeedToDuty(desired);
  setDuty(duty);
}

int MotorController::convertSpeedToDuty(int speed) const {
  int maxValue = pwmMaxSetting > 0 ? pwmMaxSetting : maxDuty;
  int low = motorConfig.pwmMin;
  if (low < 0) low = 0;
  return constrain(speed, low, maxValue);
}

int MotorController::accelSpeed(uint32_t nowMs, float currentDistance, int maxSpeed) {
  const MotorConfig::RampConfig& ramp = motionDirectionForward ? motionProfile.rampOpen : motionProfile.rampClose;
  float progress = 1.0f;
  if (ramp.mode == "time") {
    if (motionRampStartMs == 0) motionRampStartMs = nowMs;
    float elapsed = (float)(nowMs - motionRampStartMs);
    if (ramp.value > 0.0f) {
      progress = std::min(1.0f, elapsed / ramp.value);
    }
  } else {
    if (motionRampStartDistance < 0.0f) motionRampStartDistance = currentDistance;
    float traveled = motionDirectionForward ? currentDistance - motionRampStartDistance : motionRampStartDistance - currentDistance;
    if (traveled < 0.0f) traveled = 0.0f;
    if (ramp.value > 0.0f) {
      progress = std::min(1.0f, traveled / ramp.value);
    }
  }
  if (progress >= 0.999f) {
    motionStage = MOTION_CRUISE;
    return maxSpeed;
  }
  int range = maxSpeed - motionProfile.minSpeed;
  int result = motionProfile.minSpeed + (int)(range * progress);
  if (result < motionProfile.minSpeed) result = motionProfile.minSpeed;
  return result;
}

int MotorController::decelSpeed(float remaining, int maxSpeed) {
  const MotorConfig::BrakingConfig& brake = motionProfile.braking;
  float brakingDistance = motionDirectionForward ? brake.startDistanceOpen : brake.startDistanceClose;
  if (brakingDistance <= 0.0f) {
    return motionProfile.minSpeed;
  }
  float ratio = remaining / brakingDistance;
  if (ratio < 0.0f) ratio = 0.0f;
  if (ratio > 1.0f) ratio = 1.0f;
  float progress = 1.0f - ratio;
  float strength = constrain((float)brake.force / 100.0f, 0.0f, 1.5f);
  progress = powf(progress, 1.0f + strength);
  if (progress >= 1.0f) progress = 1.0f;

  int range = maxSpeed - motionProfile.minSpeed;
  int result = motionProfile.minSpeed + (int)(range * (1.0f - progress));
  if (result < motionProfile.minSpeed) result = motionProfile.minSpeed;

  if (brake.mode == "hold" && progress >= 1.0f) {
    return motionProfile.minSpeed;
  }
  return result;
}

// Proxy methods for hover UART commands
void MotorController::hoverArm() {
  if (driverKind == DRIVER_HOVER_UART) hover.arm();
}
void MotorController::hoverDisarm() {
  if (driverKind == DRIVER_HOVER_UART) hover.disarm();
}
void MotorController::hoverZero() {
  if (driverKind == DRIVER_HOVER_UART) hover.zero();
}
void MotorController::hoverGet() {
  if (driverKind == DRIVER_HOVER_UART) hover.get();
}
