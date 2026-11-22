#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Software OTA rollback guard.
//
// The Arduino-ESP32 bootloader is built without CONFIG_BOOTLOADER_APP_ROLLBACK,
// so a broken image would stay active and the device could only be recovered
// over a cable.  After an OTA update the previous firmware is still intact in
// the other OTA slot, so this guard switches back to it when the new image is
// not healthy:
//   * >= kMaxTrialCrashes crash resets (panic / watchdog) while on trial, or
//   * not confirmed healthy within kTrialTimeoutMs.
// "Healthy" = WiFi connected, GateTask alive and the device's own HTTP server
// answers on its port (checked over the network stack).
//
// otaGuardEarlyBoot() must be the first thing in setup().
void otaGuardEarlyBoot();
// Call from loop(); cheap when not on trial.
void otaGuardLoop(bool wifiConnected, bool gateTaskAlive, uint16_t webPort);
void otaGuardFillStatus(JsonObject out);
