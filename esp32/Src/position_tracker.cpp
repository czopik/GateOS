#include "position_tracker.h"

#include <math.h>
#include <stdint.h>

#include <LittleFS.h>

#include "motor_controller.h"
#include "gate_controller.h"

PositionTracker* PositionTracker::instance_ = nullptr;

namespace {
static constexpr const char* kPositionSnapshotPath = "/position.bin";
static constexpr const char* kPositionSnapshotTmpPath = "/position.tmp";
} // namespace

void PositionTracker::begin(ConfigManager* cfg, MotorController* motor, GateController* gate) {
  cfg_   = cfg;
  motor_ = motor;
  gate_  = gate;
  instance_ = this;
}

int PositionTracker::parsePullMode(const String& mode) const {
  if (mode == "down") return 2;
  if (mode == "none") return 0;
  return 1;
}

long PositionTracker::readHallCountAtomic() const {
  long v;
  portENTER_CRITICAL((portMUX_TYPE*)&hallMux_);
  v = hallCount_;
  portEXIT_CRITICAL((portMUX_TYPE*)&hallMux_);
  return v;
}

// FIX B-02: helper that reads AND snapshots hallCountLast_ under critical section
long PositionTracker::readAndConsumeHallDeltaAtomic() {
  long current, delta;
  portENTER_CRITICAL(&hallMux_);
  current = hallCount_;
  delta   = current - hallCountLast_;
  hallCountLast_ = current;
  portEXIT_CRITICAL(&hallMux_);
  return delta;
}

void PositionTracker::syncConfigPosition() {
  if (!cfg_) return;
  cfg_->gateConfig.maxDistance = maxDistanceMeters_;
  cfg_->gateConfig.totalDistance = maxDistanceMeters_;
}

void PositionTracker::syncGatePosition() {
  if (!gate_) return;
  gate_->setPosition(positionMeters_, maxDistanceMeters_);
  gate_->setControlPosition(positionMetersRaw_);
}

void PositionTracker::initializeFromConfig() {
  if (!cfg_) return;
  maxDistanceMeters_ = cfg_->gateConfig.maxDistance > 0.0f ?
    cfg_->gateConfig.maxDistance : cfg_->gateConfig.totalDistance;
  if (maxDistanceMeters_ < 0.0f) maxDistanceMeters_ = 0.0f;

  positionMeters_ = 0.0f;
  positionMetersRaw_ = positionMeters_;

  if (cfg_->gateConfig.positionSource == "hoverboard_tel") {
    float hbS = (cfg_->gateConfig.hbDistScale > 0.0f) ? cfg_->gateConfig.hbDistScale : 1.0f;
    hoverOffsetMeters_ = -((float)cfg_->gateConfig.hbOriginDistMm) / 1000.0f * hbS;
    hoverOffsetValid_  = true;
  } else {
    hoverOffsetMeters_ = 0.0f;
    hoverOffsetValid_  = false;
  }

  // FIX B-06: explicit reset of hover filter state so a second call
  //           (OTA, soft-reset, config reload) starts clean.
  posFilterInit_   = false;
  pos_f_           = 0.0f;
  lastTelMsFilter_ = 0;

  LittleFS.remove(kPositionSnapshotPath);
  LittleFS.remove(kPositionSnapshotTmpPath);

  syncConfigPosition();
  syncGatePosition();
}

bool PositionTracker::applyMaxDistance(float value, bool persist) {
  if (!cfg_ || value <= 0.0f || value > 100.0f) return false;

  maxDistanceMeters_ = value;
  if (positionMeters_ < 0.0f) positionMeters_ = 0.0f;
  if (positionMeters_ > maxDistanceMeters_) positionMeters_ = maxDistanceMeters_;
  positionMetersRaw_ = positionMeters_;
  positionPercent_ = maxDistanceMeters_ > 0.0f ?
    (int)((positionMeters_ * 100.0f) / maxDistanceMeters_ + 0.5f) : -1;

  if (cfg_->sensorsConfig.hall.enabled &&
      cfg_->sensorsConfig.hall.pin >= 0 &&
      cfg_->gateConfig.wheelCircumference > 0.0f &&
      cfg_->gateConfig.pulsesPerRevolution > 0) {
    float pulsesPerMeter = (float)cfg_->gateConfig.pulsesPerRevolution / cfg_->gateConfig.wheelCircumference;
    long totalCounts     = (long)(maxDistanceMeters_ * pulsesPerMeter);
    long newHallPos      = (long)(positionMeters_    * pulsesPerMeter);
    if (newHallPos < 0)            newHallPos = 0;
    if (newHallPos > totalCounts)  newHallPos = totalCounts;
    // FIX B-02: protect hallPosition_ write
    portENTER_CRITICAL(&hallMux_);
    hallPosition_ = newHallPos;
    portEXIT_CRITICAL(&hallMux_);
  }

  syncConfigPosition();
  syncGatePosition();

  if (persist) {
    String err;
    if (!cfg_->save(&err)) {
      Serial.printf("maxDistance save failed: %s\n", err.c_str());
      return false;
    }
  }
  return true;
}

bool PositionTracker::calibrateToMode(const char* mode, bool calibrationRunning) {
  if (!cfg_ || !gate_ || !mode || mode[0] == '\0') return false;
  if (gate_->isMoving())       return false;
  if (calibrationRunning)      return false;

  char key[8];
  size_t n = strlen(mode);
  if (n >= sizeof(key)) n = sizeof(key) - 1;
  for (size_t i = 0; i < n; ++i) {
    char c = mode[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    key[i] = c;
  }
  key[n] = '\0';

  bool setZero = strcmp(key, "zero") == 0 || strcmp(key, "close") == 0 || strcmp(key, "closed") == 0;
  bool setMax  = strcmp(key, "max")  == 0 || strcmp(key, "open")  == 0;
  if (!setZero && !setMax) return false;

  float maxDistance = cfg_->gateConfig.maxDistance > 0.0f ?
    cfg_->gateConfig.maxDistance : cfg_->gateConfig.totalDistance;
  if (maxDistance <= 0.0f) return false;

  maxDistanceMeters_ = maxDistance;
  positionMeters_    = setZero ? 0.0f : maxDistanceMeters_;
  positionMetersRaw_ = positionMeters_;
  positionPercent_   = maxDistanceMeters_ > 0.0f ?
    (int)((positionMeters_ * 100.0f) / maxDistanceMeters_ + 0.5f) : -1;

  if (cfg_->sensorsConfig.hall.enabled &&
      cfg_->sensorsConfig.hall.pin >= 0 &&
      cfg_->gateConfig.wheelCircumference > 0.0f &&
      cfg_->gateConfig.pulsesPerRevolution > 0) {
    float pulsesPerMeter = (float)cfg_->gateConfig.pulsesPerRevolution / cfg_->gateConfig.wheelCircumference;
    long totalCounts     = (long)(maxDistanceMeters_ * pulsesPerMeter);
    long newHallPos      = setZero ? 0 : totalCounts;
    // FIX B-02
    portENTER_CRITICAL(&hallMux_);
    hallPosition_ = newHallPos;
    portEXIT_CRITICAL(&hallMux_);
  }

  syncConfigPosition();
  syncGatePosition();

  if (motor_ && motor_->isHoverUart() && motor_->hoverEnabled()) {
    const HoverTelemetry& tel = motor_->hoverTelemetry();
    if (tel.lastTelMs != 0) {
      const float hbS2 = (cfg_->gateConfig.hbDistScale > 0.0f) ? cfg_->gateConfig.hbDistScale : 1.0f;
      const float pos_raw = (float)tel.distMm / 1000.0f * hbS2;
      if (setZero) {
        hoverOffsetMeters_ = -pos_raw;
        hoverOffsetValid_  = true;
        cfg_->gateConfig.hbOriginDistMm = tel.distMm;
      } else if (setMax && maxDistanceMeters_ > 0.0f) {
        hoverOffsetMeters_ = maxDistanceMeters_ - pos_raw;
        hoverOffsetValid_  = true;
        cfg_->gateConfig.hbOriginDistMm = (int32_t)lroundf((float)tel.distMm - maxDistanceMeters_ * 1000.0f / hbS2);
      }
    }
  }

  String err;
  if (!cfg_->save(&err)) {
    Serial.printf("calibrate save failed: %s\n", err.c_str());
    return false;
  }
  return true;
}

void PositionTracker::requestResyncOpen() {
  resyncAtOpenLimit_ = true;
}

void PositionTracker::requestResyncClose() {
  resyncAtCloseLimit_ = true;
}

void IRAM_ATTR PositionTracker::hallIsrThunk() {
  if (instance_) instance_->onHallIsr();
}

void IRAM_ATTR PositionTracker::onHallIsr() {
  uint32_t nowUs = micros();
  uint32_t debounceUs = hallDebounceUs_;
  if (debounceUs > 0) {
    uint32_t last = hallLastIsrUs_;
    if (nowUs - last < debounceUs) return;
    hallLastIsrUs_ = nowUs;
  }
  portENTER_CRITICAL_ISR(&hallMux_);
  hallCount_++;
  portEXIT_CRITICAL_ISR(&hallMux_);
}

void PositionTracker::updateHallAttachment(bool calibrationRunning) {
  if (!cfg_) return;
  int  pin     = cfg_->sensorsConfig.hall.pin;
  bool enabled = cfg_->sensorsConfig.hall.enabled && pin >= 0;
  hallDebounceUs_ = (uint32_t)cfg_->sensorsConfig.hall.debounceMs * 1000U;

  if (calibrationRunning) {
    if (hallAttached_ && hallPinActive_ >= 0) {
      detachInterrupt(hallPinActive_);
      Serial.printf("[HALL] detached runtime ISR for calibration on pin=%d\n", hallPinActive_);
    }
    hallAttached_ = false;
    hallPinActive_ = pin;
    return;
  }

  if (!enabled) {
    if (hallAttached_ && hallPinActive_ >= 0) {
      detachInterrupt(hallPinActive_);
    }
    hallAttached_ = false;
    hallPinActive_ = pin;
    return;
  }

  if (hallAttached_ && hallPinActive_ == pin) return;
  if (hallAttached_ && hallPinActive_ >= 0) {
    detachInterrupt(hallPinActive_);
  }

  hallPinActive_ = pin;
  int intNum = digitalPinToInterrupt(pin);
  if (intNum < 0) {
    hallAttached_ = false;
    return;
  }

  int pull = parsePullMode(cfg_->sensorsConfig.hall.pullMode);
  pinMode(pin, pull == 2 ? INPUT_PULLDOWN : (pull == 1 ? INPUT_PULLUP : INPUT));
  int mode = cfg_->sensorsConfig.hall.invert ? FALLING : RISING;
  attachInterrupt(intNum, hallIsrThunk, mode);
  hallAttached_ = true;

  // FIX B-02: protect hallCountLast_ and hallPosition_ writes
  portENTER_CRITICAL(&hallMux_);
  hallCountLast_ = hallCount_;
  portEXIT_CRITICAL(&hallMux_);

  float maxDistance = cfg_->gateConfig.maxDistance > 0.0f ?
    cfg_->gateConfig.maxDistance : cfg_->gateConfig.totalDistance;
  if (maxDistance < 0.0f) maxDistance = 0.0f;
  if (maxDistance > 0.0f &&
      cfg_->gateConfig.wheelCircumference > 0.0f &&
      cfg_->gateConfig.pulsesPerRevolution > 0) {
    float pulsesPerMeter = (float)cfg_->gateConfig.pulsesPerRevolution / cfg_->gateConfig.wheelCircumference;
    long totalCounts     = (long)(maxDistance * pulsesPerMeter);
    long newHallPos      = (long)(positionMeters_ * pulsesPerMeter);
    if (newHallPos < 0)           newHallPos = 0;
    if (newHallPos > totalCounts) newHallPos = totalCounts;
    portENTER_CRITICAL(&hallMux_);
    hallPosition_ = newHallPos;
    portEXIT_CRITICAL(&hallMux_);
  }
}

void PositionTracker::updatePosition(bool calibrationRunning) {
  if (!cfg_ || !gate_) return;

  bool resyncClose = resyncAtCloseLimit_;
  bool resyncOpen  = resyncAtOpenLimit_;
  resyncAtCloseLimit_ = false;
  resyncAtOpenLimit_    = false;

  float maxDistance = cfg_->gateConfig.maxDistance > 0.0f ?
    cfg_->gateConfig.maxDistance : cfg_->gateConfig.totalDistance;
  if (maxDistance < 0.0f) maxDistance = 0.0f;
  maxDistanceMeters_ = maxDistance;

  if (positionMeters_ < 0.0f) positionMeters_ = 0.0f;
  if (maxDistanceMeters_ > 0.0f && positionMeters_ > maxDistanceMeters_) {
    positionMeters_ = maxDistanceMeters_;
  }
  positionMetersRaw_ = positionMeters_;

  float pulsesPerMeter = 0.0f;
  long  totalCounts    = 0;
  if (cfg_->sensorsConfig.hall.enabled &&
      cfg_->sensorsConfig.hall.pin >= 0 &&
      maxDistanceMeters_ > 0.0f &&
      cfg_->gateConfig.wheelCircumference > 0.0f &&
      cfg_->gateConfig.pulsesPerRevolution > 0) {
    pulsesPerMeter = (float)cfg_->gateConfig.pulsesPerRevolution / cfg_->gateConfig.wheelCircumference;
    totalCounts    = (long)(maxDistanceMeters_ * pulsesPerMeter);
  }

  if (resyncClose) {
    positionMeters_    = 0.0f;
    positionMetersRaw_ = 0.0f;
    if (totalCounts > 0) {
      portENTER_CRITICAL(&hallMux_);
      hallPosition_ = 0;
      portEXIT_CRITICAL(&hallMux_);
    }
  }
  if (resyncOpen) {
    positionMeters_    = maxDistanceMeters_;
    positionMetersRaw_ = maxDistanceMeters_;
    if (totalCounts > 0) {
      portENTER_CRITICAL(&hallMux_);
      hallPosition_ = totalCounts;
      portEXIT_CRITICAL(&hallMux_);
    }
  }

  if (calibrationRunning) {
    syncConfigPosition();
    positionMetersRaw_ = positionMeters_;
    syncGatePosition();
    return;
  }

  const bool preferHover   = (cfg_->gateConfig.positionSource == "hoverboard_tel");
  const bool hallAvailable = (hallAttached_ && totalCounts > 0 && pulsesPerMeter > 0.0f);
  const bool hoverAvailable= (motor_ && motor_->isHoverUart() && motor_->hoverEnabled());

  if (!preferHover && hallAvailable) {
    // FIX B-02: use atomic helper that reads delta under critical section
    long delta = readAndConsumeHallDeltaAtomic();

    if (gate_->isMoving() && delta != 0) {
      int dir = gate_->getLastDirection() >= 0 ? 1 : -1;
      long newHallPos;
      portENTER_CRITICAL(&hallMux_);
      hallPosition_ += delta * dir;
      if (hallPosition_ < 0)            hallPosition_ = 0;
      if (hallPosition_ > totalCounts)  hallPosition_ = totalCounts;
      newHallPos = hallPosition_;
      portEXIT_CRITICAL(&hallMux_);
      positionMeters_ = (float)newHallPos / pulsesPerMeter;
    }
    positionMetersRaw_ = positionMeters_;
    positionPercent_ = (maxDistanceMeters_ > 0.0f) ?
      (int)((positionMeters_ * 100.0f) / maxDistanceMeters_ + 0.5f) : -1;
    syncConfigPosition();
    syncGatePosition();
    return;
  }

  if (preferHover && hoverAvailable) {
    const HoverTelemetry& tel = motor_->hoverTelemetry();
    const uint32_t now = millis();

    uint32_t telTimeoutMs = cfg_->gateConfig.telemetryTimeoutMs > 0 ? cfg_->gateConfig.telemetryTimeoutMs : 1000;
    if (tel.lastTelMs != 0 && !motor_->hoverTelemetryTimedOut(now, telTimeoutMs)) {
      const float hbScale = (cfg_->gateConfig.hbDistScale > 0.0f) ? cfg_->gateConfig.hbDistScale : 1.0f;
      const float pos_raw = (float)tel.distMm / 1000.0f * hbScale;
      if (!isfinite(pos_raw)) {
        syncConfigPosition();
        positionMetersRaw_ = positionMeters_;
        syncGatePosition();
        return;
      }
      float pos_raw_adj   = pos_raw;
      if (!hoverOffsetValid_ && cfg_->gateConfig.hbOriginDistMm != 0) {
        hoverOffsetMeters_ = -((float)cfg_->gateConfig.hbOriginDistMm) / 1000.0f * hbScale;
        hoverOffsetValid_  = true;
      }
      if (hoverOffsetValid_) {
        pos_raw_adj += hoverOffsetMeters_;
      }
      if (!isfinite(pos_raw_adj)) {
        syncConfigPosition();
        positionMetersRaw_ = positionMeters_;
        syncGatePosition();
        return;
      }

      // FIX B-06: use class member fields instead of static locals
      float dt = 0.0f;
      if (lastTelMsFilter_ != 0 && (uint32_t)tel.lastTelMs >= lastTelMsFilter_) {
        dt = (float)(tel.lastTelMs - (long)lastTelMsFilter_) / 1000.0f;
      }
      const float vMax_m_s = 1.5f;
      const float maxJump  = (dt > 0.0f) ? (vMax_m_s * dt * 1.5f) : 0.20f;

      if (!posFilterInit_) {
        pos_f_        = pos_raw_adj;
        posFilterInit_ = true;
      } else {
        const float diff = fabsf(pos_raw_adj - pos_f_);
        if (diff <= maxJump) {
          const float alpha = 0.25f;
          pos_f_ = pos_f_ + alpha * (pos_raw_adj - pos_f_);
          if (!isfinite(pos_f_)) {
            pos_f_ = pos_raw_adj;
          }
        }
      }

      if (resyncClose || resyncOpen) {
        if (resyncClose) {
          hoverOffsetMeters_ = -pos_raw;
          hoverOffsetValid_  = true;
          cfg_->gateConfig.hbOriginDistMm = tel.distMm;
        } else if (resyncOpen && maxDistanceMeters_ > 0.0f) {
          hoverOffsetMeters_ = maxDistanceMeters_ - pos_raw;
          hoverOffsetValid_  = true;
          cfg_->gateConfig.hbOriginDistMm = (int32_t)lroundf((float)tel.distMm - maxDistanceMeters_ * 1000.0f / hbScale);
        }
        if (hoverOffsetValid_) {
          pos_raw_adj  = pos_raw + hoverOffsetMeters_;
          pos_f_       = pos_raw_adj;
          posFilterInit_ = true;
        }
      }

      lastTelMsFilter_ = (uint32_t)tel.lastTelMs;

      positionMeters_    = pos_f_;
      positionMetersRaw_ = pos_raw_adj;

      if (positionMeters_ < 0.0f) positionMeters_ = 0.0f;
      if (maxDistanceMeters_ > 0.0f && positionMeters_ > maxDistanceMeters_) {
        positionMeters_ = maxDistanceMeters_;
      }
      if (positionMetersRaw_ < 0.0f) positionMetersRaw_ = 0.0f;
      if (maxDistanceMeters_ > 0.0f && positionMetersRaw_ > maxDistanceMeters_) {
        positionMetersRaw_ = maxDistanceMeters_;
      }

      if (!gate_->isMoving() && maxDistanceMeters_ > 0.0f) {
        const float endEps    = 0.02f;
        const float snapWindow= 0.35f;
        // Auto-snap: when gate rests within 2 cm of an endpoint and the raw
        // odometer also agrees (within 35 cm of the endpoint), recalibrate
        // hoverOffsetMeters_ so future readings are pinned to 0 / maxDist.
        // Only update hbOriginDistMm if the change exceeds 20 mm to avoid
        // unnecessary RAM churn on every telemetry frame.
        if (positionMeters_ <= endEps && fabsf(pos_raw_adj) <= snapWindow) {
          hoverOffsetMeters_ = -pos_raw;
          hoverOffsetValid_  = true;
          const int32_t newOrigin = (int32_t)lroundf(pos_raw * 1000.0f);
          if (abs(newOrigin - cfg_->gateConfig.hbOriginDistMm) > 20)
            cfg_->gateConfig.hbOriginDistMm = newOrigin;
          pos_f_             = 0.0f;
          positionMeters_    = 0.0f;
          positionMetersRaw_ = 0.0f;
          posFilterInit_     = true;
        } else if (positionMeters_ >= (maxDistanceMeters_ - endEps) &&
                   fabsf(maxDistanceMeters_ - pos_raw_adj) <= snapWindow) {
          hoverOffsetMeters_ = maxDistanceMeters_ - pos_raw;
          hoverOffsetValid_  = true;
          const int32_t newOrigin = (int32_t)lroundf((pos_raw - maxDistanceMeters_) * 1000.0f);
          if (abs(newOrigin - cfg_->gateConfig.hbOriginDistMm) > 20)
            cfg_->gateConfig.hbOriginDistMm = newOrigin;
          pos_f_             = maxDistanceMeters_;
          positionMeters_    = maxDistanceMeters_;
          positionMetersRaw_ = maxDistanceMeters_;
          posFilterInit_     = true;
        }
      }
      positionPercent_ = (maxDistanceMeters_ > 0.0f) ?
        (int)((positionMeters_ * 100.0f) / maxDistanceMeters_ + 0.5f) : -1;
      syncConfigPosition();
      syncGatePosition();
      return;
    }
  }

  syncConfigPosition();
  positionMetersRaw_ = positionMeters_;
  syncGatePosition();
}

void PositionTracker::updateHallStats(uint32_t nowMs) {
  if (!cfg_) return;
  if (!cfg_->sensorsConfig.hall.enabled || cfg_->sensorsConfig.hall.pin < 0) {
    hallPps_         = 0.0f;
    hallPpsLastMs_   = nowMs;
    hallPpsLastCount_= readHallCountAtomic();
    return;
  }
  if (hallPpsLastMs_ == 0) {
    hallPpsLastMs_   = nowMs;
    hallPpsLastCount_= readHallCountAtomic();
    return;
  }
  uint32_t deltaMs = nowMs - hallPpsLastMs_;
  if (deltaMs < 1000) return;
  long count = readHallCountAtomic();
  long delta = count - hallPpsLastCount_;
  hallPps_         = deltaMs > 0 ? (float)delta / (deltaMs / 1000.0f) : 0.0f;
  hallPpsLastMs_   = nowMs;
  hallPpsLastCount_= count;
}

