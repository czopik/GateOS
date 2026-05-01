#include "wifi_manager.h"
#include <WiFi.h>
#include <esp_wifi.h>

WiFiManagerClass WiFiManager;

WiFiManagerClass::WiFiManagerClass() : cfg(nullptr), lastAttempt(0), firstAttempt(0) {}

namespace {
const char* wifiStatusToString(int status) {
  switch (status) {
    case WL_IDLE_STATUS: return "idle";
    case WL_NO_SSID_AVAIL: return "no_ssid";
    case WL_SCAN_COMPLETED: return "scan_completed";
    case WL_CONNECTED: return "connected";
    case WL_CONNECT_FAILED: return "connect_failed";
    case WL_CONNECTION_LOST: return "connection_lost";
    case WL_DISCONNECTED: return "disconnected";
    default: return "unknown";
  }
}
}

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
  if (cfg->wifiConfig.ssid.length() > 0) {
    WiFi.mode(WIFI_AP_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
  } else {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
  }
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

void WiFiManagerClass::stopAp(const char* reason) {
  if (!apMode) return;
  WiFi.softAPdisconnect(true);
  apMode = false;
  if (cfg->wifiConfig.ssid.length() > 0) {
    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
  }
  Serial.printf("AP mode off (%s)\n", reason);
}

void WiFiManagerClass::begin(ConfigManager* cfg_) {
  cfg = cfg_;
  apMode = false;
  wasConnected = false;
  lastAttempt = 0;
  firstAttempt = millis();
  lastRssiLogMs = 0;
  reconnectAttempts = 0;
  retryIntervalMs = 10000;
  lastStatus = WiFi.status();
  lastRssi = 0;

  if (cfg->wifiConfig.ssid.length() > 0) {
    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);   // Disable power save - eliminates 200-500ms ping latency.
    applyStaticIp();
    WiFi.begin(cfg->wifiConfig.ssid.c_str(), cfg->wifiConfig.password.c_str());
    lastAttempt = millis();
    firstAttempt = lastAttempt;
  } else {
    startAp("no_ssid");
  }

  if (WiFi.status() == WL_CONNECTED) {
    wasConnected = true;
    stopAp("sta_connected");
    Serial.printf("WiFi connected to %s, IP: %s\n",
                  WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
  }
}

void WiFiManagerClass::loop() {
  unsigned long now = millis();
  lastStatus = WiFi.status();

  if (lastStatus == WL_CONNECTED) {
    int rssi = WiFi.RSSI();
    lastRssi = rssi;
    if (!wasConnected) {
      wasConnected = true;
      reconnectAttempts = 0;
      retryIntervalMs = 10000;
      stopAp("sta_recovered");
      Serial.printf("WiFi connected to %s, IP: %s RSSI=%d heap=%u\n",
                    WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str(),
                    rssi,
                    (unsigned)ESP.getFreeHeap());
    }
    if (lastRssiLogMs == 0 || now - lastRssiLogMs >= 60000) {
      lastRssiLogMs = now;
      Serial.printf("[WIFI] ok RSSI=%d IP=%s heap=%u minHeap=%u\n",
                    rssi,
                    WiFi.localIP().toString().c_str(),
                    (unsigned)ESP.getFreeHeap(),
                    (unsigned)ESP.getMinFreeHeap());
    }
    return;
  }

  if (wasConnected) {
    wasConnected = false;
    firstAttempt = millis();
    lastAttempt = 0;
    Serial.printf("[WIFI] disconnected status=%s(%d) lastRSSI=%d heap=%u\n",
                  wifiStatusToString(lastStatus),
                  lastStatus,
                  lastRssi,
                  (unsigned)ESP.getFreeHeap());
  }

  retryIntervalMs = retryIntervalForAttempts(reconnectAttempts);
  if (cfg->wifiConfig.ssid.length() > 0 && now - lastAttempt > retryIntervalMs) {
    lastAttempt = now;
    reconnectAttempts++;
    Serial.printf("[WIFI] reconnect attempt=%lu intervalMs=%lu status=%s(%d)\n",
                  (unsigned long)reconnectAttempts,
                  (unsigned long)retryIntervalMs,
                  wifiStatusToString(lastStatus),
                  lastStatus);
    applyStaticIp();
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

uint32_t WiFiManagerClass::retryIntervalForAttempts(uint32_t attempts) const {
  if (attempts < 6) return 10000;
  if (attempts < 12) return 30000;
  return 60000;
}

const char* WiFiManagerClass::getLastStatusCString() const {
  return wifiStatusToString(lastStatus);
}
