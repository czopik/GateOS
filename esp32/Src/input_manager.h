#pragma once

#include <Arduino.h>
#include "config_manager.h"

struct InputEvents {
  bool stopPressed = false;
  bool obstacleChanged = false;
  bool obstacleActive = false;
  bool limitOpenRising = false;
  bool limitCloseRising = false;
  bool limitsInvalid = false;
  bool limitsInvalidEdge = false;
  bool buttonPressed = false;
};

class InputManager {
public:
  void begin(const ConfigManager& cfg);
  InputEvents poll(const ConfigManager& cfg, uint32_t nowMs);

  bool limitOpenRaw() const;
  bool limitCloseRaw() const;
  bool limitOpenActive(const ConfigManager& cfg) const;
  bool limitCloseActive(const ConfigManager& cfg) const;
  bool stopActive() const;
  bool obstacleActive() const;
  bool buttonActive() const;

private:
  struct DebouncedInput {
    int pin = -1;
    bool invert = false;
    unsigned long debounceMs = 30;
    bool stableState = false;
    bool lastRaw = false;
    unsigned long lastChange = 0;

    void begin(int pin_, bool invert_, unsigned long debounceMs_, int pullMode);
    bool readRaw() const;
    bool update();
    bool isActive() const { return stableState; }
  };

  int parsePullMode(const String& mode) const;
  bool limitsReady(uint32_t nowMs) const;

  DebouncedInput limitOpenInput;
  DebouncedInput limitCloseInput;
  DebouncedInput stopInput;
  DebouncedInput obstacleInput;
  DebouncedInput buttonInput;

  unsigned long inputsReadyAtMs = 0;
  unsigned long limitsInvalidSinceMs = 0;
  bool limitsInvalidLatched = false;
};

