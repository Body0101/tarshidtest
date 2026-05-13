#pragma once

#include <Arduino.h>
#include <functional>
#include <vector>

#include "StorageLayer.h"
#include "SystemTypes.h"
#include "TimeKeeper.h"

class ControlEngine {
 public:
  using EventCallback = std::function<void(const String &eventJson, bool bufferIfOffline)>;

  void begin(SystemRuntime *runtime, StorageLayer *storage, TimeKeeper *timeKeeper, SemaphoreHandle_t stateMutex);
  void setEventCallback(EventCallback callback);

  void tickFast();
  void tickHousekeeping();
  void refreshOutputs();

  bool setManualMode(size_t relayIndex, RelayMode mode, String *error);
  bool setTimer(size_t relayIndex, uint32_t durationMinutes, RelayState targetState, String *error);
  bool cancelTimer(size_t relayIndex);
  // POWER RESET START
  bool resetConsumption(String *error);
  // POWER RESET END
  // PIR MAPPING START
  bool setPirMapping(const std::vector<PIRMapping> &mappings, String *error);
  // PIR MAPPING END
  // RATED DYNAMIC START
  bool setRatedPower(size_t relayIndex, float watts, String *error);
  // RATED DYNAMIC END
  bool setEnergyTrackingEnabled(bool enabled, String *error = nullptr);
  void updateConnectedClients(uint16_t clients);

  String buildStateJson() const;
  String buildTimerJson(size_t relayIndex) const;

 private:
  struct Decision {
    RelayState state;
    ControlSource source;
  };

  void publishEventLocked(const String &logType,
                          const String &eventName,
                          const String &msg,
                          int channel,
                          bool bufferIfOffline,
                          uint64_t forcedTs = 0) const;
  void applyNightLockTransitionLocked(bool activate, uint64_t nowEpoch);
  void markEnergyTrackingStartLocked(size_t relayIndex, RelayRuntime &relay, uint64_t nowEpoch);
  void finalizeEnergyTrackingLocked(size_t relayIndex, RelayRuntime &relay, uint64_t nowEpoch, const String &stopEvent);
  void clearEnergyTrackingLocked(RelayRuntime &relay);
  void publishEnergyUpdateLocked(size_t relayIndex, float lastWh, float totalWh) const;
  bool blockNightLockFeatureLocked(const String &eventName, const String &message, int channel, String *error) const;
  uint64_t nowEpochLocked() const;
  bool canTurnOnLocked() const;
  void processPirInputsLocked(uint64_t nowEpoch);
  Decision evaluateRelayLocked(size_t relayIndex, uint64_t nowEpoch);
  void applyDecisionsLocked(const std::vector<Decision> &decisions, uint64_t nowEpoch);
  uint64_t effectiveOnSecondsLocked(const RelayRuntime &relay, uint64_t nowEpoch) const;
  void closeActiveOnWindowLocked(RelayRuntime &relay, uint64_t nowEpoch);
  bool withLock(const std::function<void()> &fn) const;

  SystemRuntime *runtime_ = nullptr;
  StorageLayer *storage_ = nullptr;
  TimeKeeper *timeKeeper_ = nullptr;
  SemaphoreHandle_t stateMutex_ = nullptr;
  EventCallback eventCallback_;
  uint64_t lastStatsFlushEpoch_ = 0;
};
