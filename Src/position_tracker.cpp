#include "position_tracker.h"

#include <math.h>
#include <stdint.h>

#include "motor_controller.h"
#include "gate_controller.h"

PositionTracker* PositionTracker::instance_ = nullptr;

namespace {
static constexpr const char* kPositionSnapshotPath = "/position.bin";

#pragma pack(push, 1)
struct PositionSnapshot {
  float position;
  float maxDistance;
  uint32_t crc;
};
#pragma pack(pop)

uint32_t crc32Calc(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) {
      const uint32_t mask = -(crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}
} // namespace

void PositionTracker::begin(ConfigManager* cfg, MotorController* motor, GateController* gate) {
  cfg_ = cfg;
  motor_ = motor;
  gate_ = gate;
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

void PositionTracker::syncConfigPosition() {
  if (!cfg_) return;
  cfg_->gateConfig.position = positionMeters_;
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

  positionMeters_ = cfg_->gateConfig.position;
  if (positionMeters_ < 0.0f) positionMeters_ = 0.0f;
  if (maxDistanceMeters_ > 0.0f && positionMeters_ > maxDistanceMeters_) {
    positionMeters_ = maxDistanceMeters_;
  }
  positionMetersRaw_ = positionMeters_;

  if (cfg_->gateConfig.positionSource == "hoverboard_tel") {
    hoverOffsetMeters_ = -((float)cfg_->gateConfig.hbOriginDistMm) / 1000.0f;
    hoverOffsetValid_ = true;
  } else {
    hoverOffsetMeters_ = 0.0f;
    hoverOffsetValid_ = false;
  }

  lastPersistedPosition_ = positionMeters_;
  lastPositionPersistMs_ = millis();
  loadPositionSnapshot();
  syncConfigPosition();
  syncGatePosition();
}

bool PositionTracker::loadPositionSnapshot() {
  File f = LittleFS.open(kPositionSnapshotPath, "r");
  if (!f) return false;
  PositionSnapshot snap{};
  if (f.read(reinterpret_cast<uint8_t*>(&snap), sizeof(snap)) != sizeof(snap)) {
    f.close();
    return false;
  }
  f.close();
  uint32_t crc = crc32Calc(reinterpret_cast<const uint8_t*>(&snap), sizeof(snap) - sizeof(snap.crc));
  if (crc != snap.crc) {
    Serial.println("[POS] snapshot CRC mismatch");
    return false;
  }
  if (snap.maxDistance > 0.0f) {
    maxDistanceMeters_ = snap.maxDistance;
  }
  if (snap.position >= 0.0f) {
    positionMeters_ = snap.position;
  }
  if (maxDistanceMeters_ > 0.0f && positionMeters_ > maxDistanceMeters_) {
    positionMeters_ = maxDistanceMeters_;
  }
  positionMetersRaw_ = positionMeters_;
  positionPercent_ = maxDistanceMeters_ > 0.0f ?
    (int)((positionMeters_ * 100.0f) / maxDistanceMeters_ + 0.5f) : -1;
  lastPersistedPosition_ = positionMeters_;
  Serial.printf("[POS] snapshot loaded pos=%.3fm max=%.3fm\n", positionMeters_, maxDistanceMeters_);
  return true;
}

bool PositionTracker::savePositionSnapshot() {
  if (maxDistanceMeters_ <= 0.0f) return false;
  PositionSnapshot snap{};
  snap.position = positionMeters_;
  snap.maxDistance = maxDistanceMeters_;
  snap.crc = crc32Calc(reinterpret_cast<const uint8_t*>(&snap), sizeof(snap) - sizeof(snap.crc));
  File f = LittleFS.open(kPositionSnapshotPath, "w");
  if (!f) return false;
  bool ok = f.write(reinterpret_cast<const uint8_t*>(&snap), sizeof(snap)) == sizeof(snap);
  f.flush();
  f.close();
  if (ok) {
    lastPersistedPosition_ = positionMeters_;
    lastPositionPersistMs_ = millis();
    persistDirty_ = false;
  }
  return ok;
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
    long totalCounts = (long)(maxDistanceMeters_ * pulsesPerMeter);
    hallPosition_ = (long)(positionMeters_ * pulsesPerMeter);
    if (hallPosition_ < 0) hallPosition_ = 0;
    if (hallPosition_ > totalCounts) hallPosition_ = totalCounts;
  }

  syncConfigPosition();
  syncGatePosition();

  if (persist) {
    String err;
    if (!cfg_->save(&err)) {
      Serial.printf("maxDistance save failed: %s\n", err.c_str());
      return false;
    }
    lastPersistedPosition_ = positionMeters_;
    lastPositionPersistMs_ = millis();
  }
  return true;
}

bool PositionTracker::calibrateToMode(const char* mode, bool calibrationRunning) {
  if (!cfg_ || !gate_ || !mode || mode[0] == '\0') return false;
  if (gate_->isMoving()) return false;
  if (calibrationRunning) return false;

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
  bool setMax = strcmp(key, "max") == 0 || strcmp(key, "open") == 0;
  if (!setZero && !setMax) return false;

  float maxDistance = cfg_->gateConfig.maxDistance > 0.0f ?
    cfg_->gateConfig.maxDistance : cfg_->gateConfig.totalDistance;
  if (maxDistance <= 0.0f) return false;

  maxDistanceMeters_ = maxDistance;
  positionMeters_ = setZero ? 0.0f : maxDistanceMeters_;
  positionMetersRaw_ = positionMeters_;
  positionPercent_ = maxDistanceMeters_ > 0.0f ?
    (int)((positionMeters_ * 100.0f) / maxDistanceMeters_ + 0.5f) : -1;

  if (cfg_->sensorsConfig.hall.enabled &&
      cfg_->sensorsConfig.hall.pin >= 0 &&
      cfg_->gateConfig.wheelCircumference > 0.0f &&
      cfg_->gateConfig.pulsesPerRevolution > 0) {
    float pulsesPerMeter = (float)cfg_->gateConfig.pulsesPerRevolution / cfg_->gateConfig.wheelCircumference;
    long totalCounts = (long)(maxDistanceMeters_ * pulsesPerMeter);
    hallPosition_ = setZero ? 0 : totalCounts;
  }

  syncConfigPosition();
  syncGatePosition();

  if (motor_ && motor_->isHoverUart() && motor_->hoverEnabled()) {
    const HoverTelemetry& tel = motor_->hoverTelemetry();
    if (tel.lastTelMs != 0) {
      const float pos_raw = (float)tel.distMm / 1000.0f;
      if (setZero) {
        hoverOffsetMeters_ = -pos_raw;
        hoverOffsetValid_ = true;
        cfg_->gateConfig.hbOriginDistMm = tel.distMm;
      } else if (setMax && maxDistanceMeters_ > 0.0f) {
        hoverOffsetMeters_ = maxDistanceMeters_ - pos_raw;
        hoverOffsetValid_ = true;
        cfg_->gateConfig.hbOriginDistMm = (int32_t)lroundf((pos_raw - maxDistanceMeters_) * 1000.0f);
      }
    }
  }

  String err;
  if (!cfg_->save(&err)) {
    Serial.printf("calibrate save failed: %s\n", err.c_str());
    return false;
  }
  lastPersistedPosition_ = positionMeters_;
  lastPositionPersistMs_ = millis();
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
  int pin = cfg_->sensorsConfig.hall.pin;
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

  hallCountLast_ = readHallCountAtomic();
  float maxDistance = cfg_->gateConfig.maxDistance > 0.0f ?
    cfg_->gateConfig.maxDistance : cfg_->gateConfig.totalDistance;
  if (maxDistance < 0.0f) maxDistance = 0.0f;
  if (maxDistance > 0.0f &&
      cfg_->gateConfig.wheelCircumference > 0.0f &&
      cfg_->gateConfig.pulsesPerRevolution > 0) {
    float pulsesPerMeter = (float)cfg_->gateConfig.pulsesPerRevolution / cfg_->gateConfig.wheelCircumference;
    long totalCounts = (long)(maxDistance * pulsesPerMeter);
    hallPosition_ = (long)(positionMeters_ * pulsesPerMeter);
    if (hallPosition_ < 0) hallPosition_ = 0;
    if (hallPosition_ > totalCounts) hallPosition_ = totalCounts;
  }
}

void PositionTracker::updatePosition(bool calibrationRunning) {
  if (!cfg_ || !gate_) return;

  bool resyncClose = resyncAtCloseLimit_;
  bool resyncOpen = resyncAtOpenLimit_;
  resyncAtCloseLimit_ = false;
  resyncAtOpenLimit_ = false;

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
  long totalCounts = 0;
  if (cfg_->sensorsConfig.hall.enabled &&
      cfg_->sensorsConfig.hall.pin >= 0 &&
      maxDistanceMeters_ > 0.0f &&
      cfg_->gateConfig.wheelCircumference > 0.0f &&
      cfg_->gateConfig.pulsesPerRevolution > 0) {
    pulsesPerMeter = (float)cfg_->gateConfig.pulsesPerRevolution / cfg_->gateConfig.wheelCircumference;
    totalCounts = (long)(maxDistanceMeters_ * pulsesPerMeter);
  }

  if (resyncClose) {
    positionMeters_ = 0.0f;
    positionMetersRaw_ = 0.0f;
    if (totalCounts > 0) hallPosition_ = 0;
  }
  if (resyncOpen) {
    positionMeters_ = maxDistanceMeters_;
    positionMetersRaw_ = maxDistanceMeters_;
    if (totalCounts > 0) hallPosition_ = totalCounts;
  }

  if (calibrationRunning) {
    syncConfigPosition();
    positionMetersRaw_ = positionMeters_;
    syncGatePosition();
    return;
  }

  const bool preferHover = (cfg_->gateConfig.positionSource == "hoverboard_tel");
  const bool hallAvailable = (hallAttached_ && totalCounts > 0 && pulsesPerMeter > 0.0f);
  const bool hoverAvailable = (motor_ && motor_->isHoverUart() && motor_->hoverEnabled());

  if (!preferHover && hallAvailable) {
    long current = readHallCountAtomic();
    long delta = current - hallCountLast_;
    hallCountLast_ = current;
    if (gate_->isMoving() && delta != 0) {
      int dir = gate_->getLastDirection() >= 0 ? 1 : -1;
      hallPosition_ += delta * dir;
      if (hallPosition_ < 0) hallPosition_ = 0;
      if (hallPosition_ > totalCounts) hallPosition_ = totalCounts;
      positionMeters_ = (float)hallPosition_ / pulsesPerMeter;
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
      const float pos_raw = (float)tel.distMm / 1000.0f;
      float pos_raw_adj = pos_raw;
      if (!hoverOffsetValid_ && cfg_->gateConfig.hbOriginDistMm != 0) {
        hoverOffsetMeters_ = -((float)cfg_->gateConfig.hbOriginDistMm) / 1000.0f;
        hoverOffsetValid_ = true;
      }
      if (hoverOffsetValid_) {
        pos_raw_adj += hoverOffsetMeters_;
      }

      static bool posFilterInit = false;
      static float pos_f = 0.0f;
      static uint32_t lastTelMs = 0;

      float dt = 0.0f;
      if (lastTelMs != 0 && tel.lastTelMs >= (long)lastTelMs) {
        dt = (float)(tel.lastTelMs - (long)lastTelMs) / 1000.0f;
      }
      const float vMax_m_s = 1.5f;
      const float maxJump = (dt > 0.0f) ? (vMax_m_s * dt * 1.5f) : 0.20f;

      if (!posFilterInit) {
        pos_f = pos_raw_adj;
        posFilterInit = true;
      } else {
        const float diff = fabsf(pos_raw_adj - pos_f);
        if (diff <= maxJump) {
          const float alpha = 0.25f;
          pos_f = pos_f + alpha * (pos_raw_adj - pos_f);
        }
      }

      if (resyncClose || resyncOpen) {
        if (resyncClose) {
          hoverOffsetMeters_ = -pos_raw;
          hoverOffsetValid_ = true;
          cfg_->gateConfig.hbOriginDistMm = (int32_t)lroundf(pos_raw * 1000.0f);
        } else if (resyncOpen && maxDistanceMeters_ > 0.0f) {
          hoverOffsetMeters_ = maxDistanceMeters_ - pos_raw;
          hoverOffsetValid_ = true;
          cfg_->gateConfig.hbOriginDistMm = (int32_t)lroundf((pos_raw - maxDistanceMeters_) * 1000.0f);
        }
        if (hoverOffsetValid_) {
          pos_raw_adj = pos_raw + hoverOffsetMeters_;
          pos_f = pos_raw_adj;
          posFilterInit = true;
        }
      }

      lastTelMs = (uint32_t)tel.lastTelMs;

      positionMeters_ = pos_f;
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
        const float endEps = 0.02f;
        const float snapWindow = 0.35f;
        if (positionMeters_ <= endEps && fabsf(pos_raw_adj) <= snapWindow) {
          hoverOffsetMeters_ = -pos_raw;
          hoverOffsetValid_ = true;
          cfg_->gateConfig.hbOriginDistMm = (int32_t)lroundf(pos_raw * 1000.0f);
          pos_f = 0.0f;
          positionMeters_ = 0.0f;
          positionMetersRaw_ = 0.0f;
          posFilterInit = true;
        } else if (positionMeters_ >= (maxDistanceMeters_ - endEps) &&
                   fabsf(maxDistanceMeters_ - pos_raw_adj) <= snapWindow) {
          hoverOffsetMeters_ = maxDistanceMeters_ - pos_raw;
          hoverOffsetValid_ = true;
          cfg_->gateConfig.hbOriginDistMm = (int32_t)lroundf((pos_raw - maxDistanceMeters_) * 1000.0f);
          pos_f = maxDistanceMeters_;
          positionMeters_ = maxDistanceMeters_;
          positionMetersRaw_ = maxDistanceMeters_;
          posFilterInit = true;
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
    hallPps_ = 0.0f;
    hallPpsLastMs_ = nowMs;
    hallPpsLastCount_ = readHallCountAtomic();
    return;
  }
  if (hallPpsLastMs_ == 0) {
    hallPpsLastMs_ = nowMs;
    hallPpsLastCount_ = readHallCountAtomic();
    return;
  }
  uint32_t deltaMs = nowMs - hallPpsLastMs_;
  if (deltaMs < 1000) return;
  long count = readHallCountAtomic();
  long delta = count - hallPpsLastCount_;
  hallPps_ = deltaMs > 0 ? (float)delta / (deltaMs / 1000.0f) : 0.0f;
  hallPpsLastMs_ = nowMs;
  hallPpsLastCount_ = count;
}

void PositionTracker::maybePersistPosition(uint32_t nowMs) {
  if (!cfg_ || !gate_) return;
  const bool moving = gate_->isMoving();
  if (moving) {
    wasMovingLast_ = true;
    return;
  }
  if (wasMovingLast_) {
    persistDirty_ = true;
    wasMovingLast_ = false;
  }
  if (maxDistanceMeters_ <= 0.0f) return;
  if (lastPositionPersistMs_ != 0 && nowMs - lastPositionPersistMs_ < 1000) return;
  if (lastPersistedPosition_ >= 0.0f) {
    float diff = positionMeters_ - lastPersistedPosition_;
    if (diff < 0.0f) diff = -diff;
    if (diff >= 0.01f) persistDirty_ = true;
  }

  if (!persistDirty_) {
    // Safety net: persist occasionally even when no movement edge was detected.
    if (lastPositionPersistMs_ != 0 && (nowMs - lastPositionPersistMs_) < 60000) {
      return;
    }
  }

  if (!savePositionSnapshot()) {
    Serial.println("position snapshot save failed");
    return;
  }
  Serial.printf("[POS] snapshot saved pos=%.3fm max=%.3fm\n", positionMeters_, maxDistanceMeters_);
}

