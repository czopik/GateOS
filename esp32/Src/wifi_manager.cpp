#include "wifi_manager.h"
#include <WiFi.h>

WiFiManagerClass WiFiManager;

WiFiManagerClass::WiFiManagerClass() : cfg(nullptr), lastAttempt(0), firstAttempt(0) {}

bool WiFiManagerClass::applyStaticIp() {
  if (!cfg->wifiConfig.staticIp.enabled) return false;
  IPAddress ip, gw, mask, dns1, dns2;
  if (!ip.fromString(cfg->wifiConfig.staticIp.ip)) return false;
  if (!gw.fromString(cfg->wifiConfig.staticIp.gateway)) return false;
  if (!mask.fromString(cfg->wifiConfig.staticIp.netmask)) return false;
  if (!cfg->wifiConfig.staticIp.dns1.isEmpty()) dns1.fromString(cfg->wifiConfig.staticIp.dns1);
  if (!cfg->wifiConfig.staticIp.dns2.isEmpty()) dns2.fromString(cfg->wifiConfig.staticIp.dns2);
  return WiFi.config(ip, gw, mask, dns1, dns2);
}

void WiFiManagerClass::startAp(const char* reason) {
  if (apMode) return;
  apMode = true;
  WiFi.disconnect();
  String ssid = cfg->wifiConfig.apFallback.ssid;
  if (ssid.length() == 0) ssid = "GateOS-AP";
  const char* pass = cfg->wifiConfig.apFallback.password.c_str();
  if (strlen(pass) < 8) {
    WiFi.softAP(ssid.c_str());
  } else {
    WiFi.softAP(ssid.c_str(), pass);
  }
  Serial.printf("AP mode (%s): %s\n", reason, ssid.c_str());
}

void WiFiManagerClass::begin(ConfigManager* cfg_) {
  cfg = cfg_;
  apMode = false;
  wasConnected = false;
  lastAttempt = 0;
  firstAttempt = millis();

  WiFi.mode(WIFI_STA);
  if (cfg->wifiConfig.ssid.length() > 0) {
    applyStaticIp();
    WiFi.begin(cfg->wifiConfig.ssid.c_str(), cfg->wifiConfig.password.c_str());
    lastAttempt = millis();
    firstAttempt = lastAttempt;
  } else {
    startAp("no_ssid");
  }

  if (WiFi.status() == WL_CONNECTED) {
    wasConnected = true;
    Serial.printf("WiFi connected to %s, IP: %s\n",
                  WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
  }
}

void WiFiManagerClass::loop() {
  if (apMode) return;

  if (WiFi.status() == WL_CONNECTED) {
    if (!wasConnected) {
      wasConnected = true;
      Serial.printf("WiFi connected to %s, IP: %s\n",
                    WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str());
    }
    return;
  }

  if (wasConnected) {
    wasConnected = false;
  }

  unsigned long now = millis();
  if (cfg->wifiConfig.ssid.length() > 0 && now - lastAttempt > 10000) {
    lastAttempt = now;
    WiFi.begin(cfg->wifiConfig.ssid.c_str(), cfg->wifiConfig.password.c_str());
  }

  if (cfg->wifiConfig.apFallback.fallbackTimeoutMs > 0 &&
      now - firstAttempt > cfg->wifiConfig.apFallback.fallbackTimeoutMs) {
    startAp("timeout");
  }
}

bool WiFiManagerClass::isConnected() {
  return WiFi.status() == WL_CONNECTED;
}

String WiFiManagerClass::getModeString() {
  return String(getModeCString());
}

const char* WiFiManagerClass::getModeCString() {
  if (isConnected()) return "STA";
  if (apMode) return "AP";
  return "DISCONNECTED";
}
