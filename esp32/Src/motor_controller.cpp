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
    bool forward = directionForward;
    speed = static_cast<int16_t>(speed * signFor(forward, dirInverted));
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
    hover.setDecelBoost(false);
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
  vLastTickMs = 0;
  vLastDist = 0.0f;
  vEst = 0.0f;
  motionBand = -1;
  if (driverKind == DRIVER_HOVER_UART) {
    hover.setDecelBoost(false);
  }
}

void MotorController::stopSoft() {
  if (!motionActive) {
    stop();
    motionStage = MOTION_IDLE;
    return;
  }

  // Mini soft-stop:
  // When we are running a distance-based motion profile, setting targetDistance
  // to the current position makes remaining==0 and the control loop immediately
  // drops duty to 0 (feels like a hard stop / jerk). For a gate, a short
  // controlled deceleration is nicer and safer.
  //
  // Strategy: create a small "virtual" stop distance ahead of current position
  // (in the current direction). The existing decel logic then ramps speed down
  // to 0 over that short distance.
  softStopping = true;
  motionStage = MOTION_DECEL;

  // Base distance (meters) + speed-proportional term, clamped.
  // "Mini" means: stop quickly, but without a brick-wall jerk.
  float stopDist = 0.015f + fabsf(vEst) * 0.05f;  // 1.5cm + v*50ms
  if (stopDist < 0.015f) stopDist = 0.015f;       // min 1.5cm
  if (stopDist > 0.08f)  stopDist = 0.08f;        // max 8cm

  motionTargetDistance = motionLastDistance + (motionDirectionForward ? stopDist : -stopDist);
}

void MotorController::stopHard() {
  motionActive = false;
  motionStage = MOTION_IDLE;
  softStopping = false;
  forceDecelBoost = false;
  motionBand = -1;
  vLastTickMs = 0;
  vEst = 0.0f;
  if (driverKind == DRIVER_HOVER_UART) {
    hover.setDecelBoost(false);
    hover.emergencyStop();
    return;
  }
  stop();
}

void MotorController::setForceDecelBoost(bool enable) {
  forceDecelBoost = enable;
}

void MotorController::tick(uint32_t nowMs, float currentDistance) {
  if (!motionActive) return;
  motionLastDistance = currentDistance;

  // --- Estimate velocity from distance changes (m/s), light EMA to reduce jitter ---
  if (vLastTickMs != 0 && nowMs > vLastTickMs) {
    float dt = (float)(nowMs - vLastTickMs) / 1000.0f;
    if (dt > 0.0f && dt < 0.5f) {
      float v_raw = (currentDistance - vLastDist) / dt;
      const float alphaV = 0.25f;
      vEst = vEst + alphaV * (v_raw - vEst);
    }
  }
  vLastTickMs = nowMs;
  vLastDist = currentDistance;

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
  const float d_latency = fabsf(vEst) * (telAge_s + extraLatency_s);

  float remaining_eff = remaining - d_latency;

  // Multi-zone target approach for repeatable endpoint positioning:
  // - far: normal motion profile
  // - approach: capped speed
  // - precision: very low speed + tighter stop tolerance
  const float approachZone_m = 0.35f;
  const float precisionZone_m = 0.10f;
  const float stopTolerance_m = 0.010f;      // 10mm target band
  const float stopToleranceExit_m = 0.018f;  // hysteresis (18mm)
  const float vStop_mps = 0.07f;

  int8_t nextBand = 2;
  if (remaining_eff <= precisionZone_m) nextBand = 0;
  else if (remaining_eff <= approachZone_m) nextBand = 1;
  if (nextBand != motionBand) {
    motionBand = nextBand;
    Serial.printf("[MOTOR] band=%d rem=%.3fm v=%.3fm/s dir=%s\n",
                  (int)motionBand,
                  remaining_eff,
                  vEst,
                  motionDirectionForward ? "open" : "close");
  }

  // Stop when inside tolerance and nearly not moving.
  // Hysteresis avoids oscillation at band edge.
  if (remaining_eff <= stopTolerance_m && fabsf(vEst) <= vStop_mps) {
    motionActive = false;
    motionStage = MOTION_IDLE;
    motionBand = -1;
    if (driverKind == DRIVER_HOVER_UART) {
      hover.setDecelBoost(false);
    }
    setDuty(0);
    return;
  }
  if (remaining_eff <= stopToleranceExit_m) {
    motionStage = MOTION_DECEL;
  }

  int targetSpeed = motionDirectionForward ? motionProfile.maxSpeedOpen : motionProfile.maxSpeedClose;
  int minSpeed = motionProfile.minSpeed;
  float brakeDistance = motionDirectionForward ? motionProfile.braking.startDistanceOpen : motionProfile.braking.startDistanceClose;
  float brakeDistanceEff = brakeDistance;
  if (driverKind == DRIVER_HOVER_UART && brakeDistanceEff > 0.0f) {
    brakeDistanceEff *= 1.5f;
  }
  if (driverKind == DRIVER_HOVER_UART && brakeDistance > 0.0f) {
    float forceScale = constrain((float)motionProfile.braking.force / 100.0f, 0.3f, 1.7f);
    float a_brake = 0.8f * forceScale; // m/s^2 nominal decel
    if (a_brake > 0.05f) {
      float d_stop = (vEst * vEst) / (2.0f * a_brake);
      if (d_stop > brakeDistanceEff) brakeDistanceEff = d_stop;
    }
  }
  if ((brakeDistanceEff > 0.0f && remaining_eff <= brakeDistanceEff) || softStopping) {
    motionStage = MOTION_DECEL;
  }
  if (driverKind == DRIVER_HOVER_UART) {
    hover.setDecelBoost(forceDecelBoost || motionStage == MOTION_DECEL);
  }
  int desired = minSpeed;
  if (motionStage == MOTION_ACCEL) {
    desired = accelSpeed(nowMs, currentDistance, targetSpeed);
  } else if (motionStage == MOTION_CRUISE) {
    desired = targetSpeed;
  } else if (motionStage == MOTION_DECEL) {
    desired = decelSpeed(remaining_eff, targetSpeed, brakeDistanceEff);
  }

  // Multi-zone speed caps for precise endpointing.
  int approachSpeed = max(8, (int)(targetSpeed * 0.38f));
  int precisionSpeed = max(4, (int)(targetSpeed * 0.16f));
  if (driverKind != DRIVER_HOVER_UART) {
    approachSpeed = max(minSpeed, approachSpeed);
    precisionSpeed = max(minSpeed, precisionSpeed);
  }
  if (motionBand == 1 && desired > approachSpeed) {
    desired = approachSpeed;
  } else if (motionBand == 0 && desired > precisionSpeed) {
    desired = precisionSpeed;
  }

  int minClamp = minSpeed;
  if (driverKind == DRIVER_HOVER_UART && motionStage == MOTION_DECEL) {
    minClamp = 0;
  }
  desired = constrain(desired, minClamp, targetSpeed);

  // Additional damping very close to target.
  if (motionBand == 0) {
    int damp = (int)(fabsf(vEst) * 0.20f * (float)targetSpeed);
    desired -= damp;
    if (desired < minClamp) desired = minClamp;
  }

  int duty = convertSpeedToDuty(desired);
  if (motionBand == 0) {
    Serial.printf("[MOTOR] precise rem=%.3fm v=%.3fm/s desired=%d duty=%d\n",
                  remaining_eff,
                  vEst,
                  desired,
                  duty);
  }
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

int MotorController::decelSpeed(float remaining, int maxSpeed, float brakingDistance) {
  const MotorConfig::BrakingConfig& brake = motionProfile.braking;
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
