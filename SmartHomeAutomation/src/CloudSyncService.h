#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <initializer_list>

#include "Config.h"
#include "ControlEngine.h"
#include "StorageLayer.h"
#include "TimeKeeper.h"

class CloudSyncService {
 public:
  void begin(ControlEngine *engine, StorageLayer *storage, TimeKeeper *timeKeeper);
  void loop();
  bool enqueueLocalEvent(const String &eventJson);
  void requestStateSync();
  bool isConfigured() const;
  // WIFI RUNTIME START
  // Register (or re-register) this device in Supabase via device_self_register RPC.
  // Safe to call multiple times; the RPC is an upsert.
  bool registerDevice();
  // Push the current relay names and timer configuration to Supabase as part of
  // the device state snapshot.  Called once after first successful registration.
  bool syncConfigToCloud();
  // WIFI RUNTIME END

 private:
  struct QueuedCloudEvent {
    char json[512];
  };

  bool networkReady() const;
  bool httpRequest(const char *method,
                   const String &path,
                   const String &body,
                   int *statusCode,
                   String *response,
                   const char *preferHeader = nullptr);
  bool sendEventToCloud(const String &eventJson);
  bool syncStateSnapshot();
  void processRealtimeEventQueue();
  void flushStoredEventQueue();
  void pollRemoteCommands();
  bool applyRemoteCommand(const String &commandId, JsonObjectConst command, String *resultMessage);
  bool markRemoteCommand(const String &commandId, const char *status, bool ok, const String &message);
  bool shouldPersistEventForCloud(const String &eventJson) const;
  bool validateCommandToken(JsonObjectConst command) const;
  bool commandHasOnlyAllowedKeys(JsonObjectConst command, std::initializer_list<const char *> allowedKeys) const;
  bool rateLimitRelayCommand(size_t relayIndex);
  String deviceId();
  String restPath(const String &tablePath) const;
  String jsonString(const String &value) const;
  String urlEncode(const String &value) const;
  uint64_t nowEpoch() const;

  // SUPABASE REALTIME START
  // Persistent WebSocket connection to Supabase Realtime.
  // Subscribes to INSERT events on device_commands for this device so
  // new commands are delivered in real-time instead of only on the poll cadence.
  void beginRealtime();
  void loopRealtime();
  void onRealtimeWsEvent(WStype_t type, uint8_t *payload, size_t length);
  void sendRealtimeMsg(const String &json);
  void joinRealtimeChannel();
  String extractRealtimeHost() const;
  static void onRealtimeWsEventStatic(WStype_t type, uint8_t *payload, size_t length);
  static CloudSyncService *instance_;

  WebSocketsClient realtimeWs_;
  bool realtimeSubscribed_ = false;
  uint32_t lastRealtimeHeartbeatMs_ = 0;
  uint32_t realtimeMsgRef_ = 0;
  // SUPABASE REALTIME END

  ControlEngine *engine_ = nullptr;
  StorageLayer *storage_ = nullptr;
  TimeKeeper *timeKeeper_ = nullptr;
  QueueHandle_t eventQueue_ = nullptr;
  String deviceId_;
  uint32_t lastStateSyncMs_ = 0;
  uint32_t lastCommandPollMs_ = 0;
  uint32_t lastQueueFlushMs_ = 0;
  uint32_t lastRelayCommandMs_[RELAY_COUNT] = {};
  volatile bool stateDirty_ = false;
  bool configured_ = false;
};
