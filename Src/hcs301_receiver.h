#pragma once
#include <Arduino.h>

// Minimalny HCS301 receiver wrapper - wystawia callback po odebraniu pakietu
using HCSCallback = void(*)(unsigned long serialNum, unsigned long encript, bool btnToggle, bool btnGreen, bool batteryLow);

class HCS301Receiver {
public:
  HCS301Receiver(int pin);
  void begin();
  void loop();
  void setCallback(HCSCallback cb);

  // Tryb nauki: kiedy aktywny, odebrane piloty są automatycznie zapisywane
  void setLearnMode(bool enable);
  bool isLearnMode() const;

private:
  int pin;
  HCSCallback cb;
  // Very small state - for production use the robust decoder already in project
  volatile unsigned long last_change;
};
