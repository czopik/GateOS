#include "mqtt_manager.h"

MqttManager::MqttManager() : client(wifiClient) {}

void MqttManager::begin(const MQTTConfig& cfgIn) {
  applyConfig(cfgIn);
}

void MqttManager::applyConfig(const MQTTConfig& cfgIn) {
  cfg = cfgIn;
  enabledFlag = cfg.enabled && cfg.server.length() > 0;
  buildTopics();
  // PubSubClient default payload buffer is often too small for rich telemetry JSON.
  // Keep a larger buffer to avoid false publish failures when link state is OK.
  client.setBufferSize(1024);

  if (!enabledFlag) {
    if (client.connected()) {
      client.disconnect();
    }
    resetStatus();
    lastErrorMsg = cfg.enabled ? "no_server" : "disabled";
    return;
  }

  client.setServer(cfg.server.c_str(), cfg.port);
  client.setSocketTimeout(1);
  client.disconnect();
  resetStatus();
  lastErrorMsg = "";
}

void MqttManager::loop() {
  if (!enabledFlag) return;
  if (client.connected()) {
    client.loop();
    return;
  }
  if (!connectAllowed) return;
  if (ensureConnected()) {
    client.loop();
  }
}

bool MqttManager::connected() {
  return enabledFlag && client.connected();
}

bool MqttManager::enabled() const {
  return enabledFlag;
}

int MqttManager::state() {
  return enabledFlag ? client.state() : lastState;
}

const String& MqttManager::lastError() const {
  return lastErrorMsg;
}

void MqttManager::setCallback(MQTT_CALLBACK_SIGNATURE) {
  client.setCallback(callback);
}

bool MqttManager::publish(const char* topic, const char* payload, bool retain) {
  if (!enabledFlag || !client.connected()) return false;
  bool ok = client.publish(topic, payload, retain);
  if (!ok) {
    lastErrorMsg = String("publish_failed:") + String(client.state());
    unsigned long now = millis();
    if (now - lastPublishFailLogMs >= 2000) {
      lastPublishFailLogMs = now;
      Serial.printf("MQTT publish fail %s state=%d\n", topic, client.state());
    }
  }
  return ok;
}

bool MqttManager::subscribe(const char* topic) {
  if (!enabledFlag || !client.connected()) return false;
  bool ok = client.subscribe(topic);
  if (!ok) {
    lastErrorMsg = String("subscribe_failed:") + String(client.state());
    Serial.printf("MQTT subscribe fail %s state=%d\n", topic, client.state());
  }
  return ok;
}

bool MqttManager::testPublish(String& outTopic, String& outMsg) {
  outTopic = "";
  outMsg = "";
  if (!enabledFlag) {
    lastErrorMsg = cfg.enabled ? "no_server" : "disabled";
    return false;
  }
  if (!ensureConnected()) {
    return false;
  }
  String base = cfg.topicBase.length() ? cfg.topicBase : "gateos";
  outTopic = base + "/test";
  outMsg = String("gateos test ") + String(millis());
  bool ok = client.publish(outTopic.c_str(), outMsg.c_str(), cfg.retain);
  Serial.printf("MQTT publish %s ok=%d state=%d\n", outTopic.c_str(), ok, client.state());
  if (!ok) {
    lastErrorMsg = String("publish_failed:") + String(client.state());
  }
  return ok;
}

const char* MqttManager::topicState() const {
  return topicStateBuf;
}

const char* MqttManager::topicCommand() const {
  return topicCommandBuf;
}

const char* MqttManager::topicEvents() const {
  return topicEventsBuf;
}

const char* MqttManager::topicTelemetry() const {
  return topicTelemetryBuf;
}

const char* MqttManager::topicPosition() const {
  return topicPosBuf;
}

const char* MqttManager::topicGateCmd() const {
  return topicGateCmdBuf;
}

const char* MqttManager::topicGateSetMax() const {
  return topicGateSetMaxBuf;
}

const char* MqttManager::topicGateCalibrate() const {
  return topicGateCalibrateBuf;
}

const char* MqttManager::topicLedState() const {
  return topicLedStateBuf;
}

const char* MqttManager::topicLedCmd() const {
  return topicLedCmdBuf;
}

const char* MqttManager::topicMotionState() const {
  return topicMotionStateBuf;
}

const char* MqttManager::topicMotionPosition() const {
  return topicMotionPositionBuf;
}

bool MqttManager::ensureConnected() {
  if (!enabledFlag) return false;
  if (client.connected()) return true;
  if (!WiFi.isConnected()) {
    lastErrorMsg = "wifi_disconnected";
    return false;
  }
  unsigned long now = millis();
  if (now - lastConnectAttemptMs < 8000) return false;
  lastConnectAttemptMs = now;
  return connectNow();
}

bool MqttManager::connectNow() {
  String clientId = buildClientId();
  String lwtTopic = buildLwtTopic();
  String lwtPayload = buildLwtPayload();
  int qos = cfg.qos;
  if (qos < 0) qos = 0;
  if (qos > 1) qos = 1;

  Serial.printf("MQTT connect %s:%d user=%s state=%d\n",
                cfg.server.c_str(), cfg.port,
                cfg.user.length() ? "yes" : "no",
                client.state());

  bool ok = false;
  if (cfg.user.length() > 0) {
    ok = client.connect(clientId.c_str(),
                        cfg.user.c_str(),
                        cfg.password.c_str(),
                        lwtTopic.c_str(),
                        qos,
                        cfg.retain,
                        lwtPayload.c_str());
  } else {
    ok = client.connect(clientId.c_str(),
                        lwtTopic.c_str(),
                        qos,
                        cfg.retain,
                        lwtPayload.c_str());
  }

  lastState = client.state();
  if (!ok) {
    lastErrorMsg = String("connect_failed:") + String(lastState);
    Serial.printf("MQTT connect failed state=%d\n", lastState);
    return false;
  }

  lastErrorMsg = "";
  Serial.printf("MQTT connected state=%d\n", lastState);
  return true;
}

void MqttManager::resetStatus() {
  lastConnectAttemptMs = 0;
  wasConnected = false;
  lastState = 0;
}

void MqttManager::buildTopics() {
  if (!enabledFlag) {
    topicStateBuf[0] = '\0';
    topicCommandBuf[0] = '\0';
    topicEventsBuf[0] = '\0';
    topicTelemetryBuf[0] = '\0';
    topicPosBuf[0] = '\0';
    topicGateCmdBuf[0] = '\0';
    topicGateSetMaxBuf[0] = '\0';
    topicGateCalibrateBuf[0] = '\0';
    topicLedStateBuf[0] = '\0';
    topicLedCmdBuf[0] = '\0';
    return;
  }
  const char* base = cfg.topicBase.length() ? cfg.topicBase.c_str() : "gateos";
  snprintf(topicStateBuf, sizeof(topicStateBuf), "%s/stan", base);
  snprintf(topicCommandBuf, sizeof(topicCommandBuf), "%s/sterowanie", base);
  snprintf(topicEventsBuf, sizeof(topicEventsBuf), "%s/zdarzenia", base);
  snprintf(topicTelemetryBuf, sizeof(topicTelemetryBuf), "%s/telemetry", base);
  snprintf(topicPosBuf, sizeof(topicPosBuf), "%s/pos", base);
  snprintf(topicGateCmdBuf, sizeof(topicGateCmdBuf), "%s/gate/cmd", base);
  snprintf(topicGateSetMaxBuf, sizeof(topicGateSetMaxBuf), "%s/gate/set_max_distance", base);
  snprintf(topicGateCalibrateBuf, sizeof(topicGateCalibrateBuf), "%s/gate/calibrate", base);
  snprintf(topicLedStateBuf, sizeof(topicLedStateBuf), "%s/led/state", base);
  snprintf(topicLedCmdBuf, sizeof(topicLedCmdBuf), "%s/led/cmd", base);
  snprintf(topicMotionStateBuf, sizeof(topicMotionStateBuf), "%s/motion/state", base);
  snprintf(topicMotionPositionBuf, sizeof(topicMotionPositionBuf), "%s/motion/position", base);
}

String MqttManager::buildClientId() const {
  uint64_t mac = ESP.getEfuseMac();
  char buf[32];
  snprintf(buf, sizeof(buf), "gateos-%04X%08X",
           (uint16_t)(mac >> 32),
           (uint32_t)(mac & 0xFFFFFFFF));
  return String(buf);
}

String MqttManager::buildLwtTopic() const {
  if (cfg.lwtTopic.length() > 0) return cfg.lwtTopic;
  String base = cfg.topicBase.length() ? cfg.topicBase : "gateos";
  return base + "/lwt";
}

String MqttManager::buildLwtPayload() const {
  return cfg.lwtPayload.length() > 0 ? cfg.lwtPayload : "offline";
}
