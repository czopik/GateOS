#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include "config_manager.h"

class MqttManager;
class CalibrationManager;
class LedController;

typedef void(*LearnCb)(bool);
typedef void(*TestCb)(unsigned long, unsigned long, bool, bool, bool);
typedef void(*ControlCb)(const String& action);
typedef void(*StatusCb)(JsonObject& out);
typedef void(*RemoteStateCb)(JsonObject& out);
typedef void(*DiagnosticsCb)(JsonObject& out);
typedef bool(*GateCalibrateCb)(const char* mode);

class MotorController;
class WebServerManager {
public:
  WebServerManager(ConfigManager* cfg);
  void begin();

  void setLearnCallback(LearnCb cb);
  void setTestCallback(TestCb cb);
  void setControlCallback(ControlCb cb);
  void setGateCalibrateCallback(GateCalibrateCb cb);
  void setStatusCallback(StatusCb cb);
  void setDiagnosticsCallback(DiagnosticsCb cb);
  void setRemoteStateCallback(RemoteStateCb cb);
  void setMqttManager(MqttManager* mqtt);
  void setCalibrationManager(CalibrationManager* calibration);
  void setLedController(LedController* led);
  void setMotorController(MotorController* motor);
  void setLearnState(bool enabled);

  void broadcastJson(const String& json);
  void broadcastJson(const char* json);
  void broadcastStatus();
  void broadcastEvent(const char* level, const char* message);

private:
  ConfigManager* cfg;
  MqttManager* mqtt = nullptr;
  CalibrationManager* calibration = nullptr;
  LedController* led = nullptr;
  MotorController* motor = nullptr;
  AsyncWebServer server{80};
  AsyncWebSocket ws{ "/ws" };
  bool fsMounted = false;

  void setupRoutes();
  bool isAuthorized(AsyncWebServerRequest* request) const;
  void sendUnauthorized(AsyncWebServerRequest* request) const;

  LearnCb learnCb = nullptr;
  TestCb testCb = nullptr;
  ControlCb controlCb = nullptr;
  GateCalibrateCb gateCalibrateCb = nullptr;
  StatusCb statusCb = nullptr;
  DiagnosticsCb diagnosticsCb = nullptr;
  RemoteStateCb remoteStateCb = nullptr;
  bool learnMode = false;
};
