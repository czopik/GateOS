#include "MqttManager.h"

MqttManager* MqttManager::instance = nullptr;

MqttManager::MqttManager(const ConfigManager& c, GateController& g) : cfg(c), gate(g), client(espClient) {
    instance = this;
}

void MqttManager::begin() {
    if (cfg.getConfig().mqtt_server.length() > 0) {
        client.setServer(cfg.getConfig().mqtt_server.c_str(), cfg.getConfig().mqtt_port);
        client.setCallback(MqttManager::callback);
    }
}

void MqttManager::loop() {
    if (cfg.getConfig().mqtt_server.length() == 0) return;

    if (!client.connected()) {
        if (millis() - lastReconnectAttempt > 5000) {
            lastReconnectAttempt = millis();
            reconnect();
        }
    } else {
        client.loop();

        static unsigned long lastPub = 0;
        if (millis() - lastPub > 1000) { // publish state every second
            publishState();
            lastPub = millis();
        }
    }
}

void MqttManager::reconnect() {
    String clientId = "NovaGate-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    if (client.connect(clientId.c_str(), cfg.getConfig().mqtt_user.c_str(), cfg.getConfig().mqtt_pass.c_str())) {
        client.subscribe("novagate/cmd");
        publishAutoDiscovery();
    }
}

void MqttManager::publishAutoDiscovery() {
    // HA Auto-Discovery JSON
    StaticJsonDocument<512> doc;
    doc["name"] = "NovaGate";
    doc["state_topic"] = "novagate/state";
    doc["command_topic"] = "novagate/cmd";
    doc["payload_open"] = "OPEN";
    doc["payload_close"] = "CLOSE";
    doc["payload_stop"] = "STOP";
    doc["state_open"] = "OPEN";
    doc["state_closed"] = "CLOSED";
    doc["state_opening"] = "OPENING";
    doc["state_closing"] = "CLOSING";
    doc["state_stopped"] = "STOPPED";
    doc["device_class"] = "gate";

    char buffer[512];
    serializeJson(doc, buffer);
    client.publish("homeassistant/cover/novagate/config", buffer, true);
}

void MqttManager::publishState() {
    String stateStr = gate.getStateString();
    client.publish("novagate/state", stateStr.c_str(), true);
}

void MqttManager::callback(char* topic, byte* payload, unsigned int length) {
    if (!instance) return;
    String cmd = "";
    for (unsigned int i = 0; i < length; i++) {
        cmd += (char)payload[i];
    }

    if (cmd == "OPEN") instance->gate.open();
    else if (cmd == "CLOSE") instance->gate.close();
    else if (cmd == "STOP") instance->gate.stop();
}
