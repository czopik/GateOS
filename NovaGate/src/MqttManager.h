#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "ConfigManager.h"
#include "GateController.h"

class MqttManager {
public:
    MqttManager(const ConfigManager& cfg, GateController& gate);
    void begin();
    void loop();

private:
    const ConfigManager& cfg;
    GateController& gate;

    WiFiClient espClient;
    PubSubClient client;

    unsigned long lastReconnectAttempt = 0;

    void reconnect();
    void publishAutoDiscovery();
    void publishState();

    static MqttManager* instance;
    static void callback(char* topic, byte* payload, unsigned int length);
};
