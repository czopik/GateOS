#include "ota_guard.h"
#include <WiFi.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>

namespace {
constexpr uint32_t kRtcMagic = 0x0A7A6A01u;
constexpr uint32_t kMaxTrialCrashes = 3;
constexpr uint32_t kMinUptimeForConfirmMs = 60000;    // stay up >= 60 s
constexpr uint32_t kTrialTimeoutMs = 10UL * 60UL * 1000UL;  // 10 min
constexpr uint32_t kSelfTestIntervalMs = 10000;
constexpr const char* kNvsNs = "otaguard";
constexpr const char* kNvsGoodKey = "good";

// Survive panic / watchdog / software resets (not power-on).
RTC_NOINIT_ATTR uint32_t rtcMagic;
RTC_NOINIT_ATTR uint32_t rtcCrashCount;

bool g_trial = false;
bool g_confirmed = false;
uint32_t g_crashCount = 0;
uint32_t g_lastSelfTestMs = 0;
uint32_t g_selfTestFails = 0;
char g_running[17] = {0};
char g_lastGood[17] = {0};
const char* g_lastSelfTestResult = "none";

bool isCrashReset(esp_reset_reason_t r) {
  return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
}

bool runningPendingVerify() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  return running && esp_ota_get_state_partition(running, &st) == ESP_OK &&
         st == ESP_OTA_IMG_PENDING_VERIFY;
}

void rollbackNow(const char* why) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  Serial.printf("[OTA-GUARD] ROLLBACK requested: %s\n", why);
  if (runningPendingVerify()) {
    // Bootloader rollback: mark invalid + reboot into previous image.
    rtcCrashCount = 0;
    Serial.flush();
    esp_ota_mark_app_invalid_rollback_and_reboot();
    Serial.println("[OTA-GUARD] bootloader rollback failed - trying manual switch");
  }
  if (!other || !running || other == running) {
    Serial.println("[OTA-GUARD] no alternative slot - rollback impossible");
    return;
  }
  esp_app_desc_t desc;
  if (esp_ota_get_partition_description(other, &desc) != ESP_OK) {
    Serial.printf("[OTA-GUARD] slot %s has no valid app - rollback impossible\n", other->label);
    return;
  }
  // esp_ota_set_boot_partition() verifies the image before switching.
  const esp_err_t err = esp_ota_set_boot_partition(other);
  if (err != ESP_OK) {
    Serial.printf("[OTA-GUARD] set_boot_partition(%s) failed err=%d\n", other->label, (int)err);
    return;
  }
  Serial.printf("[OTA-GUARD] booting previous firmware from %s (%s %s %s)\n",
                other->label, desc.version, desc.date, desc.time);
  rtcCrashCount = 0;
  Serial.flush();
  delay(100);
  esp_restart();
}

// Proves the web server accepts connections and answers HTTP (any status).
bool selfTestHttp(uint16_t port) {
  if (!WiFi.isConnected()) return false;
  WiFiClient c;
  if (!c.connect(WiFi.localIP(), port, 1500)) return false;
  c.print("GET /api/status-lite HTTP/1.0\r\nHost: self\r\nConnection: close\r\n\r\n");
  const uint32_t t0 = millis();
  String line;
  while (millis() - t0 < 2000) {
    while (c.available()) {
      const char ch = (char)c.read();
      if (ch == '\n') {
        c.stop();
        return line.startsWith("HTTP/1.");
      }
      if (ch != '\r' && line.length() < 64) line += ch;
    }
    if (!c.connected() && !c.available()) break;
    delay(5);
  }
  c.stop();
  return false;
}
} // namespace

// Overrides the weak Arduino-core hook: keep a new OTA image in PENDING_VERIFY
// until otaGuardLoop() confirms it, so any reset before that makes the
// bootloader boot the previous firmware automatically.
extern "C" bool verifyRollbackLater() { return true; }

void otaGuardEarlyBoot() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running) strncpy(g_running, running->label, sizeof(g_running) - 1);

  Preferences prefs;
  if (prefs.begin(kNvsNs, true)) {
    String good = prefs.getString(kNvsGoodKey, "");
    strncpy(g_lastGood, good.c_str(), sizeof(g_lastGood) - 1);
    prefs.end();
  }

  // Trial = this slot has not yet been confirmed as a good firmware.
  g_trial = ((g_running[0] != '\0') && (strcmp(g_running, g_lastGood) != 0)) ||
            runningPendingVerify();

  const esp_reset_reason_t reason = esp_reset_reason();
  if (rtcMagic != kRtcMagic || reason == ESP_RST_POWERON) {
    rtcMagic = kRtcMagic;
    rtcCrashCount = 0;
  }
  if (!g_trial) {
    rtcCrashCount = 0;
    g_confirmed = true;
    Serial.printf("[OTA-GUARD] running=%s (confirmed)\n", g_running);
    return;
  }
  if (isCrashReset(reason)) rtcCrashCount++;
  g_crashCount = rtcCrashCount;
  Serial.printf("[OTA-GUARD] TRIAL running=%s lastGood=%s crashes=%lu reset=%d\n",
                g_running, g_lastGood[0] ? g_lastGood : "-", (unsigned long)g_crashCount, (int)reason);
  if (g_crashCount >= kMaxTrialCrashes) {
    rollbackNow("crash loop on new firmware");
  }
}

void otaGuardLoop(bool wifiConnected, bool gateTaskAlive, uint16_t webPort) {
  if (!g_trial || g_confirmed) return;
  const uint32_t now = millis();
  if (now > kTrialTimeoutMs) {
    rollbackNow("not healthy within 10 min");
    g_trial = false;  // only reached if rollback impossible: stop retrying
    return;
  }
  if (now < kMinUptimeForConfirmMs) return;
  if (now - g_lastSelfTestMs < kSelfTestIntervalMs) return;
  g_lastSelfTestMs = now;
  if (!wifiConnected) { g_lastSelfTestResult = "wifi_down"; return; }
  if (!gateTaskAlive) { g_lastSelfTestResult = "gate_task_stalled"; return; }
  if (!selfTestHttp(webPort)) {
    g_selfTestFails++;
    g_lastSelfTestResult = "http_fail";
    Serial.printf("[OTA-GUARD] HTTP self-test failed (%lu)\n", (unsigned long)g_selfTestFails);
    return;
  }
  Preferences prefs;
  if (!prefs.begin(kNvsNs, false)) { g_lastSelfTestResult = "nvs_fail"; return; }
  prefs.putString(kNvsGoodKey, g_running);
  prefs.end();
  strncpy(g_lastGood, g_running, sizeof(g_lastGood) - 1);
  g_confirmed = true;
  g_lastSelfTestResult = "ok";
  rtcCrashCount = 0;
  // Harmless when bootloader rollback is disabled; correct if it is enabled.
  esp_ota_mark_app_valid_cancel_rollback();
  Serial.printf("[OTA-GUARD] firmware in %s CONFIRMED good (uptime %lus)\n",
                g_running, (unsigned long)(now / 1000));
}

void otaGuardFillStatus(JsonObject out) {
  out["running"] = (const char*)g_running;
  out["lastGood"] = (const char*)g_lastGood;
  out["trial"] = g_trial && !g_confirmed;
  out["confirmed"] = g_confirmed;
  out["crashCount"] = g_crashCount;
  out["selfTest"] = g_lastSelfTestResult;
}
