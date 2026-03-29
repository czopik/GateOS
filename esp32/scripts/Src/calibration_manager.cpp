#include "calibration_manager.h"
#include "motor_controller.h"
#include "gate_controller.h"
#include <math.h>

CalibrationManager* CalibrationManager::instance = nullptr;

static const unsigned long kWaitSafeMs = 1500;
static const unsigned long kDetectTimeoutMs = 8000;
static const unsigned long kJogMs = 600;
static const unsigned long kJogMaxMs = 1200;
static const unsigned long kRampMoveMs = 1200;
static const unsigned long kRampStopMaxMs = 1200;
static const unsigned long kRampQuietMs = 200;

CalibrationManager::CalibrationManager() {}

void CalibrationManager::begin(ConfigManager* cfg_, MotorController* motor_, GateController* gate_) {
  cfg = cfg_;
  motor = motor_;
  gate = gate_;
}

bool CalibrationManager::start() {
  if (!cfg || !motor) return false;
  if (running) return false;

  running = true;
  errorMsg = "";
  message = "";
  step = CAL_WAIT_SAFE;
  stepStartMs = millis();

  original = *cfg;
  proposed = *cfg;

  motorInvertOriginal = cfg->motorConfig.invertDir;
  motorInvertProposed = motorInvertOriginal;
  motorInvertKnown = false;
  dirConfirmPending = false;
  dirSuggestedInvert = motorInvertOriginal;

  forwardHitsOpen = false;
  forwardHitsClose = false;
  reverseHitsOpen = false;
  reverseHitsClose = false;

  travelMs = 0;
  travelPulses = 0;

  hallPin = cfg->sensorsConfig.hall.pin;
  hallDetected = false;
  hallCount = 0;
  hallCountStart = 0;
  hallCountLast = 0;
  hallRate = 0;
  hallRateMs = millis();

  rampActive = false;
  rampPhase = 0;
  rampStartMs = 0;
  rampStopStartMs = 0;
  rampFirstPulseMs = 0;
  rampLastPulseMs = 0;
  rampHallCount = 0;
  rampPulseCount = 0;

  initProbe(limitOpen, cfg->gpioConfig.limitOpenPin);
  initProbe(limitClose, cfg->gpioConfig.limitClosePin);
  initProbe(obstacle, cfg->gpioConfig.obstaclePin);

  if (gate) gate->stop();
  stopMotor();
  motor->setInvertDir(motorInvertOriginal || cfg->gpioConfig.dirInvert);

  attachHall();
  setStep(CAL_WAIT_SAFE, "Upewnij sie, ze brama jest wolna");
  Serial.println("CAL: start");
  return true;
}

void CalibrationManager::stop() {
  if (!running) return;
  stopMotor();
  detachHall();
  motor->setInvertDir(motorInvertOriginal || cfg->gpioConfig.dirInvert);
  running = false;
  step = CAL_IDLE;
  message = "";
  errorMsg = "";
  Serial.println("CAL: stopped");
}

bool CalibrationManager::apply(String* error) {
  if (!cfg) {
    if (error) *error = "no_config";
    return false;
  }
  if (step != CAL_COMPLETE) {
    if (error) *error = "not_complete";
    return false;
  }
  *cfg = proposed;
  if (!cfg->save(error)) {
    return false;
  }
  return true;
}

bool CalibrationManager::confirmDirection(bool invert) {
  if (!running || step != CAL_CONFIRM_DIR) return false;
  dirConfirmPending = false;
  motorInvertProposed = invert;
  proposed.motorConfig.invertDir = invert;
  if (motor) {
    motor->setInvertDir(invert || cfg->gpioConfig.dirInvert);
  }
  setStep(CAL_MOVE_TO_CLOSE, "Ustawianie pozycji startowej");
  Serial.printf("CAL: direction confirmed invert=%d\n", invert ? 1 : 0);
  return true;
}

void CalibrationManager::loop() {
  if (!running) return;
  updateHallRate();
  updateMove();
  updateStep();
}

void CalibrationManager::setStep(Step next, const char* msg) {
  step = next;
  stepStartMs = millis();
  message = msg ? msg : "";
  moveActive = false;
  moveMode = MOVE_NONE;
}

void CalibrationManager::setError(const char* msg) {
  stopMotor();
  detachHall();
  errorMsg = msg ? msg : "error";
  step = CAL_ERROR;
  message = "";
  Serial.printf("CAL ERROR: %s\n", errorMsg);
}

void CalibrationManager::updateStep() {
  unsigned long now = millis();
  switch (step) {
    case CAL_WAIT_SAFE:
      if (now - stepStartMs >= kWaitSafeMs) {
        setStep(CAL_DETECT_LIMIT_OPEN, "Nacisnij krancowke OPEN");
      }
      break;

    case CAL_DETECT_LIMIT_OPEN:
      if (!limitOpen.present) {
        proposed.gpioConfig.limitOpenPin = -1;
        setStep(CAL_DETECT_LIMIT_CLOSE, "Nacisnij krancowke CLOSE");
        break;
      }
      if (updateProbe(limitOpen, kDetectTimeoutMs)) {
        if (limitOpen.detected) {
          proposed.gpioConfig.limitOpenInvert = limitOpen.invert;
          Serial.printf("CAL: limit open invert=%d\n", limitOpen.invert ? 1 : 0);
        } else {
          proposed.gpioConfig.limitOpenPin = -1;
          Serial.println("CAL: limit open not detected");
        }
        setStep(CAL_DETECT_LIMIT_CLOSE, "Nacisnij krancowke CLOSE");
      }
      break;

    case CAL_DETECT_LIMIT_CLOSE:
      if (!limitClose.present) {
        proposed.gpioConfig.limitClosePin = -1;
        setStep(CAL_DETECT_OBSTACLE, "Nacisnij czujnik przeszkody");
        break;
      }
      if (updateProbe(limitClose, kDetectTimeoutMs)) {
        if (limitClose.detected) {
          proposed.gpioConfig.limitCloseInvert = limitClose.invert;
          Serial.printf("CAL: limit close invert=%d\n", limitClose.invert ? 1 : 0);
        } else {
          proposed.gpioConfig.limitClosePin = -1;
          Serial.println("CAL: limit close not detected");
        }
        setStep(CAL_DETECT_OBSTACLE, "Nacisnij czujnik przeszkody");
      }
      break;

    case CAL_DETECT_OBSTACLE:
      if (!obstacle.present) {
        proposed.gpioConfig.obstaclePin = -1;
        setStep(CAL_JOG_OPEN, "Test kierunku: krotki ruch");
        break;
      }
      if (updateProbe(obstacle, kDetectTimeoutMs)) {
        if (obstacle.detected) {
          proposed.gpioConfig.obstacleInvert = obstacle.invert;
          proposed.safetyConfig.obstacleAction = "stop";
          Serial.printf("CAL: obstacle invert=%d\n", obstacle.invert ? 1 : 0);
        } else {
          proposed.gpioConfig.obstaclePin = -1;
          Serial.println("CAL: obstacle not detected");
        }
        setStep(CAL_JOG_OPEN, "Test kierunku: krotki ruch");
      }
      break;

    case CAL_JOG_OPEN: {
      if (moveMode == MOVE_NONE) {
        int duty = max(cfg->motorConfig.pwmMin, 40);
        startMove(true, MOVE_JOG, kJogMaxMs, kJogMs, duty);
      }
      if (!moveActive) {
        forwardHitsOpen = moveHitOpen;
        forwardHitsClose = moveHitClose;
        if (labs(hallCount - hallCountStart) > 0) {
          hallDetected = true;
        }
        setStep(CAL_JOG_CLOSE, "Test kierunku: krotki ruch w druga strone");
      }
      break;
    }

    case CAL_JOG_CLOSE: {
      if (moveMode == MOVE_NONE) {
        int duty = max(cfg->motorConfig.pwmMin, 40);
        startMove(false, MOVE_JOG, kJogMaxMs, kJogMs, duty);
      }
      if (!moveActive) {
        reverseHitsOpen = moveHitOpen;
        reverseHitsClose = moveHitClose;
        if (labs(hallCount - hallCountStart) > 0) {
          hallDetected = true;
        }
        if (hallPin >= 0) {
          proposed.sensorsConfig.hall.enabled = hallDetected;
        }
        determineDirection();
        dirConfirmPending = true;
        dirSuggestedInvert = motorInvertProposed;
        setStep(CAL_CONFIRM_DIR, "Potwierdz kierunek ruchu");
      }
      break;
    }

    case CAL_CONFIRM_DIR:
      if (!dirConfirmPending) {
        setStep(CAL_MOVE_TO_CLOSE, "Ustawianie pozycji startowej");
      }
      break;

    case CAL_MOVE_TO_CLOSE: {
      if (!limitClose.present) {
        setStep(CAL_MOVE_TO_OPEN, "Przejazd do OPEN");
        break;
      }
      if (isActive(limitClose)) {
        setStep(CAL_MOVE_TO_OPEN, "Przejazd do OPEN");
        break;
      }
      if (moveMode == MOVE_NONE) {
        int duty = cfg->motorConfig.pwmMax;
        unsigned long maxMs = max(10000UL, cfg->gateConfig.movementTimeout / 2);
        startMove(false, MOVE_TO_LIMIT, maxMs, 0, duty);
      }
      if (!moveActive) {
        if (moveHitClose) {
          setStep(CAL_MOVE_TO_OPEN, "Przejazd do OPEN");
        } else if (moveTimedOut) {
          setError("timeout_close");
        }
      }
      break;
    }

    case CAL_MOVE_TO_OPEN: {
      if (!limitOpen.present) {
        setStep(CAL_RAMP_TEST, "Test ramp");
        break;
      }
      if (isActive(limitOpen)) {
        setStep(CAL_RAMP_TEST, "Test ramp");
        break;
      }
      if (moveMode == MOVE_NONE) {
        int duty = cfg->motorConfig.pwmMax;
        unsigned long maxMs = max(10000UL, cfg->gateConfig.movementTimeout / 2);
        startMove(true, MOVE_TO_LIMIT, maxMs, 0, duty);
      }
      if (!moveActive) {
        if (moveHitOpen) {
          travelMs = millis() - moveStartMs;
          travelPulses = labs(hallCount - hallCountStart);
          computeDistanceAndTimeout();
          setStep(CAL_RAMP_TEST, "Test ramp");
        } else if (moveTimedOut) {
          setError("timeout_open");
        }
      }
      break;
    }

    case CAL_RAMP_TEST:
      if (!hallDetected) {
        setStep(CAL_COMPLETE, "Kalibracja zakonczona");
        break;
      }
      computeRampTiming();
      break;

    case CAL_COMPLETE:
      stopMotor();
      detachHall();
      break;

    case CAL_ERROR:
      break;

    case CAL_IDLE:
    default:
      break;
  }
}

void CalibrationManager::updateMove() {
  if (!moveActive) return;

  if (obstacle.present && isActive(obstacle)) {
    setError("obstacle");
    return;
  }

  bool openActive = limitOpen.present && isActive(limitOpen);
  bool closeActive = limitClose.present && isActive(limitClose);

  if (openActive) moveHitOpen = true;
  if (closeActive) moveHitClose = true;

  unsigned long now = millis();
  if (moveMode == MOVE_JOG && moveDurationMs > 0 && now - moveStartMs >= moveDurationMs) {
    stopMotor();
    moveActive = false;
    return;
  }

  if (moveMode == MOVE_TO_LIMIT) {
    if ((moveDirectionOpen && openActive) || (!moveDirectionOpen && closeActive)) {
      stopMotor();
      moveActive = false;
      return;
    }
  }

  if (moveMaxMs > 0 && now - moveStartMs >= moveMaxMs) {
    moveTimedOut = true;
    stopMotor();
    moveActive = false;
    return;
  }
}

void CalibrationManager::startMove(bool openDir, MoveMode mode, unsigned long maxMs, unsigned long durationMs, int duty) {
  moveActive = true;
  moveMode = mode;
  moveDirectionOpen = openDir;
  moveStartMs = millis();
  moveMaxMs = maxMs;
  moveDurationMs = durationMs;
  moveHitOpen = false;
  moveHitClose = false;
  moveTimedOut = false;
  hallCountStart = hallCount;

  motor->setDirection(openDir);
  motor->rampTo(duty, 200);
}

void CalibrationManager::stopMotor() {
  if (!motor) return;
  motor->rampTo(0, 200);
}

bool CalibrationManager::readRaw(int pin) const {
  if (pin < 0) return false;
  return digitalRead(pin) == HIGH;
}

bool CalibrationManager::isActive(const InputProbe& probe) const {
  if (!probe.present || probe.pin < 0) return false;
  bool raw = readRaw(probe.pin);
  return probe.invert ? !raw : raw;
}

void CalibrationManager::initProbe(InputProbe& probe, int pin) {
  probe.pin = pin;
  probe.present = pin >= 0;
  probe.detected = false;
  probe.invert = false;
  probe.idleState = -1;
  probe.startMs = millis();
  if (pin >= 0) {
    pinMode(pin, INPUT_PULLUP);
    probe.idleState = readRaw(pin) ? HIGH : LOW;
  }
}

bool CalibrationManager::updateProbe(InputProbe& probe, unsigned long timeoutMs) {
  if (!probe.present || probe.pin < 0) return true;
  if (probe.idleState < 0) {
    probe.idleState = readRaw(probe.pin) ? HIGH : LOW;
  }
  bool raw = readRaw(probe.pin);
  int rawState = raw ? HIGH : LOW;
  if (!probe.detected && rawState != probe.idleState) {
    probe.detected = true;
    probe.invert = (probe.idleState == HIGH) && (rawState == LOW);
    return true;
  }
  if (millis() - probe.startMs >= timeoutMs) {
    probe.present = false;
    return true;
  }
  return false;
}

void CalibrationManager::attachHall() {
  if (hallPin < 0) return;
  int intNum = digitalPinToInterrupt(hallPin);
  if (intNum < 0) return;
  pinMode(hallPin, INPUT_PULLUP);
  instance = this;
  attachInterrupt(intNum, CalibrationManager::hallIsr, RISING);
  hallAttached = true;
}

void CalibrationManager::detachHall() {
  if (!hallAttached || hallPin < 0) return;
  int intNum = digitalPinToInterrupt(hallPin);
  if (intNum >= 0) {
    detachInterrupt(intNum);
  }
  hallAttached = false;
  instance = nullptr;
}

void CalibrationManager::updateHallRate() {
  if (!hallAttached) return;
  unsigned long now = millis();
  if (now - hallRateMs < 250) return;
  long delta = hallCount - hallCountLast;
  hallRate = (int)(delta * 4);
  hallCountLast = hallCount;
  hallRateMs = now;
}

void CalibrationManager::handleHallPulse() {
  hallCount++;
}

void CalibrationManager::determineDirection() {
  bool toggle = forwardHitsClose || reverseHitsOpen;
  if (forwardHitsOpen || reverseHitsClose || toggle) {
    motorInvertKnown = true;
    motorInvertProposed = motorInvertOriginal ^ toggle;
    proposed.motorConfig.invertDir = motorInvertProposed;
    motor->setInvertDir(motorInvertProposed || cfg->gpioConfig.dirInvert);
    Serial.printf("CAL: motor invert=%d\n", motorInvertProposed ? 1 : 0);
  } else {
    Serial.println("CAL: motor direction unknown, keeping current invert");
  }
}

void CalibrationManager::computeDistanceAndTimeout() {
  if (travelMs > 0) {
    unsigned long timeout = (travelMs * 13) / 10;
    timeout = max(10000UL, timeout);
    timeout = min(120000UL, timeout);
    proposed.gateConfig.movementTimeout = timeout;
    Serial.printf("CAL: movementTimeout=%lu\n", timeout);
  }
  if (hallDetected && travelPulses > 0 &&
      cfg->gateConfig.pulsesPerRevolution > 0 &&
      cfg->gateConfig.wheelCircumference > 0.0f) {
    float distance = (float)travelPulses * cfg->gateConfig.wheelCircumference /
                     (float)cfg->gateConfig.pulsesPerRevolution;
    proposed.gateConfig.maxDistance = distance;
    proposed.gateConfig.totalDistance = distance;
    Serial.printf("CAL: maxDistance=%.3f\n", distance);
  }
}

void CalibrationManager::computeRampTiming() {
  unsigned long now = millis();
  if (!rampActive) {
    rampActive = true;
    rampPhase = 1;
    rampStartMs = now;
    rampFirstPulseMs = 0;
    rampLastPulseMs = 0;
    rampHallCount = hallCount;
    rampPulseCount = 0;
    motor->setDirection(true);
    motor->rampTo(cfg->motorConfig.pwmMax, 800);
    return;
  }

  if (rampPhase == 1) {
    if (hallCount != rampHallCount) {
      rampPulseCount += (int)labs(hallCount - rampHallCount);
      rampHallCount = hallCount;
      if (rampPulseCount >= 3 && rampFirstPulseMs == 0) {
        rampFirstPulseMs = now - rampStartMs;
      }
    }
    if (now - rampStartMs >= kRampMoveMs) {
      motor->rampTo(0, 600);
      rampStopStartMs = now;
      rampPhase = 2;
      rampHallCount = hallCount;
      rampLastPulseMs = now;
    }
    return;
  }

  if (rampPhase == 2) {
    if (hallCount != rampHallCount) {
      rampHallCount = hallCount;
      rampLastPulseMs = now;
    }
    if ((now - rampLastPulseMs >= kRampQuietMs) ||
        (now - rampStopStartMs >= kRampStopMaxMs)) {
      int startMs = cfg->motorConfig.softStartMs;
      int stopMs = cfg->motorConfig.softStopMs;
      if (rampFirstPulseMs > 0) {
        startMs = (int)constrain((int)rampFirstPulseMs * 2, 800, 2200);
      }
      unsigned long stopDelay = (rampLastPulseMs > rampStopStartMs) ?
        (rampLastPulseMs - rampStopStartMs) : 0;
      if (stopDelay > 0) {
        stopMs = (int)constrain((int)stopDelay * 2, 500, 1600);
      }
      proposed.motorConfig.softStartMs = startMs;
      proposed.motorConfig.softStopMs = stopMs;
      Serial.printf("CAL: softStartMs=%d softStopMs=%d\n", startMs, stopMs);
      rampActive = false;
      setStep(CAL_COMPLETE, "Kalibracja zakonczona");
    }
  }
}

int CalibrationManager::progressForStep(Step s) const {
  switch (s) {
    case CAL_WAIT_SAFE: return 5;
    case CAL_DETECT_LIMIT_OPEN: return 15;
    case CAL_DETECT_LIMIT_CLOSE: return 25;
    case CAL_DETECT_OBSTACLE: return 35;
    case CAL_JOG_OPEN: return 45;
    case CAL_JOG_CLOSE: return 55;
    case CAL_CONFIRM_DIR: return 60;
    case CAL_MOVE_TO_CLOSE: return 70;
    case CAL_MOVE_TO_OPEN: return 85;
    case CAL_RAMP_TEST: return 95;
    case CAL_COMPLETE: return 100;
    case CAL_ERROR: return 100;
    case CAL_IDLE:
    default: return 0;
  }
}

const char* CalibrationManager::stepName(Step s) const {
  switch (s) {
    case CAL_WAIT_SAFE: return "wait_safe";
    case CAL_DETECT_LIMIT_OPEN: return "detect_limit_open";
    case CAL_DETECT_LIMIT_CLOSE: return "detect_limit_close";
    case CAL_DETECT_OBSTACLE: return "detect_obstacle";
    case CAL_JOG_OPEN: return "jog_open";
    case CAL_JOG_CLOSE: return "jog_close";
    case CAL_CONFIRM_DIR: return "confirm_dir";
    case CAL_MOVE_TO_CLOSE: return "move_to_close";
    case CAL_MOVE_TO_OPEN: return "move_to_open";
    case CAL_RAMP_TEST: return "ramp_test";
    case CAL_COMPLETE: return "complete";
    case CAL_ERROR: return "error";
    case CAL_IDLE:
    default: return "idle";
  }
}

bool CalibrationManager::buildDelta(JsonObject& out) const {
  bool any = false;
  auto ensure = [&out](const char* key) -> JsonObject {
    if (out.containsKey(key) && out[key].is<JsonObject>()) {
      return out[key].as<JsonObject>();
    }
    return out.createNestedObject(key);
  };

  if (proposed.gpioConfig.limitOpenPin != original.gpioConfig.limitOpenPin) {
    JsonObject gpio = ensure("gpio");
    gpio["limitOpen"] = proposed.gpioConfig.limitOpenPin;
    any = true;
  }
  if (proposed.gpioConfig.limitClosePin != original.gpioConfig.limitClosePin) {
    JsonObject gpio = ensure("gpio");
    gpio["limitClose"] = proposed.gpioConfig.limitClosePin;
    any = true;
  }
  if (proposed.gpioConfig.obstaclePin != original.gpioConfig.obstaclePin) {
    JsonObject gpio = ensure("gpio");
    gpio["obstacle"] = proposed.gpioConfig.obstaclePin;
    any = true;
  }
  if (proposed.gpioConfig.limitOpenInvert != original.gpioConfig.limitOpenInvert) {
    JsonObject gpio = ensure("gpio");
    gpio["limitOpenInvert"] = proposed.gpioConfig.limitOpenInvert;
    any = true;
  }
  if (proposed.gpioConfig.limitCloseInvert != original.gpioConfig.limitCloseInvert) {
    JsonObject gpio = ensure("gpio");
    gpio["limitCloseInvert"] = proposed.gpioConfig.limitCloseInvert;
    any = true;
  }
  if (proposed.gpioConfig.obstacleInvert != original.gpioConfig.obstacleInvert) {
    JsonObject gpio = ensure("gpio");
    gpio["obstacleInvert"] = proposed.gpioConfig.obstacleInvert;
    any = true;
  }
  if (proposed.motorConfig.invertDir != original.motorConfig.invertDir) {
    JsonObject motorObj = ensure("motor");
    motorObj["invertDir"] = proposed.motorConfig.invertDir;
    any = true;
  }
  if (proposed.motorConfig.softStartMs != original.motorConfig.softStartMs) {
    JsonObject motorObj = ensure("motor");
    motorObj["softStartMs"] = proposed.motorConfig.softStartMs;
    any = true;
  }
  if (proposed.motorConfig.softStopMs != original.motorConfig.softStopMs) {
    JsonObject motorObj = ensure("motor");
    motorObj["softStopMs"] = proposed.motorConfig.softStopMs;
    any = true;
  }
  if (proposed.gateConfig.movementTimeout != original.gateConfig.movementTimeout) {
    JsonObject gateObj = ensure("gate");
    gateObj["movementTimeout"] = proposed.gateConfig.movementTimeout;
    any = true;
  }
  if (fabsf(proposed.gateConfig.maxDistance - original.gateConfig.maxDistance) > 0.001f) {
    JsonObject gateObj = ensure("gate");
    gateObj["maxDistance"] = proposed.gateConfig.maxDistance;
    gateObj["totalDistance"] = proposed.gateConfig.maxDistance;
    any = true;
  }
  if (proposed.sensorsConfig.hall.enabled != original.sensorsConfig.hall.enabled) {
    JsonObject sensors = ensure("sensors");
    JsonObject hall = sensors.containsKey("hall") ? sensors["hall"].as<JsonObject>() : sensors.createNestedObject("hall");
    hall["enabled"] = proposed.sensorsConfig.hall.enabled;
    any = true;
  }
  if (proposed.safetyConfig.obstacleAction != original.safetyConfig.obstacleAction) {
    JsonObject safety = ensure("safety");
    safety["obstacleAction"] = proposed.safetyConfig.obstacleAction;
    any = true;
  }
  return any;
}

void CalibrationManager::fillStatus(JsonObject& out) const {
  out["running"] = running;
  out["step"] = stepName(step);
  out["progress"] = progressForStep(step);
  out["message"] = message;
  out["error"] = errorMsg;
  out["dirSuggestedInvert"] = dirSuggestedInvert;

  JsonObject live = out.createNestedObject("liveSignals");
  live["limitOpen"] = limitOpen.present && isActive(limitOpen);
  live["limitClose"] = limitClose.present && isActive(limitClose);
  live["obstacle"] = obstacle.present && isActive(obstacle);
  live["hallRate"] = hallRate;

  DynamicJsonDocument deltaDoc(512);
  JsonObject delta = deltaDoc.to<JsonObject>();
  if (buildDelta(delta)) {
    out["proposedConfigDelta"] = delta;
  }
}

void IRAM_ATTR CalibrationManager::hallIsr() {
  if (instance) {
    instance->handleHallPulse();
  }
}
