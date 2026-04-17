#pragma once

#include <Arduino.h>
#include "config_manager.h"

class MotorController;
class GateController;

class PositionTracker {
public:
  void begin(ConfigManager* cfg, MotorController* motor, GateController* gate);

  void initializeFromConfig();
  bool applyMaxDistance(float value, bool persist);
  bool calibrateToMode(const char* mode, bool calibrationRunning);

  void requestResyncOpen();
  void requestResyncClose();

  void updateHallAttachment(bool calibrationRunning);
  void updatePosition(bool calibrationRunning);
  void updateHallStats(uint32_t nowMs);
  void maybePersistPosition(uint32_t nowMs);

  float positionMeters() const { return positionMeters_; }
  float positionMetersRaw() const { return positionMetersRaw_; }
  float maxDistanceMeters() const { return maxDistanceMeters_; }
  int positionPercent() const { return positionPercent_; }
  float hallPps() const { return hallPps_; }

private:
  static PositionTracker* instance_;
  static void IRAM_ATTR hallIsrThunk();
  void IRAM_ATTR onHallIsr();

  long readHallCountAtomic() const;
  long readAndConsumeHallDeltaAtomic();
  int parsePullMode(const String& mode) const;
  void syncConfigPosition();
  void syncGatePosition();
  bool loadPositionSnapshot();
  bool savePositionSnapshot();

  ConfigManager* cfg_ = nullptr;
  MotorController* motor_ = nullptr;
  GateController* gate_ = nullptr;

  volatile long hallCount_ = 0;
  portMUX_TYPE hallMux_ = portMUX_INITIALIZER_UNLOCKED;
  long hallCountLast_ = 0;
  long hallPosition_ = 0;
  volatile uint32_t hallLastIsrUs_ = 0;
  volatile uint32_t hallDebounceUs_ = 0;
  float hallPps_ = 0.0f;
  unsigned long hallPpsLastMs_ = 0;
  long hallPpsLastCount_ = 0;
  int hallPinActive_ = -1;
  bool hallAttached_ = false;

  // Hover filter state (reset in initializeFromConfig)
  bool posFilterInit_ = false;
  float pos_f_ = 0.0f;
  uint32_t lastTelMsFilter_ = 0;

  int positionPercent_ = -1;
  float positionMeters_ = 0.0f;
  float positionMetersRaw_ = 0.0f;
  float hoverOffsetMeters_ = 0.0f;
  bool hoverOffsetValid_ = false;
  float maxDistanceMeters_ = 0.0f;
  unsigned long lastPositionPersistMs_ = 0;
  float lastPersistedPosition_ = -1.0f;
  bool wasMovingLast_ = false;
  bool persistDirty_ = false;

  bool resyncAtOpenLimit_ = false;
  bool resyncAtCloseLimit_ = false;
};

