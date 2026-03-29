#include "hover_uart_driver.h"

#if defined(ESP32)
#include <HardwareSerial.h>
#endif

namespace {
static constexpr uint16_t kStartFrame = 0xABCD;
static constexpr uint32_t kKeepaliveMs = 20;
static constexpr int16_t kDeadzone = 1;
static constexpr uint32_t kDefaultTelTimeoutMs = 1000;

bool isStrappingPin(int pin) {
  const int pins[] = {0, 2, 4, 5, 12, 15};
  for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
    if (pin == pins[i]) return true;
  }
  return false;
}

#pragma pack(push, 1)
struct SerialCommand {
  uint16_t start;
  int16_t steer;
  int16_t speed;
  uint16_t checksum;
};
#pragma pack(pop)

uint16_t checksumFor(const SerialCommand& cmd) {
  return static_cast<uint16_t>(cmd.start ^ static_cast<uint16_t>(cmd.steer) ^ static_cast<uint16_t>(cmd.speed));
}

HardwareSerial HoverSerial(2);
} // namespace

HoverUartDriver::HoverUartDriver() {}

void HoverUartDriver::configure(const HoverUartConfig& cfgIn) {
  cfg = cfgIn;
  enabled = cfg.rxPin >= 0 && cfg.txPin >= 0 && cfg.baud > 0;
  if (cfg.maxSpeed < 0) cfg.maxSpeed = 0;
  if (cfg.rampStep < 1) cfg.rampStep = 1;
  targetSpeed = 0;
  currentSpeed = 0;
  steer = 0;
  tel = HoverTelemetry();
  lineLen = 0;
  lastKeepaliveMs = 0;
}

void HoverUartDriver::begin() {
  if (!enabled) return;
#if defined(ESP32)
  if (isStrappingPin(cfg.rxPin) || isStrappingPin(cfg.txPin)) {
    Serial.printf("Hover UART warn: RX=%d TX=%d uses strapping pin\n", cfg.rxPin, cfg.txPin);
  }
  HoverSerial.begin(cfg.baud, SERIAL_8N1, cfg.rxPin, cfg.txPin);
#endif
  HoverSerial.print("ZERO\n");
}

bool HoverUartDriver::telemetryTimedOut(uint32_t nowMs, uint32_t timeoutMs) const {
  if (!enabled) return false;
  if (tel.lastTelMs == 0) return false;
  uint32_t limit = timeoutMs > 0 ? timeoutMs : kDefaultTelTimeoutMs;
  return nowMs - tel.lastTelMs > limit;
}

void HoverUartDriver::setTargetSpeed(int16_t speed) {
  if (!enabled) return;
  if (speed > cfg.maxSpeed) speed = cfg.maxSpeed;
  if (speed < -cfg.maxSpeed) speed = -cfg.maxSpeed;
  targetSpeed = speed;
  if (speed != 0) {
    lastTargetNonZeroMs = millis();
    // request arm when we intend to move
    if (!armedRequested) {
      armedRequested = true;
      arm();
    }
  } else {
    // note time when we reached zero target
    if (targetSpeed == 0) lastZeroStopMs = millis();
  }
}

void HoverUartDriver::stop() {
  setTargetSpeed(0);
}

void HoverUartDriver::emergencyStop() {
  targetSpeed = 0;
  currentSpeed = 0;
  lastTargetNonZeroMs = 0;
  lastZeroStopMs = millis();
  // Send command immediately (no ramp)
  sendCommand(0, 0);
  lastKeepaliveMs = millis();
}

void HoverUartDriver::update(uint32_t nowMs) {
  if (!enabled) return;
  handleRx();

  if (nowMs - lastKeepaliveMs >= kKeepaliveMs) {
    lastKeepaliveMs = nowMs;
    updateRamp();
    sendCommand(steer, currentSpeed);
  }

  // Auto-arm/disarm logic
  // If we requested arming but telemetry says not armed, retry ARM every 500ms
  if (armedRequested) {
    if (!tel.armed) {
      if (nowMs - lastArmCmdMs >= 500) {
        arm();
      }
    }
  }

  // If target == 0 and motors stopped (rpm ~= 0 and currentSpeed==0) for 1500ms -> disarm
  if (targetSpeed == 0) {
    bool rpmZero = (tel.lastTelMs == 0) ? false : (tel.rpm == 0);
    if (currentSpeed == 0 && rpmZero) {
      if (lastZeroStopMs == 0) lastZeroStopMs = nowMs;
      if (nowMs - lastZeroStopMs >= 1500 && armedRequested) {
        disarm();
        armedRequested = false;
      }
    } else {
      lastZeroStopMs = 0;
    }
  }
}

void HoverUartDriver::updateRamp() {
  if (currentSpeed < targetSpeed) {
    currentSpeed += cfg.rampStep;
    if (currentSpeed > targetSpeed) currentSpeed = targetSpeed;
  } else if (currentSpeed > targetSpeed) {
    currentSpeed -= cfg.rampStep;
    if (currentSpeed < targetSpeed) currentSpeed = targetSpeed;
  }
  if (currentSpeed > -kDeadzone && currentSpeed < kDeadzone) {
    currentSpeed = 0;
  }
}

void HoverUartDriver::sendCommand(int16_t steerValue, int16_t speedValue) {
  SerialCommand cmd;
  cmd.start = kStartFrame;
  cmd.steer = steerValue;
  cmd.speed = speedValue;
  cmd.checksum = checksumFor(cmd);
  HoverSerial.write(reinterpret_cast<uint8_t*>(&cmd), sizeof(cmd));
}

void HoverUartDriver::arm() {
  if (!enabled) return;
  HoverSerial.print("ARM\n");
  lastArmCmdMs = millis();
}

void HoverUartDriver::disarm() {
  if (!enabled) return;
  HoverSerial.print("DISARM\n");
  lastDisarmCmdMs = millis();
}

void HoverUartDriver::zero() {
  if (!enabled) return;
  HoverSerial.print("ZERO\n");
}

void HoverUartDriver::get() {
  if (!enabled) return;
  HoverSerial.print("GET\n");
}

void HoverUartDriver::handleRx() {
  while (HoverSerial.available()) {
    char c = static_cast<char>(HoverSerial.read());
    if (c == '\r') continue;
    if (c == '\n') {
      lineBuf[lineLen] = '\0';
      if (lineLen >= 4 && strncmp(lineBuf, "TEL,", 4) == 0) {
        // parse key=value pairs separated by commas for forward/backward compatibility
        int dir = tel.dir;
        int rpm = tel.rpm;
        long dist = tel.distMm;
        int fault = tel.fault;
        int bat_cV = -1;
        long batV_old = -1;
        bool haveBatCV = false;
        bool haveBatVold = false;
        int iA_val = -1;
        bool have_iA = false;
        int armedVal = -1;

        char tmp[160];
        strncpy(tmp, lineBuf + 4, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        char* saveptr = nullptr;
        char* tok = strtok_r(tmp, ",", &saveptr);
        while (tok) {
          if (strncmp(tok, "dir=", 4) == 0) dir = atoi(tok + 4);
          else if (strncmp(tok, "rpm=", 4) == 0) rpm = atoi(tok + 4);
          else if (strncmp(tok, "dist_mm=", 8) == 0) dist = atol(tok + 8);
          else if (strncmp(tok, "fault=", 6) == 0) fault = atoi(tok + 6);
          else if (strncmp(tok, "bat_cV=", 7) == 0) { bat_cV = atoi(tok + 7); haveBatCV = true; }
          else if (strncmp(tok, "batV=", 5) == 0) { batV_old = atol(tok + 5); haveBatVold = true; }
          else if (strncmp(tok, "iA=", 3) == 0) { iA_val = atoi(tok + 3); have_iA = true; }
          else if (strncmp(tok, "armed=", 6) == 0) { armedVal = atoi(tok + 6); }
          else if (strncmp(tok, "cmd_age_ms=", 11) == 0) { tel.cmd_age_ms = atoi(tok + 11); }
          tok = strtok_r(nullptr, ",", &saveptr);
        }

        // apply parsed values
        tel.dir = dir;
        tel.rpm = rpm;
        tel.distMm = dist;
        tel.fault = fault;

        if (haveBatCV) {
          tel.bat_cV = bat_cV;
          tel.rawBat = bat_cV;
          tel.batValid = bat_cV >= 0;
          if (tel.batValid) {
            tel.batV = (float)bat_cV / 100.0f;
            tel.batScale = 0; // explicit cV
          }
        } else if (haveBatVold) {
          tel.rawBat = batV_old;
          tel.batValid = (batV_old >= 0);
          if (tel.batValid) {
            if (batV_old >= 1000) {
              tel.batV = (float)batV_old / 1000.0f;
              tel.batScale = 3;
            } else if (batV_old >= 100) {
              tel.batV = (float)batV_old / 10.0f;
              tel.batScale = 2;
            } else {
              tel.batV = (float)batV_old;
              tel.batScale = 1;
            }
          }
        } else {
          tel.batValid = false;
        }

        if (have_iA) {
          tel.iA_x100 = iA_val;
        }
        if (armedVal >= 0) {
          tel.armed = (armedVal != 0);
        }

        tel.lastTelMs = millis();
      }
      lineLen = 0;
      continue;
    }
    if (static_cast<uint8_t>(c) < 0x20 || static_cast<uint8_t>(c) > 0x7E) {
      lineLen = 0;
      continue;
    }
    if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    } else {
      lineLen = 0;
    }
  }
}