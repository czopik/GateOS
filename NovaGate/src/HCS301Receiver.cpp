#include "HCS301Receiver.h"
#include "GateController.h"

// Define static variables
volatile uint32_t HCS301Receiver::lastEdgeTime = 0;
volatile uint64_t HCS301Receiver::currentData = 0;
volatile int HCS301Receiver::bitCount = 0;
volatile bool HCS301Receiver::codeReady = false;
volatile uint64_t HCS301Receiver::lastValidCode = 0;

HCS301Receiver::HCS301Receiver(int p, GateController& g) : rxPin(p), gate(g) {}

void HCS301Receiver::begin() {
    pinMode(rxPin, INPUT);
    attachInterrupt(digitalPinToInterrupt(rxPin), isrHandler, CHANGE);
}

void HCS301Receiver::loop() {
    if (codeReady) {
        portENTER_CRITICAL(&mux);
        uint64_t code = lastValidCode;
        codeReady = false;
        portEXIT_CRITICAL(&mux);

        if (millis() - lastActionMs > 1000) { // Simple debounce
            Serial.printf("RF Code received: %llX\n", code);
            gate.toggle();
            lastActionMs = millis();
        }
    }
}

void IRAM_ATTR HCS301Receiver::isrHandler() {
    uint32_t now = micros();
    uint32_t duration = now - lastEdgeTime;
    lastEdgeTime = now;

    // PWM decoding logic for HCS301 (simplified Te = ~400us)
    if (duration > 3000) { // Preamble or dead time
        bitCount = 0;
        currentData = 0;
        return;
    }

    if (digitalRead(22) == LOW) { // High period ended
        if (duration > 200 && duration < 600) { // Short high -> 0
            currentData = (currentData << 1) | 0;
            bitCount++;
        } else if (duration > 600 && duration < 1200) { // Long high -> 1
            currentData = (currentData << 1) | 1;
            bitCount++;
        } else {
            bitCount = 0; // Error
        }
    }

    if (bitCount == 66) { // 66 bits for HCS301 (usually 34 header/fixed + 32 hopping)
        lastValidCode = currentData;
        codeReady = true;
        bitCount = 0;
    }
}
