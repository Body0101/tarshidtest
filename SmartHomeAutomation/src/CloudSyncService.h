#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
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
  // V2: Fetch friendly names and config from Supabase (device_fetch_config RPC).
  // Stores relay names in runtime for state JSON display only — NEVER used in
  // control logic (ControlEngine uses hardware channel IDs exclusively).
  bool fetchDeviceConfig();
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

  ControlEngine *engine_ = nullptr;
  StorageLayer *storage_ = nullptr;
  TimeKeeper *timeKeeper_ = nullptr;
  QueueHandle_t eventQueue_ = nullptr;
  String deviceId_;
  uint32_t lastStateSyncMs_ = 0;
  uint32_t lastCommandPollMs_ = 0;
  uint32_t lastQueueFlushMs_ = 0;
  uint32_t lastConfigFetchMs_ = 0;
  uint32_t lastRelayCommandMs_[RELAY_COUNT] = {};
  volatile bool stateDirty_ = false;
  bool configured_ = false;
};
