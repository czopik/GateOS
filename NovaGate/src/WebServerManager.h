#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "GateController.h"
#include "ConfigManager.h"

class WebServerManager {
public:
    WebServerManager(ConfigManager& cfg, GateController& gate);
    void begin();
    void loop();

private:
    ConfigManager& cfg;
    GateController& gate;
    AsyncWebServer server;

    void setupRoutes();
    void handleControl(AsyncWebServerRequest* request);
};
