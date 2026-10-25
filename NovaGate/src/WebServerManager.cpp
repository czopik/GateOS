#include "WebServerManager.h"

WebServerManager::WebServerManager(ConfigManager& c, GateController& g) : cfg(c), gate(g), server(80) {}

void WebServerManager::begin() {
    setupRoutes();
    server.begin();
}

void WebServerManager::loop() {
    // ESPAsyncWebServer runs asynchronously, nothing usually needed in loop
}

void WebServerManager::setupRoutes() {
    server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest *request){
        StaticJsonDocument<256> doc;
        doc["state"] = gate.getStateString();
        doc["error"] = gate.getLastError();

        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    server.on("/api/control", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
        [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
            if (index + len != total) return;

            StaticJsonDocument<256> doc;
            DeserializationError err = deserializeJson(doc, data, len);
            if (err) {
                request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Bad JSON\"}");
                return;
            }

            String cmd = doc["cmd"] | "";
            if (cmd == "open") gate.open();
            else if (cmd == "close") gate.close();
            else if (cmd == "stop") gate.stop();
            else if (cmd == "toggle") gate.toggle();
            else if (cmd == "home") gate.home();

            request->send(200, "application/json", "{\"status\":\"ok\"}");
    });
}
