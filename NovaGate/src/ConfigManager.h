#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

struct NovaConfig {
    // WiFi
    String wifi_ssid = "NovaGate";
    String wifi_pass = "novagate123";

    // MQTT
    String mqtt_server = "";
    int mqtt_port = 1883;
    String mqtt_user = "";
    String mqtt_pass = "";

    // Motor & Gate mechanics
    float gate_length_m = 4.0;
    int max_speed_rpm = 300;
    int min_speed_rpm = 50;
    int ramp_time_ms = 1500;
    float soft_stop_distance_m = 0.5;

    // Safety
    int max_current_ma = 15000;

    // Pins
    int pin_limit_open = 18;
    int pin_limit_close = 19;
    int pin_photocell = 21;
    int pin_rf_rx = 22;
    int pin_hover_tx = 17;
    int pin_hover_rx = 16;
};

class ConfigManager {
public:
    ConfigManager();
    bool begin();
    bool load();
    bool save();
    const NovaConfig& getConfig() const;
    NovaConfig& getConfigMutable();

private:
    NovaConfig config;
};
