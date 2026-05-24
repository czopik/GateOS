#pragma once
#include <Arduino.h>
#include "config_manager.h"

// WiFiManager - WiFi connection with AP fallback
class WiFiManagerClass {
public:
  WiFiManagerClass();
  void begin(ConfigManager* cfg);
  void loop();

  bool isConnected();
  String getModeString();
  const char* getModeCString();
  uint32_t getReconnectAttempts() const { return reconnectAttempts; }
  uint32_t getRetryIntervalMs() const { return retryIntervalMs; }
  int getLastStatus() const { return lastStatus; }
  const char* getLastStatusCString() const;

private:
  void startAp(const char* reason);
  void stopAp(const char* reason);
  bool applyStaticIp();
  uint32_t retryIntervalForAttempts(uint32_t attempts) const;

  ConfigManager* cfg;
  unsigned long lastAttempt;
  unsigned long firstAttempt;
  unsigned long lastRssiLogMs = 0;
  uint32_t reconnectAttempts = 0;
  uint32_t retryIntervalMs = 10000;
  int lastStatus = 0;
  int lastRssi = 0;
  bool apMode = false;
  bool wasConnected = false;
};

extern WiFiManagerClass WiFiManager;
