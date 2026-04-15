#include "led_controller.h"
#include <FastLED.h>
#include <math.h>
#include <time.h>

namespace {
bool isStrappingPin(int pin) {
  const int pins[] = {0, 2, 4, 5, 12, 15};
  for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
    if (pin == pins[i]) return true;
  }
  return false;
}

uint8_t percentToByte(int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  return static_cast<uint8_t>((percent * 255 + 50) / 100);
}
} // namespace

LedController::LedController() {}

void LedController::init(const LedConfig& cfgIn) {
  applyConfig(cfgIn);
}

void LedController::applyConfig(const LedConfig& cfgIn) {
  cfg = cfgIn;
  enabled = cfg.enabled && cfg.type == "ws2812";
  pin = cfg.pin;
  count = cfg.count;
  brightness = cfg.brightness;
  ringStartIndex = cfg.ringStartIndex;
  ringReverse = cfg.ringReverse;
  animSpeed = cfg.animSpeed;
  nightEnabled = cfg.nightMode.enabled;
  nightBrightness = cfg.nightMode.brightness;
  nightFromMinutes = -1;
  nightToMinutes = -1;
  if (cfg.nightMode.from.length()) parseTimeToMinutes(cfg.nightMode.from, nightFromMinutes);
  if (cfg.nightMode.to.length()) parseTimeToMinutes(cfg.nightMode.to, nightToMinutes);

  defaultMode = parseMode(cfg.defaultMode);
  if (cfg.mode.length() > 0) {
    mode = parseMode(cfg.mode);
    modeOverride = true;
  } else {
    mode = defaultMode;
    modeOverride = false;
  }

  String orderStr = cfg.colorOrder;
  orderStr.toUpperCase();
  if (orderStr == "RGB") colorOrder = 1;
  else if (orderStr == "BRG") colorOrder = 2;
  else if (orderStr == "RBG") colorOrder = 3;
  else if (orderStr == "GBR") colorOrder = 4;
  else if (orderStr == "BGR") colorOrder = 5;
  else colorOrder = 0;

  if (!enabled || pin < 0 || count <= 0) {
    if (controller && leds) {
      clear();
      show();
    }
    enabled = false;
    return;
  }

  if (isStrappingPin(pin)) {
    Serial.printf("LED warn: pin %d is strapping\n", pin);
  }
  rebuildStrip();
  Serial.printf("LED init ok pin=%d count=%d\n", pin, count);
}

void LedController::setState(GateState state,
                             GateErrorCode err,
                             GateStopReason stopReason,
                             bool obstacle,
                             bool homing,
                             bool wifiConn,
                             bool mqttConn,
                             int positionPct,
                             bool apMode,
                             bool otaActiveIn) {
  GateState prevState = gateState;
  GateStopReason prevStopReason = lastStopReason;
  gateState = state;
  errorCode = err;
  lastStopReason = stopReason;
  wifiConnected = wifiConn;
  mqttConnected = mqttConn;
  positionPercent = positionPct;
  wifiApMode = apMode;
  otaActive = otaActiveIn;
  homingActive = homing;

  if (obstacle && !obstacleActive) {
    obstacleUntilMs = millis() + 2000;
    if (mode == MODE_STEALTH) {
      stealthAlertUntilMs = millis() + 1000;
    }
  }
  obstacleActive = obstacle;

  if ((err != GATE_ERR_NONE || state == GATE_ERROR) && mode == MODE_STEALTH) {
    stealthAlertUntilMs = millis() + 1000;
  }

  if (state == GATE_OPENING || state == GATE_CLOSING) {
    stopPulseUntilMs = 0;
    stopPulseReason = GATE_STOP_NONE;
  }

  const bool wasMoving = (prevState == GATE_OPENING || prevState == GATE_CLOSING);
  const bool isStopped = (state == GATE_STOPPED);
  if (wasMoving && isStopped && (stopReason != GATE_STOP_NONE || prevStopReason != stopReason)) {
    switch (stopReason) {
      case GATE_STOP_USER:
        startStopPulse(stopReason, 2400);
        break;
      case GATE_STOP_LIMIT_OPEN:
      case GATE_STOP_LIMIT_CLOSE:
        startStopPulse(stopReason, 900);
        break;
      case GATE_STOP_OBSTACLE:
        startStopPulse(stopReason, 1400);
        break;
      case GATE_STOP_ERROR:
        startStopPulse(stopReason, 1600);
        break;
      case GATE_STOP_TELEMETRY_TIMEOUT:
      case GATE_STOP_TELEMETRY_STALL:
      case GATE_STOP_OVER_CURRENT:
      case GATE_STOP_HOVER_FAULT:
        startStopPulse(stopReason, 1800);
        break;
      default:
        break;
    }
  }
}

void LedController::setHomingActive(bool active) {
  homingActive = active;
}

void LedController::setWifiModeAp(bool apMode) {
  wifiApMode = apMode;
}

void LedController::setLearnMode(bool enabledIn) {
  learnMode = enabledIn;
}

void LedController::setOtaActive(bool active) {
  otaActive = active;
  if (!active) {
    otaProgress = -1;
  }
}

void LedController::setOtaProgress(int percent) {
  otaProgress = percent;
}

void LedController::setFactoryCountdown(int seconds) {
  if (seconds <= 0) return;
  factoryActive = true;
  factorySeconds = seconds;
  factoryStartMs = millis();
}

void LedController::setOverride(const char* patternName, unsigned long durationMs) {
  LedPattern p = patternFromName(patternName);
  setOverridePattern(static_cast<int>(p), durationMs);
}

void LedController::setOverridePattern(int pattern, unsigned long durationMs) {
  overridePattern = static_cast<LedPattern>(pattern);
  overrideUntilMs = millis() + durationMs;
}

void LedController::setMode(const char* modeNameIn) {
  if (!modeNameIn || modeNameIn[0] == '\0') {
    modeOverride = false;
    mode = defaultMode;
    return;
  }
  modeOverride = true;
  mode = parseMode(String(modeNameIn));
}

void LedController::setBrightness(int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  brightness = percent;
}

void LedController::setRingOrientation(int startIndex, bool reverse) {
  ringStartIndex = startIndex;
  ringReverse = reverse;
}

void LedController::setEnabled(bool enabledIn) {
  enabled = enabledIn;
  if (!enabled) {
    clear();
    show();
  } else {
    rebuildStrip();
  }
}

void LedController::setMqttEnabled(bool enabledIn) {
  mqttEnabled = enabledIn;
}

void LedController::startTest() {
  if (!enabled) return;
  testActive = true;
  testStartMs = millis();
}

bool LedController::isReady() const {
  return enabled && controller && leds;
}

const char* LedController::getMode() const {
  return modeName(mode);
}

const char* LedController::getPattern() const {
  return patternName(currentPattern);
}

void LedController::fillStatus(JsonObject& out) const {
  out["enabled"] = enabled;
  out["pin"] = pin;
  out["count"] = count;
  out["brightness"] = brightness;
  out["brightnessApplied"] = (int)((((uint16_t)lastBrightness8) * 100U + 127U) / 255U);
  out["ringStartIndex"] = ringStartIndex;
  out["ringReverse"] = ringReverse;
  out["mode"] = modeName(mode);
  out["pattern"] = patternName(currentPattern);
  out["wifiAp"] = wifiApMode;
  out["ota"] = otaActive;
  out["learn"] = learnMode;
  out["nightActive"] = isNightActive();
}

void LedController::tick(uint32_t nowMs) {
  if (!enabled || !controller || !leds || count <= 0) return;

  if (overrideUntilMs > 0 && nowMs >= overrideUntilMs) {
    overrideUntilMs = 0;
  }
  if (factoryActive) {
    unsigned long elapsed = nowMs - factoryStartMs;
    if (elapsed >= static_cast<unsigned long>(factorySeconds * 1000)) {
      factoryActive = false;
    }
  }
  if (testActive) {
    const unsigned long testDuration = 1200 * 10;
    if (nowMs - testStartMs >= testDuration) {
      testActive = false;
    }
  }

  LedPattern desired = pickPattern(nowMs);
  bool force = desired != currentPattern;
  int interval = frameIntervalMs();
  if (!force && nowMs - lastFrameMs < static_cast<unsigned long>(interval)) return;

  applyBrightness(nowMs);
  renderPattern(desired, nowMs);
  show();
  currentPattern = desired;
  lastFrameMs = nowMs;
}

void LedController::rebuildStrip() {
  if (!enabled || pin < 0 || count <= 0) return;

  if (!leds || count > ledsCapacity) {
    delete[] leds;
    leds = new CRGB[count];
    ledsCapacity = count;
  }

  if (!controller) {
    switch (pin) {
      case 0: controller = &FastLED.addLeds<WS2812B, 0, GRB>(leds, count); break;
      case 1: controller = &FastLED.addLeds<WS2812B, 1, GRB>(leds, count); break;
      case 2: controller = &FastLED.addLeds<WS2812B, 2, GRB>(leds, count); break;
      case 3: controller = &FastLED.addLeds<WS2812B, 3, GRB>(leds, count); break;
      case 4: controller = &FastLED.addLeds<WS2812B, 4, GRB>(leds, count); break;
      case 5: controller = &FastLED.addLeds<WS2812B, 5, GRB>(leds, count); break;
      case 12: controller = &FastLED.addLeds<WS2812B, 12, GRB>(leds, count); break;
      case 13: controller = &FastLED.addLeds<WS2812B, 13, GRB>(leds, count); break;
      case 14: controller = &FastLED.addLeds<WS2812B, 14, GRB>(leds, count); break;
      case 15: controller = &FastLED.addLeds<WS2812B, 15, GRB>(leds, count); break;
      case 16: controller = &FastLED.addLeds<WS2812B, 16, GRB>(leds, count); break;
      case 17: controller = &FastLED.addLeds<WS2812B, 17, GRB>(leds, count); break;
      case 18: controller = &FastLED.addLeds<WS2812B, 18, GRB>(leds, count); break;
      case 19: controller = &FastLED.addLeds<WS2812B, 19, GRB>(leds, count); break;
      case 21: controller = &FastLED.addLeds<WS2812B, 21, GRB>(leds, count); break;
      case 22: controller = &FastLED.addLeds<WS2812B, 22, GRB>(leds, count); break;
      case 23: controller = &FastLED.addLeds<WS2812B, 23, GRB>(leds, count); break;
      case 25: controller = &FastLED.addLeds<WS2812B, 25, GRB>(leds, count); break;
      case 26: controller = &FastLED.addLeds<WS2812B, 26, GRB>(leds, count); break;
      case 27: controller = &FastLED.addLeds<WS2812B, 27, GRB>(leds, count); break;
      case 32: controller = &FastLED.addLeds<WS2812B, 32, GRB>(leds, count); break;
      case 33: controller = &FastLED.addLeds<WS2812B, 33, GRB>(leds, count); break;
      default:
        Serial.printf("LED error: pin %d unsupported for FastLED\n", pin);
        enabled = false;
        return;
    }
    activePin = pin;
  }

  if (activePin != pin) {
    Serial.printf("LED warn: pin change %d->%d requires reboot\n", activePin, pin);
  }

  controller->setLeds(leds, count);
  FastLED.setBrightness(percentToByte(brightness));
  clear();
  show();
}

CRGB LedController::color(uint8_t r, uint8_t g, uint8_t b) const {
  return applyOrder(r, g, b);
}

void LedController::clear() {
  if (!leds) return;
  fill_solid(leds, count, CRGB::Black);
}

void LedController::show() {
  FastLED.show();
}

void LedController::renderPattern(LedPattern pattern, uint32_t nowMs) {
  switch (pattern) {
    case PATTERN_IDLE: renderIdle(nowMs); break;
    case PATTERN_STOPPED: renderStopped(nowMs); break;
    case PATTERN_OPENING: renderProgress(nowMs, true); break;
    case PATTERN_CLOSING: renderProgress(nowMs, false); break;
    case PATTERN_HOMING: renderHoming(nowMs); break;
    case PATTERN_OBSTACLE: renderObstacle(nowMs); break;
    case PATTERN_ERROR: renderError(nowMs); break;
    case PATTERN_WIFI_AP: renderWifiAp(nowMs); break;
    case PATTERN_OTA: renderOta(nowMs); break;
    case PATTERN_LEARN: renderLearn(nowMs); break;
    case PATTERN_FACTORY: renderFactory(nowMs); break;
    case PATTERN_MQTT_DOWN: renderMqttDown(nowMs); break;
    case PATTERN_OVERRIDE_FLASH: renderOverrideFlash(nowMs); break;
    case PATTERN_REMOTE_OK:
      setAll(((nowMs / 110) % 2) == 0 ? color(0, 255, 110) : color(0, 25, 10));
      break;
    case PATTERN_REMOTE_REJECT:
      setAll(((nowMs / 110) % 2) == 0 ? color(255, 0, 70) : color(35, 0, 8));
      break;
    case PATTERN_BOOT:
      setAll(((nowMs / 130) % 2) == 0 ? color(180, 220, 255) : color(40, 70, 120));
      break;
    case PATTERN_COMMAND_REJECTED:
      setAll(((nowMs / 90) % 2) == 0 ? color(255, 0, 255) : color(70, 0, 30));
      break;
    case PATTERN_LIMIT_OPEN_HIT:
      clear();
      for (int i = 0; i < segmentTotal(); ++i) {
        int phys = mapIndex(i);
        if (phys >= 0) leds[phys] = ((nowMs / 90 + i) % 2) == 0 ? color(0, 255, 80) : color(0, 35, 10);
      }
      break;
    case PATTERN_LIMIT_CLOSE_HIT:
      clear();
      for (int i = 0; i < segmentTotal(); ++i) {
        int phys = mapIndex(i);
        if (phys >= 0) leds[phys] = ((nowMs / 90 + i) % 2) == 0 ? color(255, 80, 0) : color(45, 10, 0);
      }
      break;
    case PATTERN_TEST: renderTest(nowMs); break;
    case PATTERN_OFF:
    default:
      clear();
      break;
  }
}

void LedController::renderIdle(uint32_t nowMs) {
  (void)nowMs;
  // User requirement: no heartbeat / no light in idle.
  clear();
}

void LedController::renderStopped(uint32_t nowMs) {
  const bool stopPulseActive = stopPulseUntilMs > nowMs;
  if (stopPulseActive) {
    if (stopPulseReason == GATE_STOP_LIMIT_OPEN || stopPulseReason == GATE_STOP_LIMIT_CLOSE) {
      const unsigned long elapsed = nowMs - stopPulseStartMs;
      const int pulseMs = 120;
      const int gapMs = 120;
      const int pulses = 3;
      const int cycleMs = pulses * (pulseMs + gapMs);
      if (elapsed < (unsigned long)cycleMs) {
        int slot = (int)(elapsed / (pulseMs + gapMs));
        int within = (int)(elapsed - slot * (pulseMs + gapMs));
        if (within < pulseMs) {
          setAll(color(255, 0, 0));
        } else {
          clear();
        }
        return;
      }
    } else if (stopPulseReason == GATE_STOP_USER) {
      bool on = ((nowMs / 120) % 2) == 0;
      setAll(on ? color(255, 255, 255) : color(20, 20, 20));
      return;
    } else if (stopPulseReason == GATE_STOP_OBSTACLE) {
      bool on = ((nowMs / 140) % 2) == 0;
      setAll(on ? color(255, 220, 0) : color(80, 60, 0));
      return;
    } else if (stopPulseReason == GATE_STOP_ERROR) {
      bool on = ((nowMs / 120) % 2) == 0;
      setAll(on ? color(255, 80, 0) : color(180, 20, 0));
      return;
    } else if (stopPulseReason == GATE_STOP_TELEMETRY_TIMEOUT ||
               stopPulseReason == GATE_STOP_TELEMETRY_STALL ||
               stopPulseReason == GATE_STOP_OVER_CURRENT ||
               stopPulseReason == GATE_STOP_HOVER_FAULT) {
      bool on = ((nowMs / 100) % 2) == 0;
      if (on) {
        setAll(color(255, 0, 0));
      } else {
        clear();
      }
      return;
    }
  }
  // After stop pulse -> fully off.
  clear();
}

void LedController::renderProgress(uint32_t nowMs, bool opening) {
  clear();
  int total = segmentTotal();
  if (total <= 0) return;

  int pct = positionPercent;
  if (pct < 0) pct = 50;
  if (pct > 100) pct = 100;

  const int closedPct = 100 - pct;
  const int filled = (closedPct * total + 99) / 100;
  for (int idx = 0; idx < total; ++idx) {
    const int phys = mapIndex(idx);
    if (phys < 0) continue;
    if (idx < filled) {
      leds[phys] = opening ? color(0, 18, 6) : color(48, 6, 0);
    } else {
      leds[phys] = opening ? color(0, 6, 2) : color(8, 0, 0);
    }
  }

  const int speed = max(5, 120 - animSpeed);
  const int step = (nowMs / speed) % total;
  const int headPos = opening ? (total - 1 - step) : step;
  const CRGB headColor = opening ? color(40, 255, 140) : color(255, 55, 0);
  const CRGB midColor = opening ? color(0, 120, 70) : color(150, 20, 0);
  const CRGB tailColor = opening ? color(0, 40, 24) : color(60, 6, 0);

  for (int dist = 0; dist < 3; ++dist) {
    int pos = opening ? (headPos + dist) % total : (headPos - dist + total) % total;
    int phys = mapIndex(pos);
    if (phys < 0) continue;
    leds[phys] = (dist == 0) ? headColor : (dist == 1 ? midColor : tailColor);
  }
}

void LedController::renderHoming(uint32_t nowMs) {
  clear();
  int total = segmentTotal();
  if (total <= 0) return;

  int speed = max(6, 140 - animSpeed);
  int headA = (nowMs / speed) % total;
  int headB = (headA + total / 2) % total;

  for (int idx = 0; idx < total; ++idx) {
    int phys = mapIndex(idx);
    if (phys < 0) continue;
    leds[phys] = ((idx + (nowMs / 180)) % 2 == 0) ? color(0, 14, 24) : color(10, 8, 0);
  }

  int physA = mapIndex(headA);
  int physB = mapIndex(headB);
  if (physA >= 0) leds[physA] = color(0, 255, 200);
  if (physB >= 0) leds[physB] = color(255, 140, 0);
}

void LedController::renderObstacle(uint32_t nowMs) {
  clear();
  int total = segmentTotal();
  if (total <= 0) return;
  bool phase = ((nowMs / 120) % 2) == 0;
  for (int idx = 0; idx < total; ++idx) {
    int phys = mapIndex(idx);
    if (phys < 0) continue;
    bool on = ((idx % 2) == 0) ? phase : !phase;
    leds[phys] = on ? color(255, 220, 0) : color(90, 55, 0);
  }
}

void LedController::renderError(uint32_t nowMs) {
  CRGB cOn = color(255, 0, 0);
  CRGB cOff = color(20, 0, 0);
  switch (errorCode) {
    case GATE_ERR_HOVER_TEL_TIMEOUT:
    case GATE_ERR_TIMEOUT:
      cOn = color(180, 0, 255);      // telemetry timeout -> purple
      cOff = color(35, 0, 45);
      break;
    case GATE_ERR_HOVER_FAULT:
    case GATE_ERR_HOVER_OFFLINE:
      cOn = color(255, 0, 0);        // hover fault -> alarm red
      cOff = color(30, 0, 0);
      break;
    case GATE_ERR_OVER_CURRENT:
      cOn = color(255, 120, 0);      // overcurrent -> orange
      cOff = color(40, 12, 0);
      break;
    case GATE_ERR_LIMITS_INVALID:
      cOn = color(160, 0, 220);      // config/limits -> violet
      cOff = color(24, 0, 34);
      break;
    default:
      cOn = color(255, 0, 0);
      cOff = color(24, 0, 0);
      break;
  }

  int code = static_cast<int>(errorCode);
  if (code <= 0) code = 1;
  const int pulseMs = 180;
  const int gapMs = 180;
  const int pauseMs = 800;
  int cycleMs = code * (pulseMs + gapMs) + pauseMs;
  int t = nowMs % cycleMs;
  if (t < code * (pulseMs + gapMs)) {
    int slot = t / (pulseMs + gapMs);
    int within = t - slot * (pulseMs + gapMs);
    if (within < pulseMs) {
      setAll(cOn);
    } else {
      setAll(cOff);
    }
  } else {
    clear();
  }
}

void LedController::renderWifiAp(uint32_t nowMs) {
  clear();
  int total = segmentTotal();
  if (total <= 0) return;
  int speed = max(5, 150 - animSpeed);
  int pos = (nowMs / speed) % total;
  int segCount = segmentCount();
  int globalIdx = 0;
  for (int s = 0; s < segCount; ++s) {
    int start = 0;
    int len = 0;
    segmentAt(s, start, len);
    for (int i = 0; i < len; ++i) {
      int dist = abs(globalIdx - pos);
      if (dist <= 2) {
        uint8_t level = dist == 0 ? 200 : (dist == 1 ? 120 : 60);
        leds[start + i] = color(level, 0, level);
      }
      globalIdx++;
    }
  }
}

void LedController::renderOta(uint32_t nowMs) {
  clear();
  int total = segmentTotal();
  if (total <= 0) return;
  if (otaProgress >= 0) {
    int lit = (otaProgress * total) / 100;
    for (int globalIdx = 0; globalIdx < total; ++globalIdx) {
      if (globalIdx < lit) {
        int phys = mapIndex(globalIdx);
        if (phys >= 0) leds[phys] = color(0, 120, 255);
      }
    }
    if ((nowMs / 300) % 2 == 0 && lit < total) {
      int pos = lit;
      int phys = mapIndex(pos);
      if (phys >= 0) leds[phys] = color(0, 180, 255);
    }
    return;
  }
  int speed = max(6, 120 - animSpeed);
  int pos = (nowMs / speed) % total;
  int phys = mapIndex(pos);
  if (phys >= 0) leds[phys] = color(0, 120, 255);
}

void LedController::renderLearn(uint32_t nowMs) {
  clear();
  int total = segmentTotal();
  if (total <= 0) return;
  int speed = max(5, 110 - animSpeed);
  int head = (nowMs / speed) % total;
  for (int idx = 0; idx < total; ++idx) {
    int phys = mapIndex(idx);
    if (phys < 0) continue;
    leds[phys] = color(10, 0, 26);
  }
  for (int dist = 0; dist < 3; ++dist) {
    int pos = (head - dist + total) % total;
    int phys = mapIndex(pos);
    if (phys < 0) continue;
    leds[phys] = dist == 0 ? color(150, 0, 255)
                           : (dist == 1 ? color(50, 0, 90) : color(18, 0, 30));
  }
  int sparkle = mapIndex((head + total / 2) % total);
  if (sparkle >= 0 && ((nowMs / 120) % 2) == 0) {
    leds[sparkle] = color(180, 220, 255);
  }
}

void LedController::renderFactory(uint32_t nowMs) {
  clear();
  if (!factoryActive || factorySeconds <= 0) return;
  int total = segmentTotal();
  if (total <= 0) return;
  int segs = 3;
  int segLen = max(1, total / segs);
  unsigned long elapsed = nowMs - factoryStartMs;
  int remaining = factorySeconds - static_cast<int>(elapsed / 1000);
  if (remaining < 0) remaining = 0;
  int globalIdx = 0;
  for (int s = 0; s < segs; ++s) {
    bool lit = s < remaining;
    for (int i = 0; i < segLen && globalIdx < total; ++i) {
      if (lit) {
        int phys = mapIndex(globalIdx);
        if (phys >= 0) leds[phys] = color(255, 0, 0);
      }
      globalIdx++;
    }
  }
  if ((nowMs / 200) % 2 == 0 && remaining > 0) {
    int pos = (remaining - 1) * segLen;
    if (pos >= 0 && pos < total) {
      int phys = mapIndex(pos);
      if (phys >= 0) leds[phys] = color(255, 120, 120);
    }
  }
}

void LedController::renderMqttDown(uint32_t nowMs) {
  int t = nowMs % 2000;
  if (t < 120 || (t > 240 && t < 360)) {
    setAll(color(255, 140, 0));
  } else {
    clear();
  }
}

void LedController::renderOverrideFlash(uint32_t nowMs) {
  bool on = ((nowMs / 120) % 2) == 0;
  setAll(on ? color(0, 255, 255) : color(0, 30, 30));
}

void LedController::renderTest(uint32_t nowMs) {
  static const LedPattern patterns[] = {
    PATTERN_IDLE,
    PATTERN_OPENING,
    PATTERN_CLOSING,
    PATTERN_STOPPED,
    PATTERN_OBSTACLE,
    PATTERN_ERROR,
    PATTERN_WIFI_AP,
    PATTERN_MQTT_DOWN,
    PATTERN_LEARN,
    PATTERN_OTA
  };
  const int countPatterns = sizeof(patterns) / sizeof(patterns[0]);
  unsigned long elapsed = nowMs - testStartMs;
  int idx = static_cast<int>(elapsed / 1200);
  if (idx >= countPatterns) {
    testActive = false;
    renderIdle(nowMs);
    return;
  }
  renderPattern(patterns[idx], nowMs);
}

void LedController::applyBrightness(uint32_t nowMs) {
  int target = brightness;
  if (nightEnabled && isNightActive()) {
    target = nightBrightness;
  }
  uint8_t value = percentToByte(target);
  if (value != lastBrightness8) {
    lastBrightness8 = value;
    FastLED.setBrightness(value);
    lastFrameMs = nowMs;
  }
}

bool LedController::isNightActive() const {
  if (!nightEnabled || nightFromMinutes < 0 || nightToMinutes < 0) return false;
  time_t now = time(nullptr);
  if (now < 1600000000) return false;
  struct tm tmNow;
  localtime_r(&now, &tmNow);
  int minutes = tmNow.tm_hour * 60 + tmNow.tm_min;
  if (nightFromMinutes <= nightToMinutes) {
    return minutes >= nightFromMinutes && minutes < nightToMinutes;
  }
  return minutes >= nightFromMinutes || minutes < nightToMinutes;
}

bool LedController::parseTimeToMinutes(const String& value, int& minutes) const {
  if (value.length() < 4) return false;
  int sep = value.indexOf(':');
  if (sep < 0) return false;
  int h = value.substring(0, sep).toInt();
  int m = value.substring(sep + 1).toInt();
  if (h < 0 || h > 23 || m < 0 || m > 59) return false;
  minutes = h * 60 + m;
  return true;
}

int LedController::frameIntervalMs() const {
  int speed = animSpeed;
  if (speed < 1) speed = 1;
  if (speed > 100) speed = 100;
  return 140 - speed;
}

LedController::ModeKind LedController::parseMode(const String& value) const {
  String v = value;
  v.toLowerCase();
  if (v == "off") return MODE_OFF;
  if (v == "stealth") return MODE_STEALTH;
  if (v == "idle") return MODE_IDLE;
  if (v == "stopped") return MODE_STOPPED;
  return MODE_STATUS;
}

const char* LedController::modeName(ModeKind modeKind) const {
  switch (modeKind) {
    case MODE_OFF: return "off";
    case MODE_STEALTH: return "stealth";
    case MODE_IDLE: return "idle";
    case MODE_STOPPED: return "stopped";
    case MODE_STATUS:
    default:
      return "status";
  }
}

LedController::LedPattern LedController::patternFromName(const char* name) const {
  if (!name) return PATTERN_OVERRIDE_FLASH;
  String v(name);
  v.toLowerCase();
  if (v == "off") return PATTERN_OFF;
  if (v == "idle") return PATTERN_IDLE;
  if (v == "stopped") return PATTERN_STOPPED;
  if (v == "opening") return PATTERN_OPENING;
  if (v == "closing") return PATTERN_CLOSING;
  if (v == "homing") return PATTERN_HOMING;
  if (v == "obstacle") return PATTERN_OBSTACLE;
  if (v == "error") return PATTERN_ERROR;
  if (v == "wifi_ap") return PATTERN_WIFI_AP;
  if (v == "ota") return PATTERN_OTA;
  if (v == "learn") return PATTERN_LEARN;
  if (v == "factory") return PATTERN_FACTORY;
  if (v == "mqtt") return PATTERN_MQTT_DOWN;
  if (v == "flash") return PATTERN_OVERRIDE_FLASH;
  if (v == "remote_ok" || v == "success") return PATTERN_REMOTE_OK;
  if (v == "remote_reject" || v == "reject") return PATTERN_REMOTE_REJECT;
  if (v == "boot") return PATTERN_BOOT;
  if (v == "command_rejected" || v == "cmd_reject") return PATTERN_COMMAND_REJECTED;
  if (v == "limit_open_hit" || v == "limit_open") return PATTERN_LIMIT_OPEN_HIT;
  if (v == "limit_close_hit" || v == "limit_close") return PATTERN_LIMIT_CLOSE_HIT;
  return PATTERN_OVERRIDE_FLASH;
}

const char* LedController::patternName(LedPattern pattern) const {
  switch (pattern) {
    case PATTERN_IDLE: return "idle";
    case PATTERN_STOPPED: return "stopped";
    case PATTERN_OPENING: return "opening";
    case PATTERN_CLOSING: return "closing";
    case PATTERN_HOMING: return "homing";
    case PATTERN_OBSTACLE: return "obstacle";
    case PATTERN_ERROR: return "error";
    case PATTERN_WIFI_AP: return "wifi_ap";
    case PATTERN_OTA: return "ota";
    case PATTERN_LEARN: return "learn";
    case PATTERN_FACTORY: return "factory";
    case PATTERN_MQTT_DOWN: return "mqtt_down";
    case PATTERN_OVERRIDE_FLASH: return "flash";
    case PATTERN_REMOTE_OK: return "remote_ok";
    case PATTERN_REMOTE_REJECT: return "remote_reject";
    case PATTERN_BOOT: return "boot";
    case PATTERN_COMMAND_REJECTED: return "command_rejected";
    case PATTERN_LIMIT_OPEN_HIT: return "limit_open_hit";
    case PATTERN_LIMIT_CLOSE_HIT: return "limit_close_hit";
    case PATTERN_TEST: return "test";
    case PATTERN_OFF:
    default:
      return "off";
  }
}

LedController::LedPattern LedController::pickPattern(uint32_t nowMs) {
  if (testActive) return PATTERN_TEST;
  if (factoryActive) return PATTERN_FACTORY;
  if (otaActive) return PATTERN_OTA;

  // Priority required by user: ERROR > OBSTACLE > LIMIT/REJECT FLASH > MOTION > IDLE_OFF
  if (errorCode != GATE_ERR_NONE || gateState == GATE_ERROR) return PATTERN_ERROR;
  if (obstacleActive) return PATTERN_OBSTACLE;
  if (obstacleUntilMs > nowMs) return PATTERN_OBSTACLE;

  if (overrideUntilMs > nowMs &&
      (overridePattern == PATTERN_LIMIT_OPEN_HIT ||
       overridePattern == PATTERN_LIMIT_CLOSE_HIT ||
       overridePattern == PATTERN_COMMAND_REJECTED)) {
    return overridePattern;
  }

  // Remote result pulse should be visible even if learn mode remains active.
  if (overrideUntilMs > nowMs &&
      (overridePattern == PATTERN_REMOTE_OK || overridePattern == PATTERN_REMOTE_REJECT)) {
    return overridePattern;
  }

  if (learnMode) return PATTERN_LEARN;

  if (overrideUntilMs > nowMs) return overridePattern;
  if (stopPulseUntilMs > nowMs) return PATTERN_STOPPED;
  if (homingActive) return PATTERN_HOMING;

  ModeKind effectiveMode = modeOverride ? mode : defaultMode;
  if (effectiveMode == MODE_OFF) return PATTERN_OFF;
  if (effectiveMode == MODE_STEALTH) {
    if (stealthAlertUntilMs > nowMs) return PATTERN_ERROR;
    return PATTERN_OFF;
  }

  if (gateState == GATE_OPENING) return PATTERN_OPENING;
  if (gateState == GATE_CLOSING) return PATTERN_CLOSING;

  // No heartbeat in idle.
  if (effectiveMode == MODE_STOPPED || effectiveMode == MODE_IDLE || effectiveMode == MODE_STATUS) {
    return PATTERN_OFF;
  }
  return PATTERN_OFF;
}

void LedController::startStopPulse(GateStopReason reason, unsigned long durationMs) {
  stopPulseReason = reason;
  stopPulseStartMs = millis();
  stopPulseUntilMs = stopPulseStartMs + durationMs;
}

void LedController::setAll(const CRGB& c) {
  if (!leds) return;
  fill_solid(leds, count, c);
}

int LedController::segmentCount() const {
  if (cfg.segmentCount > 0) return cfg.segmentCount;
  return 1;
}

void LedController::segmentAt(int idx, int& start, int& length) const {
  if (cfg.segmentCount > 0) {
    if (idx < 0 || idx >= cfg.segmentCount) {
      start = 0;
      length = 0;
      return;
    }
    start = cfg.segments[idx].start;
    length = cfg.segments[idx].length;
  } else {
    start = 0;
    length = count;
  }
  if (start < 0) start = 0;
  if (length < 0) length = 0;
  if (start > count) {
    length = 0;
    return;
  }
  if (start + length > count) length = count - start;
}

int LedController::segmentTotal() const {
  if (cfg.segmentCount <= 0) return count;
  int total = 0;
  for (int i = 0; i < cfg.segmentCount; ++i) {
    int start = 0;
    int len = 0;
    segmentAt(i, start, len);
    total += len;
  }
  return total;
}

int LedController::mapIndex(int globalIndex) const {
  if (globalIndex < 0) return -1;

  int total = segmentTotal();
  if (total <= 0) return -1;
  int idx = globalIndex % total;
  if (idx < 0) idx += total;

  if (ringReverse) {
    idx = total - 1 - idx;
  }

  int offset = ringStartIndex % total;
  if (offset < 0) offset += total;
  idx = (idx + offset) % total;

  int accum = 0;
  int segCount = segmentCount();
  for (int i = 0; i < segCount; ++i) {
    int start = 0;
    int len = 0;
    segmentAt(i, start, len);
    if (idx < accum + len) {
      return start + (idx - accum);
    }
    accum += len;
  }
  return -1;
}

CRGB LedController::applyOrder(uint8_t r, uint8_t g, uint8_t b) const {
  switch (colorOrder) {
    case 1: return CRGB(g, r, b); // RGB
    case 2: return CRGB(r, b, g); // BRG
    case 3: return CRGB(b, r, g); // RBG
    case 4: return CRGB(b, g, r); // GBR
    case 5: return CRGB(g, b, r); // BGR
    case 0:
    default:
      return CRGB(r, g, b); // GRB default
  }
}
