#pragma once
#include <Arduino.h>

class GateController;

class HCS301Receiver {
public:
    HCS301Receiver(int pin, GateController& gate);
    void begin();
    void loop();

private:
    static void IRAM_ATTR isrHandler();

    int rxPin;
    GateController& gate;

    // Static members for ISR
    static volatile uint32_t lastEdgeTime;
    static volatile uint64_t currentData;
    static volatile int bitCount;
    static volatile bool codeReady;
    static volatile uint64_t lastValidCode;

    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    unsigned long lastActionMs = 0;
};
