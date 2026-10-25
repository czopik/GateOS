#include "ConfigManager.h"

ConfigManager::ConfigManager() {
}

bool ConfigManager::begin() {
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS Mount Failed");
        return false;
    }
    load();
    return true;
}

bool ConfigManager::load() {
    if (!LittleFS.exists("/config.json")) return false;

    File file = LittleFS.open("/config.json", "r");
    if (!file) return false;

    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) return false;

    config.wifi_ssid = doc["wifi_ssid"] | config.wifi_ssid;
    config.wifi_pass = doc["wifi_pass"] | config.wifi_pass;
    config.mqtt_server = doc["mqtt_server"] | config.mqtt_server;
    config.mqtt_port = doc["mqtt_port"] | config.mqtt_port;
    config.mqtt_user = doc["mqtt_user"] | config.mqtt_user;
    config.mqtt_pass = doc["mqtt_pass"] | config.mqtt_pass;

    config.gate_length_m = doc["gate_length_m"] | config.gate_length_m;
    config.max_speed_rpm = doc["max_speed_rpm"] | config.max_speed_rpm;
    config.min_speed_rpm = doc["min_speed_rpm"] | config.min_speed_rpm;
    config.ramp_time_ms = doc["ramp_time_ms"] | config.ramp_time_ms;
    config.soft_stop_distance_m = doc["soft_stop_distance_m"] | config.soft_stop_distance_m;

    config.max_current_ma = doc["max_current_ma"] | config.max_current_ma;

    return true;
}

bool ConfigManager::save() {
    File file = LittleFS.open("/config.json", "w");
    if (!file) return false;

    StaticJsonDocument<1024> doc;
    doc["wifi_ssid"] = config.wifi_ssid;
    doc["wifi_pass"] = config.wifi_pass;
    doc["mqtt_server"] = config.mqtt_server;
    doc["mqtt_port"] = config.mqtt_port;
    doc["mqtt_user"] = config.mqtt_user;
    doc["mqtt_pass"] = config.mqtt_pass;

    doc["gate_length_m"] = config.gate_length_m;
    doc["max_speed_rpm"] = config.max_speed_rpm;
    doc["min_speed_rpm"] = config.min_speed_rpm;
    doc["ramp_time_ms"] = config.ramp_time_ms;
    doc["soft_stop_distance_m"] = config.soft_stop_distance_m;
    doc["max_current_ma"] = config.max_current_ma;

    serializeJson(doc, file);
    file.close();
    return true;
}

const NovaConfig& ConfigManager::getConfig() const { return config; }
NovaConfig& ConfigManager::getConfigMutable() { return config; }
