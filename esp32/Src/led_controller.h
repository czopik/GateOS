#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "config_manager.h"
#include "gate_controller.h"

struct CRGB;
class CLEDController;

class LedController {
public:
  LedController();

  void init(const LedConfig& cfg);
  void applyConfig(const LedConfig& cfg);

  void setState(GateState state,
                GateErrorCode err,
                GateStopReason stopReason,
                bool obstacle,
                bool homing,
                bool wifiConnected,
                bool mqttConnected,
                int positionPercent,
                bool apMode,
                bool otaActive);
  void setHomingActive(bool active);
  void setWifiModeAp(bool apMode);
  void setLearnMode(bool enabled);
  void setOtaActive(bool active);
  void setOtaProgress(int percent);
  void setFactoryCountdown(int seconds);
  void setOverride(const char* patternName, unsigned long durationMs);
  void setOverridePattern(int pattern, unsigned long durationMs);
  void setMode(const char* modeName);
  void setBrightness(int percent);
  void setRingOrientation(int startIndex, bool reverse);
  void setEnabled(bool enabled);
  void setMqttEnabled(bool enabled);
  void startTest();

  void tick(uint32_t nowMs);

  bool isReady() const;
  bool isEnabled() const { return enabled; }
  int getPin() const { return pin; }
  int getCount() const { return count; }
  int getBrightness() const { return brightness; }
  int getRingStartIndex() const { return ringStartIndex; }
  bool getRingReverse() const { return ringReverse; }
  const char* getMode() const;
  const char* getPattern() const;
  void fillStatus(JsonObject& out) const;

private:
  enum LedPattern {
    PATTERN_OFF = 0,
    PATTERN_IDLE,
    PATTERN_STOPPED,
    PATTERN_OPENING,
    PATTERN_CLOSING,
    PATTERN_HOMING,
    PATTERN_OBSTACLE,
    PATTERN_ERROR,
    PATTERN_WIFI_AP,
    PATTERN_OTA,
    PATTERN_LEARN,
    PATTERN_FACTORY,
    PATTERN_MQTT_DOWN,
    PATTERN_OVERRIDE_FLASH,
    PATTERN_REMOTE_OK,
    PATTERN_REMOTE_REJECT,
    PATTERN_BOOT,
    PATTERN_COMMAND_REJECTED,
    PATTERN_LIMIT_OPEN_HIT,
    PATTERN_LIMIT_CLOSE_HIT,
    PATTERN_TEST
  };

  enum ModeKind {
    MODE_STATUS = 0,
    MODE_OFF,
    MODE_STEALTH,
    MODE_IDLE,
    MODE_STOPPED
  };

  LedConfig cfg;
  bool enabled = false;
  int pin = -1;
  int count = 0;
  int brightness = 0;
  int ringStartIndex = 0;
  bool ringReverse = false;
  int animSpeed = 50;
  ModeKind defaultMode = MODE_STATUS;
  ModeKind mode = MODE_STATUS;
  bool modeOverride = false;
  bool mqttEnabled = false;

  bool wifiConnected = false;
  bool mqttConnected = false;
  bool wifiApMode = false;
  bool learnMode = false;
  bool otaActive = false;
  int otaProgress = -1;

  GateState gateState = GATE_STOPPED;
  GateErrorCode errorCode = GATE_ERR_NONE;
  GateStopReason lastStopReason = GATE_STOP_NONE;
  GateStopReason stopPulseReason = GATE_STOP_NONE;
  bool obstacleActive = false;
  bool homingActive = false;
  int positionPercent = -1;

  unsigned long lastFrameMs = 0;
  unsigned long patternStartMs = 0;
  LedPattern currentPattern = PATTERN_OFF;

  LedPattern overridePattern = PATTERN_OFF;
  unsigned long overrideUntilMs = 0;

  unsigned long obstacleUntilMs = 0;
  unsigned long stopPulseUntilMs = 0;
  unsigned long stopPulseStartMs = 0;
  unsigned long stealthAlertUntilMs = 0;

  bool testActive = false;
  unsigned long testStartMs = 0;

  bool factoryActive = false;
  unsigned long factoryStartMs = 0;
  int factorySeconds = 0;

  int nightFromMinutes = -1;
  int nightToMinutes = -1;
  int nightBrightness = 0;
  bool nightEnabled = false;

  CRGB* leds = nullptr;
  CLEDController* controller = nullptr;
  int activePin = -1;
  int ledsCapacity = 0;
  uint8_t lastBrightness8 = 255;
  uint8_t colorOrder = 0;

  void rebuildStrip();
  CRGB color(uint8_t r, uint8_t g, uint8_t b) const;
  void clear();
  void show();
  void renderPattern(LedPattern pattern, uint32_t nowMs);
  void renderIdle(uint32_t nowMs);
  void renderStopped(uint32_t nowMs);
  void renderProgress(uint32_t nowMs, bool opening);
  void renderHoming(uint32_t nowMs);
  void renderObstacle(uint32_t nowMs);
  void renderError(uint32_t nowMs);
  void renderWifiAp(uint32_t nowMs);
  void renderOta(uint32_t nowMs);
  void renderLearn(uint32_t nowMs);
  void renderFactory(uint32_t nowMs);
  void renderMqttDown(uint32_t nowMs);
  void renderOverrideFlash(uint32_t nowMs);
  void renderTest(uint32_t nowMs);

  void applyBrightness(uint32_t nowMs);
  bool isNightActive() const;
  bool parseTimeToMinutes(const String& value, int& minutes) const;
  int frameIntervalMs() const;
  ModeKind parseMode(const String& value) const;
  const char* modeName(ModeKind mode) const;
  LedPattern patternFromName(const char* name) const;
  const char* patternName(LedPattern pattern) const;
  LedPattern pickPattern(uint32_t nowMs);
  void setAll(const CRGB& c);
  void startStopPulse(GateStopReason reason, unsigned long durationMs);

  int segmentCount() const;
  void segmentAt(int idx, int& start, int& length) const;
  int segmentTotal() const;
  int mapIndex(int globalIndex) const;
  CRGB applyOrder(uint8_t r, uint8_t g, uint8_t b) const;
};
