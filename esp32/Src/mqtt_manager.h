#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "config_manager.h"

class MqttManager {
public:
  MqttManager();

  void begin(const MQTTConfig& cfg);
  void applyConfig(const MQTTConfig& cfg);
  void loop();
  void setConnectAllowed(bool allowed) { connectAllowed = allowed; }

  bool connected();
  bool enabled() const;
  int state();
  const String& lastError() const;

  void setCallback(MQTT_CALLBACK_SIGNATURE);
  bool publish(const char* topic, const char* payload, bool retain);
  bool subscribe(const char* topic);

  bool testPublish(String& outTopic, String& outMsg);
  const char* topicState() const;
  const char* topicCommand() const;
  const char* topicEvents() const;
  const char* topicTelemetry() const;
  const char* topicPosition() const;
  const char* topicGateCmd() const;
  const char* topicGateSetMax() const;
  const char* topicGateCalibrate() const;
  const char* topicLedState() const;
  const char* topicLedCmd() const;
  const char* topicMotionState() const;
  const char* topicMotionPosition() const;

private:
  WiFiClient wifiClient;
  PubSubClient client;
  MQTTConfig cfg;
  String lastErrorMsg;
  unsigned long lastConnectAttemptMs = 0;
  bool enabledFlag = false;
  bool connectAllowed = true;
  bool wasConnected = false;
  int lastState = 0;
  char topicStateBuf[64] = {0};
  char topicCommandBuf[64] = {0};
  char topicEventsBuf[64] = {0};
  char topicTelemetryBuf[64] = {0};
  char topicPosBuf[64] = {0};
  char topicGateCmdBuf[64] = {0};
  char topicGateSetMaxBuf[64] = {0};
  char topicGateCalibrateBuf[64] = {0};
  char topicLedStateBuf[64] = {0};
  char topicLedCmdBuf[64] = {0};
  char topicMotionStateBuf[64] = {0};
  char topicMotionPositionBuf[64] = {0};

  bool ensureConnected();
  bool connectNow();
  void resetStatus();
  void buildTopics();
  String buildClientId() const;
  String buildLwtTopic() const;
  String buildLwtPayload() const;
};
