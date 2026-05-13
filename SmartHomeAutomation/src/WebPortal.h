#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <array>

#include "Config.h"
#include "ControlEngine.h"
#include "StorageLayer.h"
#include "TimeKeeper.h"

class WebPortal
{
public:
  void begin(ControlEngine *engine, StorageLayer *storage, TimeKeeper *timeKeeper);
  void end();
  void loop();
  void recoverAfterAccessPointRestart();
  bool isRunning() const;

  bool enqueueEvent(const String &eventJson, bool bufferIfOffline);
  uint16_t connectedClientCount() const;

private:
  struct QueuedEvent
  {
    bool bufferIfOffline;
    char json[400];
    char mac[20];
  };

  struct QueuedCommand
  {
    uint8_t clientId;
    char json[400];
  };

  struct CommandContextGuard
  {
    explicit CommandContextGuard(WebPortal *portal) : portal_(portal) {}
    ~CommandContextGuard();
    WebPortal *portal_ = nullptr;
  };

  void setupRoutes();
  void handleWsEvent(uint8_t clientId, WStype_t type, uint8_t *payload, size_t length);
  void onClientConnected(uint8_t clientId);
  void onClientDisconnected(uint8_t clientId);
  bool enqueueInboundCommand(uint8_t clientId, const String &payload);
  void handleClientMessage(uint8_t clientId, const String &payload);
  void sendToClient(uint8_t clientId, const String &json);
  void broadcast(const String &json);
  void flushPendingToClient(uint8_t clientId);
  void sendCommandAck(uint8_t clientId, bool ok, const String &message);
  void pushStateSnapshot(uint8_t clientId);
  void processInboundCommands();
  void processQueue();
  void scheduleStateBroadcast();
  void processPendingStateBroadcast();
  bool applyTimeSyncFromJson(const JsonDocument &doc, bool requireClockFields);
  // CAPTIVE PORTAL START
  void beginCaptivePortal();
  void ensureCaptivePortal();
  String captivePortalUrl() const;
  bool shouldRedirectToCaptivePortal();
  void sendCaptivePortalResponse(int httpCode);
  // CAPTIVE PORTAL END

  String normalizeLogPayload(const String &rawJson, const String &fallbackMac) const;
  String formatEpochClock(uint64_t epoch) const;
  void setCommandContext(uint8_t clientId);
  void clearCommandContext();
  String activeCommandMac();
  String resolveClientMac(uint8_t clientId);
  static String formatMac(const uint8_t mac[6]);
  static IPAddress ipFromAddr(uint32_t addr, bool reverseOrder);
  String handleSetTime(const String &body);
  uint16_t recalcConnectedClients();
  void syncConnectedClients(bool broadcastSnapshot);
  void updateClientCountInEngine();
  // ACCESS CONTROL START
  struct AuthContext
  {
    bool isAuthenticated;
    UserAccount user;
    uint32_t authTimestamp;
  };

  AccessControlRuntime accessControl_;
  bool validateMacAccess(const String &macAddress);
  bool authenticateUser(const String &macAddress, const String &password);
  bool checkPermission(const UserAccount &user, const String &requiredPermission);
  void updateLastAccess(const char *macAddress);
  String getConnectedClientMac(uint8_t clientId);
  String resolveClientMacFromIP(IPAddress ip);
  String macFromHttpClient();
  bool isAuthorizedRoute(const String &uri);
  // ACCESS CONTROL END

  static void onWsEventStatic(uint8_t clientId, WStype_t type, uint8_t *payload, size_t length);
  static WebPortal *instance_;

  ControlEngine *engine_ = nullptr;
  StorageLayer *storage_ = nullptr;
  TimeKeeper *timeKeeper_ = nullptr;

  WebServer server_{HTTP_PORT};
  WebSocketsServer socket_{WS_PORT};
  // CAPTIVE PORTAL START
  DNSServer dnsServer_;
  bool captivePortalEnabled_ = false;
  IPAddress captivePortalIp_{0, 0, 0, 0};
  // CAPTIVE PORTAL END
  QueueHandle_t outboundQueue_ = nullptr;
  QueueHandle_t inboundQueue_ = nullptr;
  SemaphoreHandle_t contextMutex_ = nullptr;
  bool initialized_ = false;
  bool running_ = false;
  std::array<bool, WS_MAX_CLIENTS> clients_{};
  std::array<String, WS_MAX_CLIENTS> clientMacs_{};
  uint16_t connectedClients_ = 0;
  uint32_t lastClientRefreshMs_ = 0;
  bool stateBroadcastPending_ = false;
  bool commandContextActive_ = false;
  TaskHandle_t commandContextTask_ = nullptr;
  String commandContextMac_ = "SYSTEM";
  uint32_t commandContextExpiryMs_ = 0;
};
