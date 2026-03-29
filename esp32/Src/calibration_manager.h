#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "config_manager.h"

class MotorController;
class GateController;

class CalibrationManager {
public:
  CalibrationManager();

  void begin(ConfigManager* cfg, MotorController* motor, GateController* gate);
  void loop();

  bool start();
  void stop();
  bool confirmDirection(bool invert);
  bool apply(String* error = nullptr);

  bool isRunning() const { return running; }
  bool isComplete() const { return step == CAL_COMPLETE; }
  bool hasError() const { return step == CAL_ERROR; }

  void fillStatus(JsonObject& out) const;

private:
  enum Step {
    CAL_IDLE = 0,
    CAL_WAIT_SAFE,
    CAL_DETECT_LIMIT_OPEN,
    CAL_DETECT_LIMIT_CLOSE,
    CAL_DETECT_OBSTACLE,
    CAL_JOG_OPEN,
    CAL_JOG_CLOSE,
    CAL_CONFIRM_DIR,
    CAL_MOVE_TO_CLOSE,
    CAL_MOVE_TO_OPEN,
    CAL_RAMP_TEST,
    CAL_COMPLETE,
    CAL_ERROR
  };

  enum MoveMode {
    MOVE_NONE = 0,
    MOVE_JOG,
    MOVE_TO_LIMIT
  };

  struct InputProbe {
    int pin = -1;
    bool present = false;
    bool detected = false;
    bool invert = false;
    String pullMode = "up";
    int idleState = -1;
    unsigned long startMs = 0;
  };

  ConfigManager* cfg = nullptr;
  MotorController* motor = nullptr;
  GateController* gate = nullptr;

  ConfigManager original;
  ConfigManager proposed;

  bool running = false;
  Step step = CAL_IDLE;
  unsigned long stepStartMs = 0;
  const char* message = "";
  const char* errorMsg = "";

  InputProbe limitOpen;
  InputProbe limitClose;
  InputProbe obstacle;
  int hallPin = -1;
  bool hallAttached = false;
  bool hallDetected = false;
  volatile long hallCount = 0;
  long hallCountStart = 0;
  long hallCountLast = 0;
  unsigned long hallRateMs = 0;
  int hallRate = 0;

  bool motorInvertOriginal = false;
  bool motorInvertProposed = false;
  bool motorInvertKnown = false;
  bool dirConfirmPending = false;
  bool dirSuggestedInvert = false;

  bool forwardHitsOpen = false;
  bool forwardHitsClose = false;
  bool reverseHitsOpen = false;
  bool reverseHitsClose = false;

  MoveMode moveMode = MOVE_NONE;
  bool moveActive = false;
  bool moveDirectionOpen = true;
  unsigned long moveStartMs = 0;
  unsigned long moveMaxMs = 0;
  unsigned long moveDurationMs = 0;
  bool moveHitOpen = false;
  bool moveHitClose = false;
  bool moveTimedOut = false;

  unsigned long travelMs = 0;
  long travelPulses = 0;

  bool rampActive = false;
  int rampPhase = 0;
  unsigned long rampStartMs = 0;
  unsigned long rampStopStartMs = 0;
  unsigned long rampFirstPulseMs = 0;
  unsigned long rampLastPulseMs = 0;
  long rampHallCount = 0;
  int rampPulseCount = 0;

  void setStep(Step next, const char* msg);
  void setError(const char* msg);
  void updateStep();
  void updateMove();
  void startMove(bool openDir, MoveMode mode, unsigned long maxMs, unsigned long durationMs, int duty);
  void stopMotor();

  bool readRaw(int pin) const;
  bool isActive(const InputProbe& probe) const;
  void initProbe(InputProbe& probe, int pin, bool invert, const String& pullMode);
  bool updateProbe(InputProbe& probe, unsigned long timeoutMs);

  void attachHall();
  void detachHall();
  void updateHallRate();
  void handleHallPulse();

  void determineDirection();
  void computeDistanceAndTimeout();
  void computeRampTiming();

  int progressForStep(Step s) const;
  const char* stepName(Step s) const;
  bool buildDelta(JsonObject& out) const;

  static void IRAM_ATTR hallIsr();
  static CalibrationManager* instance;
};
