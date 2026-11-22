#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include "config_manager.h"

class MqttManager;
class LedController;

struct ControlResult {
  bool ok = false;
  bool applied = false;
  int httpCode = 500;
  const char* status = "error";
  const char* error = "not_ready";
};

typedef void(*LearnCb)(bool);
typedef void(*TestCb)(unsigned long, unsigned long, bool, bool, bool);
typedef ControlResult(*ControlCb)(const String& action);
typedef void(*StatusCb)(JsonObject& out);
typedef void(*StatusLiteCb)(JsonObject& out);
typedef void(*RemoteStateCb)(JsonObject& out);
typedef void(*DiagnosticsCb)(JsonObject& out);
typedef bool(*GateCalibrateCb)(const char* mode);
typedef bool(*OtaActiveCb)();
typedef void(*OtaHttpStateCb)(bool active);
typedef bool(*GateBusyCb)();

class MotorController;

struct WebRuntimeStats {
  uint32_t apiReqCount = 0;
  uint32_t statusReqCount = 0;
  uint32_t statusLiteReqCount = 0;
  uint32_t statusErrors = 0;
  uint32_t statusSlowCount = 0;
  uint32_t lastApiReqMs = 0;
  uint32_t lastStatusReqMs = 0;
  uint32_t lastStatusDurationUs = 0;
  uint32_t maxStatusDurationUs = 0;
  uint32_t lastMaintenanceMs = 0;
  uint32_t lastWsConnectMs = 0;
  uint32_t lastWsDisconnectMs = 0;
  uint16_t wsClients = 0;
};

class WebServerManager {
public:
  WebServerManager(ConfigManager* cfg);
  void begin();

  void setLearnCallback(LearnCb cb);
  void setTestCallback(TestCb cb);
  void setControlCallback(ControlCb cb);
  void setGateCalibrateCallback(GateCalibrateCb cb);
  void setStatusCallback(StatusCb cb);
  void setStatusLiteCallback(StatusLiteCb cb);
  void setDiagnosticsCallback(DiagnosticsCb cb);
  void setRemoteStateCallback(RemoteStateCb cb);
  void setOtaActiveCallback(OtaActiveCb cb);
  void setOtaHttpStateCallback(OtaHttpStateCb cb) { otaHttpStateCb = cb; }
  void setGateBusyCallback(GateBusyCb cb) { gateBusyCb = cb; }
  void setMqttManager(MqttManager* mqtt);
  void setLedController(LedController* led);
  void setMotorController(MotorController* motor);
  void setLearnState(bool enabled);

  void broadcastJson(const String& json);
  void broadcastJson(const char* json);
  void broadcastStatus();
  void broadcastEvent(const char* level, const char* message);
  void maintenance();
  WebRuntimeStats runtimeStats() const;

private:
  ConfigManager* cfg;
  MqttManager* mqtt = nullptr;
  LedController* led = nullptr;
  MotorController* motor = nullptr;
  AsyncWebServer server{80};
  AsyncWebSocket ws{ "/ws" };
  bool fsMounted = false;
  // HTTP OTA upload state (written from async_tcp; watchdog in maintenance()).
  volatile bool otaHttpUploadStarted = false;
  AsyncWebServerRequest* otaHttpOwner = nullptr;
  volatile uint32_t otaHttpLastChunkMs = 0;
  bool otaHttpOk = false;
  char otaHttpErr[48] = {0};
  WebRuntimeStats stats;

  void setupRoutes();
  bool isAuthorized(AsyncWebServerRequest* request) const;
  void sendUnauthorized(AsyncWebServerRequest* request) const;
  void finishBodyRequest(AsyncWebServerRequest* request) const;
  void handleOtaChunk(int command, const char* tag, AsyncWebServerRequest* request,
                      const String& filename, size_t index, uint8_t* data, size_t len, bool final);
  void handleOtaDone(AsyncWebServerRequest* request, const char* target);
  void otaHttpFinish(bool ok, const char* err);

  LearnCb learnCb = nullptr;
  TestCb testCb = nullptr;
  ControlCb controlCb = nullptr;
  GateCalibrateCb gateCalibrateCb = nullptr;
  StatusCb statusCb = nullptr;
  StatusLiteCb statusLiteCb = nullptr;
  DiagnosticsCb diagnosticsCb = nullptr;
  RemoteStateCb remoteStateCb = nullptr;
  OtaActiveCb otaActiveCb = nullptr;
  OtaHttpStateCb otaHttpStateCb = nullptr;
  GateBusyCb gateBusyCb = nullptr;
  bool learnMode = false;
};
