#include "ControlEngine.h"

#include <ArduinoJson.h>
#include <limits.h>
#include <vector>

#include "Config.h"
#include "Utils.h"

namespace {
uint8_t relayOutputLevel(RelayState state) {
  if (RELAY_ACTIVE_LOW) {
    return state == RelayState::ON ? LOW : HIGH;
  }
  return state == RelayState::ON ? HIGH : LOW;
}
}  // namespace

void ControlEngine::begin(SystemRuntime *runtime, StorageLayer *storage, TimeKeeper *timeKeeper, SemaphoreHandle_t stateMutex) {
  runtime_ = runtime;
  storage_ = storage;
  timeKeeper_ = timeKeeper;
  stateMutex_ = stateMutex;

  // Configure relay outputs and PIR inputs once at boot.
  for (size_t i = 0; i < RELAY_COUNT; ++i) {
    pinMode(RELAY_CONFIG[i].relayPin, OUTPUT);
    // Drive the physical relay pin to the logical OFF state on boot.
    // This keeps the UI meaning of ON/OFF aligned with the hardware board polarity.
    digitalWrite(RELAY_CONFIG[i].relayPin, relayOutputLevel(RelayState::OFF));
  }
  for (size_t i = 0; i < PIR_COUNT; ++i) {
    pinMode(PIR_CONFIG[i].pin, INPUT);
  }

  withLock([&]() {
    const uint64_t nowEpoch = nowEpochLocked();
    for (size_t i = 0; i < RELAY_COUNT; ++i) {
      RelayRuntime &relay = runtime_->relays[i];

      // Reboot reconstruction guard:
      // keep valid persisted timers intact across reboot so they can reconstruct once
      // client time is available again; only malformed plans are cleared here.
      const bool timerShapeInvalid = (relay.timer.durationMinutes == 0) || (relay.timer.startEpoch == 0) ||
                                     (relay.timer.endEpoch == 0) || (relay.timer.endEpoch <= relay.timer.startEpoch);
      if (relay.timer.active && timerShapeInvalid) {
        relay.timer.active = false;
        relay.timer.startEpoch = 0;
        relay.timer.endEpoch = 0;
        relay.timer.durationMinutes = 0;
        relay.timer.targetState = RelayState::OFF;
        relay.timer.restorePending = false;
        storage_->persistTimer(i, relay.timer);
      }

      // If a relay was restored ON from flash, restart its open ON window.
      if (relay.appliedState == RelayState::ON && relay.stats.lastOnEpoch == 0) {
        relay.stats.lastOnEpoch = nowEpoch;
      }
    }
  });

  refreshOutputs();
}

void ControlEngine::setEventCallback(EventCallback callback) { eventCallback_ = callback; }

void ControlEngine::tickFast() {
  withLock([&]() {
    const uint64_t nowEpoch = nowEpochLocked();
    const DayPhase previousPhase = runtime_->dayPhase;
    const bool previousNightLock = runtime_->nightLockActive;
    runtime_->dayPhase = timeKeeper_->currentDayPhase();
    runtime_->timeValid = timeKeeper_->hasValidTime();

    // Forced Night Lock:
    // - Active only when time is valid and current phase is NIGHT.
    // - On transition, force all relays OFF and cancel active timers.
    const bool shouldNightLock = runtime_->timeValid && runtime_->dayPhase == DayPhase::NIGHT;
    if (shouldNightLock != previousNightLock || runtime_->dayPhase != previousPhase) {
      applyNightLockTransitionLocked(shouldNightLock, nowEpoch);
    }

    processPirInputsLocked(nowEpoch);

    std::vector<Decision> decisions(RELAY_COUNT);
    for (size_t i = 0; i < RELAY_COUNT; ++i) {
      decisions[i] = evaluateRelayLocked(i, nowEpoch);
    }
    applyDecisionsLocked(decisions, nowEpoch);
  });
}

void ControlEngine::tickHousekeeping() {
  withLock([&]() {
    const uint64_t nowEpoch = nowEpochLocked();
    if (timeKeeper_->hasValidTime()) {
      const uint32_t dayToken = timeKeeper_->currentDayToken();
      if (dayToken != 0 && dayToken != storage_->loadLastCleanupDay()) {
        storage_->cleanupDaily(nowEpoch);
        storage_->persistLastCleanupDay(dayToken);
        publishEventLocked("TIMER", "storage.cleanup", "Daily log cleanup complete.", -1, false);
      }
    }

    // Persist ON-duration stats infrequently to reduce flash wear.
    if (timeKeeper_->hasValidTime() && (lastStatsFlushEpoch_ == 0 || nowEpoch >= lastStatsFlushEpoch_ + STATS_FLUSH_INTERVAL_SECONDS)) {
      lastStatsFlushEpoch_ = nowEpoch;
      for (size_t i = 0; i < RELAY_COUNT; ++i) {
        RelayRuntime &relay = runtime_->relays[i];
        if (relay.appliedState == RelayState::ON && relay.stats.lastOnEpoch > 0 && nowEpoch > relay.stats.lastOnEpoch) {
          relay.stats.accumulatedOnSeconds += (nowEpoch - relay.stats.lastOnEpoch);
          relay.stats.lastOnEpoch = nowEpoch;
          storage_->persistRelayStats(i, relay.stats);
        }
      }
    }
  });
}

void ControlEngine::refreshOutputs() {
  withLock([&]() {
    for (size_t i = 0; i < RELAY_COUNT; ++i) {
      digitalWrite(RELAY_CONFIG[i].relayPin, relayOutputLevel(runtime_->relays[i].appliedState));
    }
  });
}

bool ControlEngine::setManualMode(size_t relayIndex, RelayMode mode, String *error) {
  if (relayIndex >= RELAY_COUNT) {
    if (error) *error = "Invalid relay index.";
    return false;
  }

  bool accepted = false;
  withLock([&]() {
    const uint64_t nowEpoch = nowEpochLocked();
    const uint64_t timerEpoch = timeKeeper_->nowUserEpoch();

    RelayRuntime &relay = runtime_->relays[relayIndex];

    // Manual ON is blocked while Night Lock is active.
    if (runtime_->nightLockActive && mode == RelayMode::ON) {
      if (error) *error = "Night Lock Active";
      publishEventLocked("ERROR",
                         "manual.blocked_night_lock",
                         "Command Blocked by Night Lock",
                         static_cast<int>(relayIndex),
                         true);
      return;
    }

    // Night mode overrides conflicting manual ON requests.
    if (mode == RelayMode::ON && !canTurnOnLocked()) {
      if (error) *error = "Night mode blocks ON actions.";
      publishEventLocked("ERROR", "manual.blocked", "Manual ON blocked by night mode.", static_cast<int>(relayIndex), true);
      return;
    }

    // Manual ON/OFF should immediately override any running timer.
    if (mode != RelayMode::AUTO && relay.timer.active) {
      finalizeEnergyTrackingLocked(relayIndex, relay, timerEpoch > 0 ? timerEpoch : nowEpoch, "timer.canceled");
      relay.timer.active = false;
      relay.timer.startEpoch = 0;
      relay.timer.endEpoch = 0;
      relay.timer.targetState = RelayState::OFF;
      relay.timer.durationMinutes = 0;
      relay.timer.restorePending = false;
      storage_->persistTimer(relayIndex, relay.timer);
      publishEventLocked("TIMER",
                         "timer.canceled",
                         "Timer canceled by manual override.",
                         static_cast<int>(relayIndex),
                         true,
                         timerEpoch);
    }

    relay.manualMode = mode;
    storage_->persistManualMode(relayIndex, mode);
    publishEventLocked("TIMER",
                       "manual.changed",
                       "Manual mode set to " + String(relayModeToText(mode)) + ".",
                       static_cast<int>(relayIndex),
                       true);
    accepted = true;
  });
  return accepted;
}

bool ControlEngine::setTimer(size_t relayIndex, uint32_t durationMinutes, RelayState targetState, String *error) {
  if (relayIndex >= RELAY_COUNT) {
    if (error) *error = "Invalid relay index.";
    return false;
  }
  if (durationMinutes == 0) {
    if (error) *error = "Duration must be at least 1 minute.";
    return false;
  }

  bool accepted = false;
  withLock([&]() {
    RelayRuntime &relay = runtime_->relays[relayIndex];
    const uint64_t nowEpoch = timeKeeper_->nowUserEpoch();
    if (!timeKeeper_->hasUserTime() || nowEpoch == 0) {
      if (error) *error = "Sync from user device first (open web page).";
      publishEventLocked("ERROR",
                         "timer.blocked",
                         "Timer rejected because user-device time is not synchronized.",
                         static_cast<int>(relayIndex),
                         true,
                         0);
      return;
    }

    // Night Lock freezes timer creation entirely so no queued automation can
    // slip through from a stale UI while the system is forced into safe OFF mode.
    if (blockNightLockFeatureLocked("timer.blocked_night_lock",
                                    "Command Blocked by Night Lock",
                                    static_cast<int>(relayIndex),
                                    error)) {
      return;
    }

    // Per requirements: timer can only be created while relay is in manual ON/OFF mode.
    if (relay.manualMode == RelayMode::AUTO) {
      if (error) *error = "Set relay to manual ON/OFF before starting a timer.";
      publishEventLocked("ERROR", "timer.blocked", "Timer requires manual ON/OFF mode first.", static_cast<int>(relayIndex), true);
      return;
    }

    if (targetState == RelayState::ON && !canTurnOnLocked()) {
      if (error) *error = "Night mode blocks ON timers.";
      publishEventLocked("ERROR", "timer.blocked", "Timer ON request blocked by night mode.", static_cast<int>(relayIndex), true);
      return;
    }

    TimerPlan plan{};
    plan.active = true;
    plan.startEpoch = nowEpoch;
    plan.targetState = targetState;
    plan.previousState = relay.appliedState;
    plan.previousManualMode = relay.manualMode;
    plan.durationMinutes = durationMinutes;
    plan.restorePending = false;
    const uint64_t durationSeconds = static_cast<uint64_t>(durationMinutes) * 60ULL;
    if (durationSeconds == 0 || nowEpoch > (UINT64_MAX - durationSeconds)) {
      if (error) *error = "Invalid timer duration.";
      publishEventLocked("ERROR", "timer.blocked", "Timer rejected due invalid duration/overflow.", static_cast<int>(relayIndex), true, nowEpoch);
      return;
    }
    plan.endEpoch = nowEpoch + durationSeconds;
    if (plan.endEpoch <= plan.startEpoch) {
      if (error) *error = "Invalid timer boundaries.";
      publishEventLocked("ERROR", "timer.blocked", "Timer rejected due invalid start/end boundaries.", static_cast<int>(relayIndex), true, nowEpoch);
      return;
    }
    relay.timer = plan;

    // Energy tracking uses a lightweight arm/start approach:
    // - Arm timestamp when ON timer is created.
    // - Activate only when relay is actually ON via TIMER source.
    if (runtime_->energyTrackingEnabled && targetState == RelayState::ON) {
      relay.energyStartEpoch = nowEpoch;
      relay.energyTrackingActive = false;
    } else {
      clearEnergyTrackingLocked(relay);
    }

    // Allow timer logic to control output while it is active.
    relay.manualMode = RelayMode::AUTO;

    relay.stats.timerUses += 1;
    if (relay.stats.totalTimerMinutes > (UINT32_MAX - durationMinutes)) {
      relay.stats.totalTimerMinutes = UINT32_MAX;
    } else {
      relay.stats.totalTimerMinutes += durationMinutes;
    }

    // Capture a fresh user-time sync point at timer start for accurate post-reboot reconstruction.
    timeKeeper_->persistUserSyncPoint();

    storage_->persistTimer(relayIndex, plan);
    storage_->persistManualMode(relayIndex, relay.manualMode);
    storage_->persistRelayStats(relayIndex, relay.stats);

    publishEventLocked("TIMER",
                       "timer.started",
                       "Timer started for " + String(durationMinutes) + " minute(s), target " +
                           String(relayStateToText(targetState)) + ".",
                       static_cast<int>(relayIndex),
                       true,
                       nowEpoch);
    accepted = true;
  });
  return accepted;
}

bool ControlEngine::cancelTimer(size_t relayIndex) {
  if (relayIndex >= RELAY_COUNT) {
    return false;
  }
  bool canceled = false;
  withLock([&]() {
    const uint64_t nowEpoch = nowEpochLocked();
    const uint64_t timerEpoch = timeKeeper_->nowUserEpoch();
    RelayRuntime &relay = runtime_->relays[relayIndex];
    if (!relay.timer.active && !relay.timer.restorePending) {
      return;
    }
    finalizeEnergyTrackingLocked(relayIndex, relay, timerEpoch > 0 ? timerEpoch : nowEpoch, "timer.canceled");
    relay.timer.active = false;
    relay.timer.startEpoch = 0;
    relay.timer.endEpoch = 0;
    relay.timer.durationMinutes = 0;
    relay.timer.targetState = RelayState::OFF;
    relay.timer.restorePending = false;
    storage_->persistTimer(relayIndex, relay.timer);
    publishEventLocked("TIMER", "timer.canceled", "Timer canceled.", static_cast<int>(relayIndex), true, timerEpoch);
    canceled = true;
  });
  return canceled;
}

// PIR MAPPING START
bool ControlEngine::setPirMapping(const std::vector<PIRMapping> &mappings, String *error) {
  bool updated = false;
  withLock([&]() {
    if (blockNightLockFeatureLocked("pir_mapping.blocked_night_lock",
                                    "PIR mapping change blocked by Night Lock",
                                    -1,
                                    error)) {
      return;
    }
    if (mappings.size() != PIR_COUNT) {
      if (error) *error = "Expected one mapping entry per PIR.";
      return;
    }
    for (size_t i = 0; i < PIR_COUNT; ++i) {
      runtime_->pirMap[i] = mappings[i];
      runtime_->pirMap[i].relayMask &= relayMaskForCount(RELAY_COUNT);
      storage_->persistPirMapping(i, runtime_->pirMap[i]);
    }
    updated = true;
  });

  if (!updated && error && error->isEmpty()) {
    *error = "Could not save PIR mapping.";
  }
  return updated;
}
// PIR MAPPING END

// POWER RESET START
bool ControlEngine::resetConsumption(String *error) {
  bool reset = false;
  withLock([&]() {
    if (blockNightLockFeatureLocked("consumption.blocked_night_lock",
                                    "Power consumption reset blocked by Night Lock",
                                    -1,
                                    error)) {
      return;
    }
    const uint64_t nowEpoch = nowEpochLocked();

    // Clear only consumption-related counters. Relay modes, timers, and settings stay untouched.
    for (size_t i = 0; i < RELAY_COUNT; ++i) {
      RelayRuntime &relay = runtime_->relays[i];
      relay.stats.accumulatedOnSeconds = 0;
      relay.stats.totalEnergyWh = 0.0f;
      relay.stats.lastEnergyWh = 0.0f;

      // Rebase active runtime windows so future accumulation resumes from "now"
      // without re-counting usage that existed before the reset action.
      relay.stats.lastOnEpoch = relay.appliedState == RelayState::ON ? nowEpoch : 0;
      if (runtime_->energyTrackingEnabled && relay.timer.active && relay.timer.targetState == RelayState::ON) {
        relay.energyStartEpoch = nowEpoch;
      } else if (!relay.energyTrackingActive) {
        relay.energyStartEpoch = 0;
      }

      storage_->persistRelayStats(i, relay.stats);
      storage_->persistRelayEnergyStats(i, relay.stats.totalEnergyWh, relay.stats.lastEnergyWh);
    }
    reset = true;
  });

  if (!reset && error) {
    *error = "Could not reset consumption counters.";
  }
  return reset;
}
// POWER RESET END

// RATED DYNAMIC START
bool ControlEngine::setRatedPower(size_t relayIndex, float watts, String *error) {
  if (relayIndex >= RELAY_COUNT) {
    if (error) *error = "Invalid relay index.";
    return false;
  }
  if (!(watts > 0.0f) || watts > 50000.0f) {
    if (error) *error = "Rated power must be between 0 and 50000 watts.";
    return false;
  }

  bool updated = false;
  withLock([&]() {
    if (blockNightLockFeatureLocked("rated_power.blocked_night_lock",
                                    "Rated power change blocked by Night Lock",
                                    static_cast<int>(relayIndex),
                                    error)) {
      return;
    }
    RelayRuntime &relay = runtime_->relays[relayIndex];
    if (relay.ratedPowerLocked && !ALLOW_RATED_RESET) {
      if (error) *error = "Rated power is already locked.";
      return;
    }

    relay.ratedPowerWatts = watts;
    relay.ratedPowerLocked = true;
    storage_->persistRatedPower(relayIndex, relay.ratedPowerWatts, relay.ratedPowerLocked);
    updated = true;
  });

  if (!updated && error && error->isEmpty()) {
    *error = "Could not save rated power.";
  }
  return updated;
}
// RATED DYNAMIC END

bool ControlEngine::setEnergyTrackingEnabled(bool enabled, String *error) {
  bool updated = false;
  withLock([&]() {
    if (blockNightLockFeatureLocked("energy_tracking.blocked_night_lock",
                                    "Energy tracking change blocked by Night Lock",
                                    -1,
                                    error)) {
      return;
    }
    if (runtime_->energyTrackingEnabled == enabled) {
      updated = true;
      return;
    }
    runtime_->energyTrackingEnabled = enabled;
    storage_->persistEnergyTrackingEnabled(enabled);

    if (!enabled) {
      // Drop in-flight tracking windows when user disables feature.
      for (size_t i = 0; i < RELAY_COUNT; ++i) {
        clearEnergyTrackingLocked(runtime_->relays[i]);
      }
    }

    publishEventLocked("TIMER",
                       "energy_tracking.changed",
                       String("Energy tracking ") + (enabled ? "enabled." : "disabled."),
                       -1,
                       true);
    updated = true;
  });
  if (!updated && error && error->isEmpty()) {
    *error = "Could not update energy tracking.";
  }
  return updated;
}

void ControlEngine::updateConnectedClients(uint16_t clients) {
  withLock([&]() {
    const bool oldManualWeb = runtime_->connectedClients > 0;
    const bool newManualWeb = clients > 0;
    runtime_->connectedClients = clients;

    // Keep the last user-selected manual mode persisted across page reloads/reconnects.
    // Automatic PIR behavior still remains unchanged while no clients are connected
    // because evaluateRelayLocked() only applies manual ON/OFF when connectedClients > 0.
    // This preserves user settings in NVS without forcing them back to AUTO on refresh.

    if (oldManualWeb != newManualWeb) {
      publishEventLocked("TIMER",
                         "mode.changed",
                         newManualWeb ? "A web client connected. Saved relay modes remain active."
                                      : "All web clients disconnected. Relay modes continue from saved state.",
                         -1,
                         false);
    }
  });
}

String ControlEngine::buildStateJson() const {
  String payload = "{}";
  withLock([&]() {
    JsonDocument doc;
    const uint64_t nowEpoch = nowEpochLocked();
    bool anyManual = false;
    bool anyAuto = false;
    bool anyTimer = false;
    for (size_t i = 0; i < RELAY_COUNT; ++i) {
      anyManual = anyManual || (runtime_->relays[i].manualMode != RelayMode::AUTO);
      anyAuto = anyAuto || (runtime_->relays[i].manualMode == RelayMode::AUTO);
      anyTimer = anyTimer || runtime_->relays[i].timer.active;
    }

    doc["type"] = "state_snapshot";
    doc["ts"] = nowEpoch;
    doc["dayPhase"] = dayPhaseToText(runtime_->dayPhase);
    doc["timeValid"] = runtime_->timeValid;
    doc["connectedClients"] = runtime_->connectedClients;
    doc["relayCount"] = RELAY_COUNT;
    doc["pirCount"] = PIR_COUNT;
    // Expose the actual operating mode derived from persisted relay settings
    // instead of current client connectivity.
    if (anyTimer) {
      doc["systemMode"] = "TIMER";
    } else if (anyManual && anyAuto) {
      doc["systemMode"] = "MIXED";
    } else if (anyManual) {
      doc["systemMode"] = "MANUAL";
    } else {
      doc["systemMode"] = "AUTO";
    }
    doc["energyTrackingEnabled"] = runtime_->energyTrackingEnabled;
    doc["nightLock"] = runtime_->nightLockActive;
    JsonArray relays = doc["relays"].to<JsonArray>();
    for (size_t i = 0; i < RELAY_COUNT; ++i) {
      JsonObject relay = relays.add<JsonObject>();
      const RelayRuntime &r = runtime_->relays[i];
      const uint64_t onSeconds = effectiveOnSecondsLocked(r, nowEpoch);
      relay["index"] = i;
      relay["name"] = RELAY_CONFIG[i].name;
      relay["state"] = relayStateToText(r.appliedState);
      relay["source"] = sourceToText(r.appliedSource);
      relay["manualMode"] = relayModeToText(r.manualMode);
      relay["timerActive"] = r.timer.active;
      relay["timerStart"] = r.timer.startEpoch;
      relay["timerEnd"] = r.timer.endEpoch;
      relay["timerTarget"] = relayStateToText(r.timer.targetState);
      relay["timerMinutes"] = r.timer.durationMinutes;
      relay["autoHoldUntil"] = r.autoHoldUntilEpoch;
      relay["timerUses"] = r.stats.timerUses;
      relay["totalTimerMinutes"] = r.stats.totalTimerMinutes;
      relay["lastEnergyWh"] = r.stats.lastEnergyWh;
      relay["totalEnergyWh"] = r.stats.totalEnergyWh;
      relay["onSeconds"] = onSeconds;
      relay["powerWh"] = runtime_->energyTrackingEnabled ? r.stats.totalEnergyWh : 0.0f;
      // RATED DYNAMIC START
      relay["powerW"] = r.ratedPowerWatts;
      relay["powerLocked"] = r.ratedPowerLocked;
      // RATED DYNAMIC END
    }
    JsonArray pirs = doc["pirs"].to<JsonArray>();
    for (size_t i = 0; i < PIR_COUNT; ++i) {
      JsonObject pir = pirs.add<JsonObject>();
      const PirRuntime &p = runtime_->pirs[i];
      pir["index"] = i;
      pir["name"] = PIR_CONFIG[i].name;
      pir["value"] = p.stableValue;
      pir["lastTrigger"] = p.lastTriggerEpoch;
      // PIR MAPPING START
      const PIRMapping &mapping = runtime_->pirMap[i];
      pir["relayMask"] = mapping.relayMask & relayMaskForCount(RELAY_COUNT);
      JsonArray mappedRelays = pir["relays"].to<JsonArray>();
      for (size_t relayIndex = 0; relayIndex < RELAY_COUNT; ++relayIndex) {
        mappedRelays.add(mapping.controlsRelay(relayIndex));
      }
      // Backward-compatible fields for existing two-relay clients.
      pir["relayA"] = RELAY_COUNT > 0 ? mapping.controlsRelay(0) : false;
      pir["relayB"] = RELAY_COUNT > 1 ? mapping.controlsRelay(1) : false;
      // PIR MAPPING END
    }
    serializeJson(doc, payload);
  });
  return payload;
}

String ControlEngine::buildTimerJson(size_t relayIndex) const {
  if (relayIndex >= RELAY_COUNT) {
    return "{}";
  }
  String payload = "{}";
  withLock([&]() {
    JsonDocument doc;
    const RelayRuntime &relay = runtime_->relays[relayIndex];
    doc["type"] = "timer_status";
    doc["channel"] = relayIndex;
    doc["active"] = relay.timer.active;
    doc["startEpoch"] = relay.timer.startEpoch;
    doc["endEpoch"] = relay.timer.endEpoch;
    doc["target"] = relayStateToText(relay.timer.targetState);
    doc["minutes"] = relay.timer.durationMinutes;
    serializeJson(doc, payload);
  });
  return payload;
}

bool ControlEngine::blockNightLockFeatureLocked(const String &eventName,
                                                const String &message,
                                                int channel,
                                                String *error) const {
  if (!runtime_->nightLockActive) {
    return false;
  }
  if (error) {
    *error = "Night Lock Active";
  }
  publishEventLocked("ERROR", eventName, message, channel, true);
  return true;
}

uint64_t ControlEngine::nowEpochLocked() const {
  return timeKeeper_->nowEpoch();
}

bool ControlEngine::canTurnOnLocked() const {
  if (runtime_->nightLockActive) {
    return false;
  }
  if (!timeKeeper_->hasValidTime()) {
    return true;
  }
  return runtime_->dayPhase == DayPhase::DAY;
}

void ControlEngine::processPirInputsLocked(uint64_t nowEpoch) {
  for (size_t i = 0; i < PIR_COUNT; ++i) {
    PirRuntime &pir = runtime_->pirs[i];
    const bool raw = digitalRead(PIR_CONFIG[i].pin) == HIGH;
    if (raw != pir.rawValue) {
      pir.rawValue = raw;
      pir.lastChangeMs = millis();
    }

    const bool debouncePassed = (millis() - pir.lastChangeMs) >= PIR_DEBOUNCE_MS;
    if (!debouncePassed || pir.stableValue == pir.rawValue) {
      continue;
    }

    pir.stableValue = pir.rawValue;
    if (!pir.stableValue) {
      // Publish sensor idle transitions so the UI can clear the activity panel.
      publishEventLocked("TIMER",
                         "pir.idle",
                         String(PIR_CONFIG[i].name) + " returned to idle.",
                         static_cast<int>(i),
                         true);
      continue;
    }
    // Track sensor activity regardless of network presence so the visual
    // activity section stays accurate for connected users.
    pir.lastTriggerEpoch = nowEpoch;
    if (runtime_->nightLockActive) {
      continue;
    }
    if (!canTurnOnLocked()) {
      publishEventLocked("ERROR", "pir.blocked", "Motion ignored by night mode.", static_cast<int>(i), true);
      continue;
    }

    // PIR MAPPING START
    const uint64_t relayMask = runtime_->pirMap[i].relayMask & relayMaskForCount(RELAY_COUNT);
    // PIR MAPPING END
    for (size_t relayIndex = 0; relayIndex < RELAY_COUNT; ++relayIndex) {
      if ((relayMask & relayMaskForRelay(relayIndex)) == 0) {
        continue;
      }
      RelayRuntime &relay = runtime_->relays[relayIndex];
      // AUTO mode is the only mode that should react to PIR motion.
      // Manual ON/OFF and timer-controlled relays keep their own behavior.
      if (relay.manualMode != RelayMode::AUTO || relay.timer.active) {
        continue;
      }
      relay.autoHoldUntilEpoch = max(relay.autoHoldUntilEpoch, nowEpoch + PIR_HOLD_SECONDS);
    }
    publishEventLocked("TIMER",
                       "pir.motion",
                       String("Motion detected on ") + PIR_CONFIG[i].name + ".",
                       static_cast<int>(i),
                       true);
  }
}

ControlEngine::Decision ControlEngine::evaluateRelayLocked(size_t relayIndex, uint64_t nowEpoch) {
  RelayRuntime &relay = runtime_->relays[relayIndex];
  const uint64_t timerEpoch = timeKeeper_->nowUserEpoch();
  const bool timerClockReady = timerEpoch >= 1700000000ULL;

  if (relay.timer.active && timerClockReady && timerEpoch >= relay.timer.endEpoch) {
    const RelayState restoreState = relay.timer.previousState;

    finalizeEnergyTrackingLocked(relayIndex, relay, timerEpoch, "timer.ended");
    relay.timer.active = false;
    relay.timer.startEpoch = 0;
    relay.timer.endEpoch = 0;
    relay.timer.targetState = RelayState::OFF;
    relay.timer.durationMinutes = 0;
    relay.timer.restorePending = false;

    // Timer lifecycle requirement: return to AUTO at timer end.
    relay.manualMode = RelayMode::AUTO;

    storage_->persistTimer(relayIndex, relay.timer);
    storage_->persistManualMode(relayIndex, relay.manualMode);

    publishEventLocked("TIMER",
                       "timer.ended",
                       "Timer ended, restoring previous state.",
                       static_cast<int>(relayIndex),
                       true,
                       timerEpoch);

    Decision ended{};
    ended.state = restoreState;
    ended.source = ControlSource::TIMER;
    return ended;
  }

  Decision out{};
  out.state = RelayState::OFF;
  out.source = ControlSource::NONE;

  // Compatibility path for legacy persisted restorePending state.
  if (relay.timer.restorePending) {
    out.state = relay.timer.previousState;
    out.source = ControlSource::TIMER;
    relay.timer.restorePending = false;
    return out;
  }

  // After reboot, preserve the last applied output until the client clock is synced again.
  // This avoids expiring or extending persisted timers against an unknown local time base.
  if (relay.timer.active && !timerClockReady) {
    out.state = relay.appliedState;
    out.source = relay.appliedSource;
    return out;
  }

  // Manual ON/OFF should continue working after the user disconnects.
  // The selected relay mode is now authoritative, not client connectivity.
  if (relay.manualMode == RelayMode::ON) {
    out.state = RelayState::ON;
    out.source = ControlSource::MANUAL;
  } else if (relay.manualMode == RelayMode::OFF) {
    out.state = RelayState::OFF;
    out.source = ControlSource::MANUAL;
  } else if (relay.timer.active) {
    out.state = relay.timer.targetState;
    out.source = ControlSource::TIMER;
  } else if (nowEpoch < relay.autoHoldUntilEpoch) {
    out.state = RelayState::ON;
    out.source = ControlSource::PIR;
  }

  // Day/Night safety may force OFF for conflicting ON decisions.
  if (out.state == RelayState::ON && !canTurnOnLocked()) {
    out.state = RelayState::OFF;
    if (out.source != ControlSource::MANUAL) {
      out.source = ControlSource::NONE;
    }
  }

  // Final safety net: Night Lock forcibly blocks every ON decision source.
  if (runtime_->nightLockActive && out.state == RelayState::ON) {
    out.state = RelayState::OFF;
    out.source = ControlSource::NONE;
  }
  return out;
}

void ControlEngine::applyDecisionsLocked(const std::vector<Decision> &decisions, uint64_t nowEpoch) {
  if (decisions.size() < RELAY_COUNT) {
    return;
  }
  for (size_t i = 0; i < RELAY_COUNT; ++i) {
    RelayRuntime &relay = runtime_->relays[i];
    const RelayState oldState = relay.appliedState;
    const ControlSource oldSource = relay.appliedSource;

    relay.appliedState = decisions[i].state;
    relay.appliedSource = decisions[i].source;

    if (oldState == relay.appliedState && oldSource == relay.appliedSource) {
      if (relay.appliedState == RelayState::ON && relay.stats.lastOnEpoch == 0) {
        relay.stats.lastOnEpoch = nowEpoch;
      }
      markEnergyTrackingStartLocked(i, relay, nowEpoch);
      continue;
    }

    if (oldState == RelayState::ON) {
      closeActiveOnWindowLocked(relay, nowEpoch);
    }
    if (relay.appliedState == RelayState::ON) {
      relay.stats.lastOnEpoch = nowEpoch;
    }

    digitalWrite(RELAY_CONFIG[i].relayPin, relayOutputLevel(relay.appliedState));
    storage_->persistRelayState(i, relay.appliedState, relay.appliedSource);
    storage_->persistRelayStats(i, relay.stats);

    markEnergyTrackingStartLocked(i, relay, nowEpoch);

    const uint64_t eventTs = (relay.appliedSource == ControlSource::TIMER) ? timeKeeper_->nowUserEpoch() : 0;
    publishEventLocked(relay.appliedState == RelayState::ON ? "ON" : "OFF",
                       "relay.changed",
                       String(RELAY_CONFIG[i].name) + " -> " + relayStateToText(relay.appliedState) +
                           " via " + sourceToText(relay.appliedSource) + ".",
                       static_cast<int>(i),
                       true,
                       eventTs);
  }
}

void ControlEngine::publishEventLocked(const String &logType,
                                       const String &eventName,
                                       const String &msg,
                                       int channel,
                                       bool bufferIfOffline,
                                       uint64_t forcedTs) const {
  if (!eventCallback_) {
    return;
  }
  JsonDocument doc;
  doc["type"] = logType;   // ON, OFF, TIMER, ERROR
  doc["event"] = eventName;
  doc["msg"] = msg;
  doc["ts"] = forcedTs > 0 ? forcedTs : nowEpochLocked();
  if (channel >= 0) {
    doc["channel"] = channel;
  }
  String line;
  serializeJson(doc, line);
  eventCallback_(line, bufferIfOffline);
}

void ControlEngine::applyNightLockTransitionLocked(bool activate, uint64_t nowEpoch) {
  if (runtime_->nightLockActive == activate) {
    return;
  }

  const uint64_t timerEpoch = timeKeeper_->nowUserEpoch();
  runtime_->nightLockActive = activate;
  if (!activate) {
    publishEventLocked("TIMER", "night_lock.released", "Night Lock Released", -1, true, timerEpoch);
    return;
  }

  publishEventLocked("TIMER", "night_lock.activated", "Night Lock Activated", -1, true, timerEpoch);

  // Night entry behavior: force all relays OFF and cancel all active timers.
  for (size_t i = 0; i < RELAY_COUNT; ++i) {
    RelayRuntime &relay = runtime_->relays[i];

    if (relay.timer.active || relay.timer.restorePending) {
      finalizeEnergyTrackingLocked(i, relay, timerEpoch > 0 ? timerEpoch : nowEpoch, "timer.canceled");
      relay.timer.active = false;
      relay.timer.startEpoch = 0;
      relay.timer.endEpoch = 0;
      relay.timer.targetState = RelayState::OFF;
      relay.timer.durationMinutes = 0;
      relay.timer.restorePending = false;
      storage_->persistTimer(i, relay.timer);
      publishEventLocked("TIMER", "timer.canceled", "Timer canceled by Night Lock", static_cast<int>(i), true, timerEpoch);
    }
    clearEnergyTrackingLocked(relay);

    if (relay.appliedState == RelayState::ON) {
      closeActiveOnWindowLocked(relay, nowEpoch);
    }
    relay.appliedState = RelayState::OFF;
    relay.appliedSource = ControlSource::NONE;
    digitalWrite(RELAY_CONFIG[i].relayPin, relayOutputLevel(RelayState::OFF));
    storage_->persistRelayState(i, relay.appliedState, relay.appliedSource);
    storage_->persistRelayStats(i, relay.stats);

    publishEventLocked("OFF",
                       "relay.night_forced_off",
                       String(RELAY_CONFIG[i].name) + " forced OFF by Night Lock.",
                       static_cast<int>(i),
                       true);
  }
}

void ControlEngine::markEnergyTrackingStartLocked(size_t relayIndex, RelayRuntime &relay, uint64_t nowEpoch) {
  if (!runtime_->energyTrackingEnabled) {
    return;
  }
  if (!relay.timer.active || relay.timer.targetState != RelayState::ON) {
    return;
  }
  if (relay.appliedState != RelayState::ON || relay.appliedSource != ControlSource::TIMER) {
    return;
  }
  if (relay.energyTrackingActive) {
    return;
  }
  if (relay.energyStartEpoch == 0) {
    relay.energyStartEpoch = nowEpoch;
  }
  relay.energyTrackingActive = true;
}

void ControlEngine::finalizeEnergyTrackingLocked(size_t relayIndex,
                                                 RelayRuntime &relay,
                                                 uint64_t nowEpoch,
                                                 const String &stopEvent) {
  if (!runtime_->energyTrackingEnabled) {
    clearEnergyTrackingLocked(relay);
    return;
  }
  if (!relay.energyTrackingActive || relay.energyStartEpoch == 0 || nowEpoch <= relay.energyStartEpoch) {
    clearEnergyTrackingLocked(relay);
    return;
  }

  // Lightweight energy math: Wh = Power(W) * duration(hours)
  const uint64_t energyEndEpoch = nowEpoch;
  const float durationHours = static_cast<float>(energyEndEpoch - relay.energyStartEpoch) / 3600.0f;
  // RATED DYNAMIC START
  const float lastWh = relay.ratedPowerWatts * durationHours;
  // RATED DYNAMIC END
  relay.stats.lastEnergyWh = lastWh;
  relay.stats.totalEnergyWh += lastWh;
  storage_->persistRelayEnergyStats(relayIndex, relay.stats.totalEnergyWh, relay.stats.lastEnergyWh);

  publishEnergyUpdateLocked(relayIndex, relay.stats.lastEnergyWh, relay.stats.totalEnergyWh);
  publishEventLocked("TIMER",
                     "energy.calculated",
                     String(RELAY_CONFIG[relayIndex].name) + " energy computed after " + stopEvent + ".",
                     static_cast<int>(relayIndex),
                     true);
  clearEnergyTrackingLocked(relay);
}

void ControlEngine::clearEnergyTrackingLocked(RelayRuntime &relay) {
  relay.energyTrackingActive = false;
  relay.energyStartEpoch = 0;
}

void ControlEngine::publishEnergyUpdateLocked(size_t relayIndex, float lastWh, float totalWh) const {
  if (!eventCallback_) {
    return;
  }
  JsonDocument doc;
  doc["type"] = "TIMER";
  doc["event"] = "energy_update";
  doc["msg"] = String("Energy updated for ") + RELAY_CONFIG[relayIndex].name + ".";
  const uint64_t energyTs = timeKeeper_->nowUserEpoch() > 0 ? timeKeeper_->nowUserEpoch() : nowEpochLocked();
  doc["ts"] = energyTs;
  doc["relay"] = relayIndex;
  doc["channel"] = relayIndex;
  doc["lastWh"] = lastWh;
  doc["totalWh"] = totalWh;
  String line;
  serializeJson(doc, line);
  eventCallback_(line, true);
}

uint64_t ControlEngine::effectiveOnSecondsLocked(const RelayRuntime &relay, uint64_t nowEpoch) const {
  uint64_t total = relay.stats.accumulatedOnSeconds;
  if (relay.appliedState == RelayState::ON && relay.stats.lastOnEpoch > 0 && nowEpoch > relay.stats.lastOnEpoch) {
    total += (nowEpoch - relay.stats.lastOnEpoch);
  }
  return total;
}

void ControlEngine::closeActiveOnWindowLocked(RelayRuntime &relay, uint64_t nowEpoch) {
  if (relay.stats.lastOnEpoch > 0 && nowEpoch > relay.stats.lastOnEpoch) {
    relay.stats.accumulatedOnSeconds += (nowEpoch - relay.stats.lastOnEpoch);
  }
  relay.stats.lastOnEpoch = 0;
}

bool ControlEngine::withLock(const std::function<void()> &fn) const {
  if (!fn || !stateMutex_) {
    return false;
  }
  if (xSemaphoreTake(stateMutex_, pdMS_TO_TICKS(80)) != pdTRUE) {
    return false;
  }
  fn();
  xSemaphoreGive(stateMutex_);
  return true;
}
