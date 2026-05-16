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
  uint32_t reconnectCount() const { return _reconnectCount; }

private:
  void startAp(const char* reason);
  bool applyStaticIp();

  ConfigManager* cfg;
  unsigned long lastAttempt;
  unsigned long firstAttempt;
  bool apMode = false;
  bool wasConnected = false;
  uint32_t _reconnectCount = 0;
};

extern WiFiManagerClass WiFiManager;
