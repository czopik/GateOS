#include "input_manager.h"

void InputManager::DebouncedInput::begin(int pin_, bool invert_, unsigned long debounceMs_, int pullMode) {
  pin = pin_;
  invert = invert_;
  debounceMs = debounceMs_;
  if (pin >= 0) {
    if (pullMode == 2) pinMode(pin, INPUT_PULLDOWN);
    else if (pullMode == 1) pinMode(pin, INPUT_PULLUP);
    else pinMode(pin, INPUT);
  }
  lastRaw = readRaw();
  stableState = lastRaw;
  lastChange = millis();
}

bool InputManager::DebouncedInput::readRaw() const {
  if (pin < 0) return false;
  bool raw = digitalRead(pin) == HIGH;
  return invert ? !raw : raw;
}

bool InputManager::DebouncedInput::update() {
  if (pin < 0) return false;
  bool raw = readRaw();
  if (raw != lastRaw) {
    lastRaw = raw;
    lastChange = millis();
  }
  if (stableState != lastRaw && millis() - lastChange >= debounceMs) {
    stableState = lastRaw;
    return true;
  }
  return false;
}

int InputManager::parsePullMode(const String& mode) const {
  if (mode == "down") return 2;
  if (mode == "none") return 0;
  return 1;
}

void InputManager::begin(const ConfigManager& cfg) {
  const auto& limOpen = cfg.limitsConfig.open;
  const auto& limClose = cfg.limitsConfig.close;
  if (cfg.limitsConfig.enabled) {
    limitOpenInput.begin(limOpen.pin, limOpen.invert, limOpen.debounceMs, parsePullMode(limOpen.pullMode));
    limitCloseInput.begin(limClose.pin, limClose.invert, limClose.debounceMs, parsePullMode(limClose.pullMode));
  } else {
    limitOpenInput.begin(-1, false, limOpen.debounceMs, parsePullMode(limOpen.pullMode));
    limitCloseInput.begin(-1, false, limClose.debounceMs, parsePullMode(limClose.pullMode));
  }

  stopInput.begin(cfg.gpioConfig.stopPin, false, 30, 1);

  const auto& photo = cfg.sensorsConfig.photocell;
  int photoPin = photo.pin;
  bool photoInvert = photo.invert;
  int photoPull = parsePullMode(photo.pullMode);
  if (photoPin < 0) {
    photoPin = cfg.gpioConfig.obstaclePin;
    photoInvert = cfg.gpioConfig.obstacleInvert;
    photoPull = 1;
  }
  obstacleInput.begin(photoPin, photoInvert, photo.debounceMs, photoPull);
  buttonInput.begin(cfg.gpioConfig.buttonPin, cfg.gpioConfig.buttonInvert, 50, 1);

  limitsInvalidSinceMs = 0;
  limitsInvalidLatched = false;
}

InputEvents InputManager::poll(const ConfigManager& cfg, uint32_t nowMs) {
  InputEvents ev;

  const bool limitsEnabled = cfg.limitsConfig.enabled;
  const bool limitOpenEnabled = limitsEnabled && cfg.limitsConfig.open.enabled;
  const bool limitCloseEnabled = limitsEnabled && cfg.limitsConfig.close.enabled;

  if (stopInput.update() && stopInput.isActive()) {
    ev.stopPressed = true;
  }

  if (obstacleInput.update()) {
    ev.obstacleChanged = true;
    ev.obstacleActive = obstacleInput.isActive();
  }

  bool openChanged = limitOpenEnabled ? limitOpenInput.update() : false;
  bool closeChanged = limitCloseEnabled ? limitCloseInput.update() : false;
  bool openActive = limitOpenEnabled && limitOpenInput.isActive();
  bool closeActive = limitCloseEnabled && limitCloseInput.isActive();
  bool both = openActive && closeActive;

  if (limitsEnabled && both) {
    if (limitsInvalidSinceMs == 0) limitsInvalidSinceMs = nowMs;
    if (nowMs - limitsInvalidSinceMs >= 200) {
      ev.limitsInvalid = true;
      if (!limitsInvalidLatched) {
        ev.limitsInvalidEdge = true;
      }
      limitsInvalidLatched = true;
    }
  } else {
    limitsInvalidSinceMs = 0;
    limitsInvalidLatched = false;
  }

  ev.limitOpenRising = limitOpenEnabled && openChanged && openActive;
  ev.limitCloseRising = limitCloseEnabled && closeChanged && closeActive;

  if (buttonInput.update() && buttonInput.isActive()) {
    ev.buttonPressed = true;
  }

  return ev;
}

bool InputManager::limitOpenRaw() const { return limitOpenInput.isActive(); }
bool InputManager::limitCloseRaw() const { return limitCloseInput.isActive(); }
bool InputManager::limitOpenActive(const ConfigManager& cfg) const {
  return cfg.limitsConfig.enabled && cfg.limitsConfig.open.enabled && limitOpenInput.isActive();
}
bool InputManager::limitCloseActive(const ConfigManager& cfg) const {
  return cfg.limitsConfig.enabled && cfg.limitsConfig.close.enabled && limitCloseInput.isActive();
}
bool InputManager::stopActive() const { return stopInput.isActive(); }
bool InputManager::obstacleActive() const { return obstacleInput.isActive(); }
bool InputManager::buttonActive() const { return buttonInput.isActive(); }

