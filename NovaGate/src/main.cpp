#include <Arduino.h>
#include <WiFi.h>
#include "ConfigManager.h"
#include "MotorController.h"
#include "PositionTracker.h"
#include "GateController.h"
#include "HCS301Receiver.h"
#include "MqttManager.h"
#include "WebServerManager.h"

// Global Instances
ConfigManager configManager;
MotorController motorController(configManager);
PositionTracker positionTracker(configManager, motorController);
GateController gateController(configManager, motorController, positionTracker);
HCS301Receiver rfReceiver(22, gateController); // Uses pin 22 by default
MqttManager mqttManager(configManager, gateController);
WebServerManager webServerManager(configManager, gateController);

void setupWiFi() {
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(configManager.getConfig().wifi_ssid.c_str(), configManager.getConfig().wifi_pass.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
        delay(500);
        Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi Connected: " + WiFi.localIP().toString());
        WiFi.mode(WIFI_STA); // Switch to STA only if successful
    } else {
        Serial.println("\nWiFi Failed. Running AP Fallback.");
        WiFi.softAP("NovaGate-AP", "novagate123");
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("\n--- NovaGate Starting ---");

    if (!configManager.begin()) {
        Serial.println("Failed to load config, using defaults");
    }

    // Initialize Motor UART
    motorController.begin(Serial2, configManager.getConfig().pin_hover_rx, configManager.getConfig().pin_hover_tx);

    positionTracker.begin();
    gateController.begin();
    rfReceiver.begin();

    setupWiFi();

    mqttManager.begin();
    webServerManager.begin();
}

void loop() {
    motorController.loop();
    positionTracker.loop();
    gateController.loop();
    rfReceiver.loop();
    mqttManager.loop();
    webServerManager.loop();

    // WDT (implicit via yield in loop, but if needed we can add manual resets)
    yield();
}
