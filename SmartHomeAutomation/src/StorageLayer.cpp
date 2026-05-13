#include "StorageLayer.h"
// ACCESS CONTROL START
#include <mbedtls/sha256.h>
#include <cstring>
#include <ArduinoJson.h>
#include <vector>

#include "Config.h"

namespace
{
  String keyFor(const char *prefix, size_t index)
  {
    String key(prefix);
    key += String(index);
    return key;
  }

  // STORAGE START
  // Files created by the application at runtime. Factory reset removes these files
  // but intentionally leaves bundled UI assets (for example /index.html) intact.
  constexpr const char *kRuntimeFiles[] = {
      FILE_LOGS,
      FILE_PENDING,
  };

  bool ensureFileExists(const char *path)
  {
    if (!path || LittleFS.exists(path))
    {
      return path != nullptr;
    }

    File file = LittleFS.open(path, FILE_WRITE);
    if (!file)
    {
      return false;
    }
    file.close();
    return true;
  }
  // STORAGE END
} // namespace

bool StorageLayer::begin()
{
  // STORAGE START
  // Storage initialization order:
  // 1) Create a mutex so flash access stays serialized across tasks.
  // 2) Mount LittleFS for persistent file storage (logs / JSON files).
  // 3) Open the Preferences namespace for NVS key-value settings.
  // RAM state is not initialized here because RAM is volatile and is rebuilt at boot.
  // STORAGE END
  // StorageLayer.cpp – begin()
  if (!ioMutex_)
  {
    ioMutex_ = xSemaphoreCreateRecursiveMutex(); // was xSemaphoreCreateMutex()
  }
  if (!ioMutex_)
  {
    return false;
  }
  if (!LittleFS.begin(false))
  {
    return false;
  }
  if (!lock())
  {
    return false;
  }
  const bool ok = preferences_.begin(PREF_NAMESPACE, false);
  if (ok)
  {
    // STORAGE MANAGEMENT: restore log-enabled flag across reboots.
    logsEnabled_ = preferences_.getBool(LOG_ENABLED_KEY, true);
  }
  unlock();
  if (ok)
  {
    // Keep runtime files present after cold boot, factory reset, or a blank
    // filesystem so later read/append paths do not fail unexpectedly.
    for (const char *path : kRuntimeFiles)
    {
      ensureFileExists(path);
    }
    ensureFileExists(FILE_CLOUD_QUEUE);
  }
  return ok;
}

Preferences *StorageLayer::prefs() { return &preferences_; }

void StorageLayer::loadRuntime(SystemRuntime *runtime)
{
  if (!runtime)
  {
    return;
  }
  if (!lock())
  {
    return;
  }
  // Restore user-changeable settings from NVS at boot:
  // - global toggles: energy tracking
  // - per-relay settings: manual mode, last applied state/source, active timer plan
  // - per-relay power configuration: rated wattage + lock flag
  // - per-sensor settings: PIR-to-relay mapping
  // After a page reload, the frontend reads the same live values back through
  // /api/state and WebSocket state_snapshot so the UI matches the saved system state.
  runtime->energyTrackingEnabled = preferences_.getBool("energy_en", false);
  runtime->relays.resize(RELAY_COUNT);
  runtime->pirs.resize(PIR_COUNT);
  runtime->pirMap.resize(PIR_COUNT);
  // PIR MAPPING START
  for (size_t i = 0; i < PIR_COUNT; ++i)
  {
    const uint64_t defaultMask = PIR_CONFIG[i].relayMask & relayMaskForCount(RELAY_COUNT);
    const String key64 = keyFor("pm64", i);
    const String legacyKey = keyFor("pm", i);
    uint64_t storedMask = defaultMask;
    if (preferences_.isKey(key64.c_str()))
    {
      storedMask = preferences_.getULong64(key64.c_str(), defaultMask);
    }
    else if (preferences_.isKey(legacyKey.c_str()))
    {
      storedMask = preferences_.getUChar(legacyKey.c_str(), static_cast<uint8_t>(defaultMask & 0xFFU));
    }
    runtime->pirMap[i].relayMask = storedMask & relayMaskForCount(RELAY_COUNT);
  }
  // PIR MAPPING END
  for (size_t i = 0; i < RELAY_COUNT; ++i)
  {
    // RATED DYNAMIC START
    // Load one-time rated power from NVS when present; otherwise keep compile-time defaults unlocked.
    const float storedRatedPower = preferences_.getFloat(keyFor("rp", i).c_str(), RELAY_CONFIG[i].ratedPowerWatts);
    runtime->relays[i].ratedPowerWatts = storedRatedPower > 0.0f ? storedRatedPower : RELAY_CONFIG[i].ratedPowerWatts;
    runtime->relays[i].ratedPowerLocked = preferences_.getBool(keyFor("rpl", i).c_str(), false);
    // RATED DYNAMIC END
    uint8_t mode = preferences_.getUChar(keyFor("m", i).c_str(), static_cast<uint8_t>(RelayMode::AUTO));
    uint8_t state = preferences_.getUChar(keyFor("rs", i).c_str(), static_cast<uint8_t>(RelayState::OFF));
    uint8_t source = preferences_.getUChar(keyFor("src", i).c_str(), static_cast<uint8_t>(ControlSource::NONE));
    if (mode > static_cast<uint8_t>(RelayMode::AUTO))
    {
      mode = static_cast<uint8_t>(RelayMode::AUTO);
    }
    if (state > static_cast<uint8_t>(RelayState::ON))
    {
      state = static_cast<uint8_t>(RelayState::OFF);
    }
    if (source > static_cast<uint8_t>(ControlSource::MANUAL))
    {
      source = static_cast<uint8_t>(ControlSource::NONE);
    }
    runtime->relays[i].manualMode = static_cast<RelayMode>(mode);
    runtime->relays[i].appliedState = static_cast<RelayState>(state);
    runtime->relays[i].appliedSource = static_cast<ControlSource>(source);
    runtime->relays[i].timer.active = preferences_.getBool(keyFor("ta", i).c_str(), false);
    runtime->relays[i].timer.startEpoch = preferences_.getULong64(keyFor("tst", i).c_str(), 0);
    runtime->relays[i].timer.endEpoch = preferences_.getULong64(keyFor("te", i).c_str(), 0);
    uint8_t target = preferences_.getUChar(keyFor("tt", i).c_str(), static_cast<uint8_t>(RelayState::OFF));
    if (target > static_cast<uint8_t>(RelayState::ON))
    {
      target = static_cast<uint8_t>(RelayState::OFF);
    }
    runtime->relays[i].timer.targetState = static_cast<RelayState>(target);
    uint8_t previousState =
        preferences_.getUChar(keyFor("tps", i).c_str(), static_cast<uint8_t>(RelayState::OFF));
    if (previousState > static_cast<uint8_t>(RelayState::ON))
    {
      previousState = static_cast<uint8_t>(RelayState::OFF);
    }
    runtime->relays[i].timer.previousState = static_cast<RelayState>(previousState);
    uint8_t previousManual =
        preferences_.getUChar(keyFor("tpm", i).c_str(), static_cast<uint8_t>(RelayMode::AUTO));
    if (previousManual > static_cast<uint8_t>(RelayMode::AUTO))
    {
      previousManual = static_cast<uint8_t>(RelayMode::AUTO);
    }
    runtime->relays[i].timer.previousManualMode = static_cast<RelayMode>(previousManual);
    runtime->relays[i].timer.durationMinutes = preferences_.getUInt(keyFor("tdm", i).c_str(), 0);
    runtime->relays[i].timer.restorePending = false; // keep restore flag runtime-only to avoid stale reboot loops

    // Reconstruct missing startEpoch for backward compatibility:
    // old firmware stored end+duration but not start.
    if (runtime->relays[i].timer.startEpoch == 0 && runtime->relays[i].timer.endEpoch > 0 &&
        runtime->relays[i].timer.durationMinutes > 0)
    {
      const uint64_t durationSeconds = static_cast<uint64_t>(runtime->relays[i].timer.durationMinutes) * 60ULL;
      if (runtime->relays[i].timer.endEpoch > durationSeconds)
      {
        runtime->relays[i].timer.startEpoch = runtime->relays[i].timer.endEpoch - durationSeconds;
      }
    }

    // Drop malformed persisted timer plans so runtime never computes negative/invalid remaining durations.
    const bool timerShapeInvalid = (runtime->relays[i].timer.durationMinutes == 0) ||
                                   (runtime->relays[i].timer.startEpoch == 0) ||
                                   (runtime->relays[i].timer.endEpoch == 0) ||
                                   (runtime->relays[i].timer.endEpoch <= runtime->relays[i].timer.startEpoch);
    if (runtime->relays[i].timer.active && timerShapeInvalid)
    {
      runtime->relays[i].timer.active = false;
      runtime->relays[i].timer.startEpoch = 0;
      runtime->relays[i].timer.endEpoch = 0;
      runtime->relays[i].timer.durationMinutes = 0;
      runtime->relays[i].timer.targetState = RelayState::OFF;
    }
    else if (!runtime->relays[i].timer.active)
    {
      runtime->relays[i].timer.startEpoch = 0;
      runtime->relays[i].timer.endEpoch = 0;
      runtime->relays[i].timer.durationMinutes = 0;
    }
    runtime->relays[i].autoHoldUntilEpoch = preferences_.getULong64(keyFor("ah", i).c_str(), 0);
    runtime->relays[i].energyTrackingActive = false;
    runtime->relays[i].energyStartEpoch = 0;
    runtime->relays[i].stats.timerUses = preferences_.getUInt(keyFor("tu", i).c_str(), 0);
    runtime->relays[i].stats.totalTimerMinutes = preferences_.getUInt(keyFor("tm", i).c_str(), 0);
    runtime->relays[i].stats.accumulatedOnSeconds = preferences_.getULong64(keyFor("os", i).c_str(), 0);
    runtime->relays[i].stats.lastOnEpoch = preferences_.getULong64(keyFor("lo", i).c_str(), 0);
    runtime->relays[i].stats.totalEnergyWh = preferences_.getFloat(keyFor("ewt", i).c_str(), 0.0f);
    runtime->relays[i].stats.lastEnergyWh = preferences_.getFloat(keyFor("ewl", i).c_str(), 0.0f);
  }
  unlock();
}

void StorageLayer::persistManualMode(size_t relayIndex, RelayMode mode)
{
  if (!lock())
  {
    return;
  }
  preferences_.putUChar(keyFor("m", relayIndex).c_str(), static_cast<uint8_t>(mode));
  unlock();
}

void StorageLayer::persistRelayState(size_t relayIndex, RelayState state, ControlSource source)
{
  if (!lock())
  {
    return;
  }
  preferences_.putUChar(keyFor("rs", relayIndex).c_str(), static_cast<uint8_t>(state));
  preferences_.putUChar(keyFor("src", relayIndex).c_str(), static_cast<uint8_t>(source));
  unlock();
}

void StorageLayer::persistTimer(size_t relayIndex, const TimerPlan &plan)
{
  if (!lock())
  {
    return;
  }
  preferences_.putBool(keyFor("ta", relayIndex).c_str(), plan.active);
  preferences_.putULong64(keyFor("tst", relayIndex).c_str(), plan.startEpoch);
  preferences_.putULong64(keyFor("te", relayIndex).c_str(), plan.endEpoch);
  preferences_.putUChar(keyFor("tt", relayIndex).c_str(), static_cast<uint8_t>(plan.targetState));
  preferences_.putUChar(keyFor("tps", relayIndex).c_str(), static_cast<uint8_t>(plan.previousState));
  preferences_.putUChar(keyFor("tpm", relayIndex).c_str(), static_cast<uint8_t>(plan.previousManualMode));
  preferences_.putUInt(keyFor("tdm", relayIndex).c_str(), plan.durationMinutes);
  preferences_.putBool(keyFor("trp", relayIndex).c_str(), plan.restorePending);
  unlock();
}

void StorageLayer::persistRelayStats(size_t relayIndex, const RelayStats &stats)
{
  if (!lock())
  {
    return;
  }
  preferences_.putUInt(keyFor("tu", relayIndex).c_str(), stats.timerUses);
  preferences_.putUInt(keyFor("tm", relayIndex).c_str(), stats.totalTimerMinutes);
  preferences_.putULong64(keyFor("os", relayIndex).c_str(), stats.accumulatedOnSeconds);
  preferences_.putULong64(keyFor("lo", relayIndex).c_str(), stats.lastOnEpoch);
  unlock();
}

void StorageLayer::persistRelayEnergyStats(size_t relayIndex, float totalEnergyWh, float lastEnergyWh)
{
  if (!lock())
  {
    return;
  }
  preferences_.putFloat(keyFor("ewt", relayIndex).c_str(), totalEnergyWh);
  preferences_.putFloat(keyFor("ewl", relayIndex).c_str(), lastEnergyWh);
  unlock();
}

void StorageLayer::persistRatedPower(size_t relayIndex, float watts, bool locked)
{
  if (!lock())
  {
    return;
  }
  preferences_.putFloat(keyFor("rp", relayIndex).c_str(), watts);
  preferences_.putBool(keyFor("rpl", relayIndex).c_str(), locked);
  unlock();
}

// PIR MAPPING START
void StorageLayer::persistPirMapping(size_t pirIndex, const PIRMapping &mapping)
{
  if (!lock())
  {
    return;
  }
  preferences_.putULong64(keyFor("pm64", pirIndex).c_str(), mapping.relayMask & relayMaskForCount(RELAY_COUNT));
  unlock();
}
// PIR MAPPING END

// STORAGE START
bool StorageLayer::saveBoolSetting(const char *key, bool value)
{
  if (!key || !lock())
  {
    return false;
  }
  const size_t written = preferences_.putBool(key, value);
  unlock();
  return written == sizeof(uint8_t);
}

bool StorageLayer::saveIntSetting(const char *key, int32_t value)
{
  if (!key || !lock())
  {
    return false;
  }
  const size_t written = preferences_.putInt(key, value);
  unlock();
  return written == sizeof(int32_t);
}

bool StorageLayer::saveStringSetting(const char *key, const String &value)
{
  if (!key || !lock())
  {
    return false;
  }
  const size_t written = preferences_.putString(key, value);
  unlock();
  return written == value.length();
}

bool StorageLayer::readBoolSetting(const char *key, bool defaultValue, bool *outValue)
{
  if (!key || !outValue || !lock())
  {
    return false;
  }
  *outValue = preferences_.getBool(key, defaultValue);
  unlock();
  return true;
}

bool StorageLayer::readIntSetting(const char *key, int32_t defaultValue, int32_t *outValue)
{
  if (!key || !outValue || !lock())
  {
    return false;
  }
  *outValue = preferences_.getInt(key, defaultValue);
  unlock();
  return true;
}

bool StorageLayer::readStringSetting(const char *key, const String &defaultValue, String *outValue)
{
  if (!key || !outValue || !lock())
  {
    return false;
  }
  // Missing string keys should quietly fall back to defaults instead of
  // producing noisy NOT_FOUND reads from Preferences/NVS.
  *outValue = preferences_.isKey(key) ? preferences_.getString(key, defaultValue) : defaultValue;
  unlock();
  return true;
}

bool StorageLayer::writeJsonFile(const char *path, const JsonDocument &doc)
{
  if (!path || !lock())
  {
    return false;
  }

  File file = LittleFS.open(path, FILE_WRITE);
  if (!file)
  {
    unlock();
    return false;
  }

  const size_t written = serializeJson(doc, file);
  file.close();
  unlock();
  return written > 0;
}

bool StorageLayer::readJsonFile(const char *path, JsonDocument *doc) const
{
  if (!path || !doc || !lock())
  {
    return false;
  }

  File file = LittleFS.open(path, FILE_READ);
  if (!file)
  {
    unlock();
    return false;
  }

  DeserializationError err = deserializeJson(*doc, file);
  file.close();
  unlock();
  return !err;
}

bool StorageLayer::deleteSettingKey(const char *key)
{
  if (!key || !lock())
  {
    return false;
  }
  const bool removed = preferences_.remove(key);
  unlock();
  return removed;
}

bool StorageLayer::deleteFile(const char *path) const
{
  if (!path || !lock())
  {
    return false;
  }
  const bool deleted = !LittleFS.exists(path) || LittleFS.remove(path);
  unlock();
  return deleted;
}

bool StorageLayer::clearLogFiles()
{
  bool allDeleted = true;
  for (const char *path : kRuntimeFiles)
  {
    if (!deleteFile(path))
    {
      allDeleted = false;
    }
  }
  return allDeleted;
}

bool StorageLayer::factoryReset()
{
  if (!lock())
  {
    return false;
  }

  // Clear only this application's NVS namespace. This preserves unrelated namespaces.
  const bool prefsCleared = preferences_.clear();
  unlock();
  if (!prefsCleared)
  {
    return false;
  }

  // Remove runtime-generated files only. Do not erase /index.html or other uploaded UI assets.
  const bool logsCleared = clearLogFiles();
  const bool cloudQueueCleared = deleteFile(FILE_CLOUD_QUEUE);
  return logsCleared && cloudQueueCleared;
}
// STORAGE END

void StorageLayer::persistEnergyTrackingEnabled(bool enabled)
{
  if (!lock())
  {
    return;
  }
  preferences_.putBool("energy_en", enabled);
  unlock();
}

void StorageLayer::persistLastCleanupDay(uint32_t dayToken)
{
  if (!lock())
  {
    return;
  }
  preferences_.putUInt("cleanup_day", dayToken);
  unlock();
}

uint32_t StorageLayer::loadLastCleanupDay()
{
  if (!lock())
  {
    return 0;
  }
  const uint32_t day = preferences_.getUInt("cleanup_day", 0);
  unlock();
  return day;
}

void StorageLayer::appendEvent(uint64_t epoch, const String &type, const String &message, int channel)
{
  if (!lock())
  {
    return;
  }
  JsonDocument doc;
  doc["ts"] = epoch;
  doc["type"] = type;
  doc["msg"] = message;
  if (channel >= 0)
  {
    doc["channel"] = channel;
  }
  String line;
  serializeJson(doc, line);
  appendLine(FILE_LOGS, line);
  trimFileBySize(FILE_LOGS, LOG_MAX_BYTES);
  unlock();
}

void StorageLayer::appendPending(const String &jsonLine)
{
  if (!lock())
  {
    return;
  }
  appendLine(FILE_PENDING, jsonLine);
  trimFileBySize(FILE_PENDING, PENDING_MAX_BYTES);
  unlock();
}

// CLOUD SYNC START
void StorageLayer::appendCloudQueue(const String &jsonLine)
{
  if (!lock())
  {
    return;
  }
  appendLine(FILE_CLOUD_QUEUE, jsonLine);
  trimFileBySize(FILE_CLOUD_QUEUE, CLOUD_QUEUE_MAX_BYTES);
  unlock();
}

bool StorageLayer::readCloudQueueHead(String *lineOut) const
{
  if (!lineOut || !lock())
  {
    return false;
  }
  if (!LittleFS.exists(FILE_CLOUD_QUEUE))
  {
    ensureFileExists(FILE_CLOUD_QUEUE);
    unlock();
    return false;
  }
  File input = LittleFS.open(FILE_CLOUD_QUEUE, FILE_READ);
  if (!input)
  {
    unlock();
    return false;
  }
  bool found = false;
  while (input.available())
  {
    String line = input.readStringUntil('\n');
    line.trim();
    if (!line.isEmpty())
    {
      *lineOut = line;
      found = true;
      break;
    }
  }
  input.close();
  unlock();
  return found;
}

void StorageLayer::dropCloudQueueHead()
{
  if (!lock())
  {
    return;
  }
  if (!LittleFS.exists(FILE_CLOUD_QUEUE))
  {
    ensureFileExists(FILE_CLOUD_QUEUE);
    unlock();
    return;
  }
  File input = LittleFS.open(FILE_CLOUD_QUEUE, FILE_READ);
  if (!input)
  {
    unlock();
    return;
  }
  File output = LittleFS.open("/cloud_queue.tmp", FILE_WRITE);
  if (!output)
  {
    input.close();
    unlock();
    return;
  }

  bool dropped = false;
  while (input.available())
  {
    String line = input.readStringUntil('\n');
    line.trim();
    if (line.isEmpty())
    {
      continue;
    }
    if (!dropped)
    {
      dropped = true;
      continue;
    }
    output.println(line);
  }
  input.close();
  output.close();
  LittleFS.remove(FILE_CLOUD_QUEUE);
  LittleFS.rename("/cloud_queue.tmp", FILE_CLOUD_QUEUE);
  unlock();
}
// CLOUD SYNC END

void StorageLayer::appendEventJson(const String &jsonLine)
{
  // STORAGE MANAGEMENT: when logging is disabled, events are still broadcast
  // over WebSocket but are never written to flash (saves LittleFS space).
  if (!logsEnabled_) return;
  if (!lock())
  {
    return;
  }
  appendLine(FILE_LOGS, jsonLine);
  trimFileBySize(FILE_LOGS, LOG_MAX_BYTES);
  unlock();
}

String StorageLayer::readRecentLogsJson(uint16_t limit) const
{
  if (!lock())
  {
    return "[]";
  }
  if (limit == 0)
  {
    limit = 1;
  }
  if (limit > LOG_FETCH_MAX_ITEMS)
  {
    limit = LOG_FETCH_MAX_ITEMS;
  }
  if (!LittleFS.exists(FILE_LOGS))
  {
    ensureFileExists(FILE_LOGS);
    unlock();
    return "[]";
  }
  File file = LittleFS.open(FILE_LOGS, FILE_READ);
  if (!file)
  {
    unlock();
    return "[]";
  }

  std::vector<String> ring;
  ring.reserve(limit);
  while (file.available())
  {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.isEmpty())
    {
      continue;
    }
    ring.push_back(line);
    if (ring.size() > limit)
    {
      ring.erase(ring.begin());
    }
  }
  file.close();

  String json = "[";
  for (size_t i = 0; i < ring.size(); ++i)
  {
    if (i > 0)
    {
      json += ",";
    }
    json += ring[i];
  }
  json += "]";
  unlock();
  return json;
}

void StorageLayer::flushPending(const std::function<void(const String &line)> &sender)
{
  if (!lock())
  {
    return;
  }
  if (!LittleFS.exists(FILE_PENDING))
  {
    ensureFileExists(FILE_PENDING);
    unlock();
    return;
  }
  File input = LittleFS.open(FILE_PENDING, FILE_READ);
  if (!input)
  {
    unlock();
    return;
  }
  while (input.available())
  {
    String line = input.readStringUntil('\n');
    line.trim();
    if (line.isEmpty())
    {
      continue;
    }
    sender(line);
  }
  input.close();
  LittleFS.remove(FILE_PENDING);
  unlock();
}

void StorageLayer::cleanupDaily(uint64_t nowEpoch)
{
  if (!lock())
  {
    return;
  }
  const uint64_t retentionSeconds = static_cast<uint64_t>(LOG_RETENTION_DAYS) * 86400ULL;
  uint64_t minEpoch = 0;
  if (nowEpoch > retentionSeconds)
  {
    minEpoch = nowEpoch - retentionSeconds;
  }
  compactByAge(FILE_LOGS, minEpoch);
  compactByAge(FILE_PENDING, minEpoch);
  compactByAge(FILE_CLOUD_QUEUE, minEpoch);
  trimFileBySize(FILE_LOGS, LOG_MAX_BYTES);
  trimFileBySize(FILE_PENDING, PENDING_MAX_BYTES);
  trimFileBySize(FILE_CLOUD_QUEUE, CLOUD_QUEUE_MAX_BYTES);
  unlock();
}

void StorageLayer::appendLine(const char *path, const String &line) const
{
  if (!ensureFileExists(path))
  {
    return;
  }
  File file = LittleFS.open(path, FILE_APPEND);
  if (!file)
  {
    return;
  }
  file.println(line);
  file.close();
}

void StorageLayer::trimFileBySize(const char *path, uint32_t maxBytes) const
{
  if (!LittleFS.exists(path))
  {
    ensureFileExists(path);
    return;
  }
  File file = LittleFS.open(path, FILE_READ);
  if (!file)
  {
    return;
  }
  const size_t currentSize = file.size();
  if (currentSize <= maxBytes)
  {
    file.close();
    return;
  }

  std::vector<String> lines;
  lines.reserve(80);
  const uint32_t target = static_cast<uint32_t>(maxBytes * 0.7f);
  while (file.available())
  {
    String line = file.readStringUntil('\n');
    line.trim();
    if (!line.isEmpty())
    {
      lines.push_back(line);
    }
  }
  file.close();

  size_t bytes = 0;
  size_t start = lines.size();
  while (start > 0)
  {
    const size_t lineBytes = lines[start - 1].length() + 1;
    if (bytes + lineBytes > target && start < lines.size())
    {
      break;
    }
    bytes += lineBytes;
    --start;
  }

  File output = LittleFS.open(path, FILE_WRITE);
  if (!output)
  {
    return;
  }
  for (size_t i = start; i < lines.size(); ++i)
  {
    output.println(lines[i]);
  }
  output.close();
}

bool StorageLayer::parseEpochFromLine(const String &line, uint64_t *epochOut) const
{
  if (!epochOut || line.isEmpty())
  {
    return false;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err)
  {
    return false;
  }
  if (!doc["ts"].is<uint64_t>())
  {
    return false;
  }
  *epochOut = doc["ts"].as<uint64_t>();
  return true;
}

void StorageLayer::compactByAge(const char *path, uint64_t minEpochToKeep) const
{
  if (!LittleFS.exists(path))
  {
    ensureFileExists(path);
    return;
  }
  File input = LittleFS.open(path, FILE_READ);
  if (!input)
  {
    return;
  }

  std::vector<String> keptLines;
  keptLines.reserve(120);
  while (input.available())
  {
    String line = input.readStringUntil('\n');
    line.trim();
    if (line.isEmpty())
    {
      continue;
    }
    uint64_t epoch = 0;
    if (!parseEpochFromLine(line, &epoch) || epoch >= minEpochToKeep)
    {
      keptLines.push_back(line);
    }
  }
  input.close();

  File output = LittleFS.open(path, FILE_WRITE);
  if (!output)
  {
    return;
  }
  for (const String &line : keptLines)
  {
    output.println(line);
  }
  output.close();
}

bool StorageLayer::lock() const {
    if (!ioMutex_) return false;
    return xSemaphoreTakeRecursive(ioMutex_, pdMS_TO_TICKS(120)) == pdTRUE;
}

void StorageLayer::unlock() const {
    if (ioMutex_) {
        xSemaphoreGiveRecursive(ioMutex_);
    }
}
// ACCESS CONTROL START
namespace
{
  constexpr char ACCESS_USERS_KEY[] = "users_json";

  String hashPassword(const String &password)
  {
    uint8_t hash[32];
    mbedtls_sha256_ret(
        reinterpret_cast<const unsigned char *>(password.c_str()),
        password.length(),
        hash,
        0);

    char hexString[65];
    for (size_t i = 0; i < 32; i++)
    {
      snprintf(hexString + (i * 2), 3, "%02x", hash[i]);
    }
    hexString[64] = '\0';
    return String(hexString);
  }

  bool isValidMacFormat(const String &mac)
  {
    if (mac.length() != 17)
      return false;
    for (size_t i = 0; i < 17; i++)
    {
      if (i % 3 == 2)
      {
        if (mac[i] != ':')
          return false;
      }
      else
      {
        char c = mac[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
        {
          return false;
        }
      }
    }
    return true;
  }

  String normalizeMac(String mac)
  {
    mac.toUpperCase();
    return mac;
  }
} // namespace

bool StorageLayer::validateMacFormat(const char *macAddress)
{
  if (!macAddress)
    return false;
  return isValidMacFormat(String(macAddress));
}

bool StorageLayer::loadUserAccounts(AccessControlRuntime *access)
{
  if (!access || !lock())
    return false;

  memset(access, 0, sizeof(AccessControlRuntime));
  access->enabled = ENABLE_ACCESS_CONTROL;

  String jsonStr = preferences_.getString(ACCESS_USERS_KEY, "");
  unlock();

  // FIX BUG-AUTH: single-pass load. If NVS is empty, parse fails, or any
  // stored hash is not exactly 64 hex chars (legacy/truncated record from
  // pre-fix firmware that could never authenticate), fall through to the
  // default-admin re-provisioning block below. No second JSON parse, no
  // extra buffers.
  bool needReprovision = jsonStr.isEmpty();
  uint8_t count = 0;

  if (!needReprovision)
  {
    JsonDocument doc;
    if (deserializeJson(doc, jsonStr))
    {
      needReprovision = true;
    }
    else
    {
      JsonArrayConst usersArray = doc["users"].as<JsonArrayConst>();
      for (JsonVariantConst userVar : usersArray)
      {
        if (count >= MAX_USER_ACCOUNTS)
          break;

        JsonObjectConst userObj = userVar.as<JsonObjectConst>();
        const char *mac = userObj["mac"] | "";
        const char *name = userObj["name"] | "";
        const char *pass = userObj["passwordHash"] | "";

        if (!isValidMacFormat(String(mac)))
          continue;

        // FIX BUG-AUTH: pre-fix firmware stored 63-char truncated hashes
        // that can never authenticate. Any non-64-char hash means the
        // whole roster is from a broken build; reset and fall through.
        if (strlen(pass) != 64)
        {
          Serial.printf("[Storage] Legacy password hash detected "
                        "(len=%u). Resetting to default admin.\n",
                        static_cast<unsigned>(strlen(pass)));
          needReprovision = true;
          count = 0;
          break;
        }

        UserAccount &user = access->users[count];
        strncpy(user.macAddress, normalizeMac(String(mac)).c_str(), MAX_MAC_LENGTH - 1);
        user.macAddress[MAX_MAC_LENGTH - 1] = '\0';
        strncpy(user.displayName, name, MAX_NAME_LENGTH - 1);
        user.displayName[MAX_NAME_LENGTH - 1] = '\0';
        strncpy(user.passwordHash, pass, MAX_PASSWORD_LENGTH - 1);
        user.passwordHash[MAX_PASSWORD_LENGTH - 1] = '\0';
        user.isAdmin = userObj["isAdmin"] | false;
        user.canManageUsers = userObj["canManageUsers"] | false;
        // RESTRICTED MODE: defaults to false so existing accounts without this
        // field are treated as normal (unrestricted) users on firmware upgrade.
        user.restricted = userObj["restricted"] | false;
        user.createdAt = userObj["createdAt"] | 0;
        user.lastAccess = userObj["lastAccess"] | 0;

        count++;
      }
    }
  }

  if (needReprovision)
  {
    memset(access, 0, sizeof(AccessControlRuntime));
    access->enabled = ENABLE_ACCESS_CONTROL;

    UserAccount defaultAdmin;
    memset(&defaultAdmin, 0, sizeof(defaultAdmin));
    strncpy(defaultAdmin.macAddress, "E6:F8:71:C0:7A:07", MAX_MAC_LENGTH - 1);
    defaultAdmin.macAddress[MAX_MAC_LENGTH - 1] = '\0';
    strncpy(defaultAdmin.displayName, "Eng Body", MAX_NAME_LENGTH - 1);
    defaultAdmin.displayName[MAX_NAME_LENGTH - 1] = '\0';
    String hashed = hashPassword("12345678");
    // FIX BUG-AUTH: copy the full 64 hex chars (MAX_PASSWORD_LENGTH-1==64)
    // and force a null terminator at the last buffer byte.
    strncpy(defaultAdmin.passwordHash, hashed.c_str(), MAX_PASSWORD_LENGTH - 1);
    defaultAdmin.passwordHash[MAX_PASSWORD_LENGTH - 1] = '\0';
    defaultAdmin.isAdmin = true;
    defaultAdmin.canManageUsers = true;
    defaultAdmin.createdAt = time(nullptr);

    access->users[0] = defaultAdmin;
    access->userCount = 1;
    saveUserAccounts(access);
    return true;
  }

  access->userCount = count;
  return true;
}

bool StorageLayer::saveUserAccounts(const AccessControlRuntime *access)
{
  if (!access || !lock())
    return false;

  JsonDocument doc;
  JsonArray usersArray = doc["users"].to<JsonArray>();

  for (uint8_t i = 0; i < access->userCount; i++)
  {
    const UserAccount &user = access->users[i];
    JsonObject userObj = usersArray.add<JsonObject>();
    userObj["mac"] = user.macAddress;
    userObj["name"] = user.displayName;
    userObj["passwordHash"] = user.passwordHash;
    userObj["isAdmin"] = user.isAdmin;
    userObj["canManageUsers"] = user.canManageUsers;
    userObj["restricted"] = user.restricted;
    userObj["createdAt"] = user.createdAt;
    userObj["lastAccess"] = user.lastAccess;
  }

  String jsonStr;
  serializeJson(doc, jsonStr);

  bool saved = preferences_.putString(ACCESS_USERS_KEY, jsonStr) == jsonStr.length();
  unlock();
  return saved;
}

bool StorageLayer::addUserAccount(const UserAccount &user)
{
  // FIX R1: every failure path now logs the exact reason to Serial so the
  // firmware-side diagnostic is no longer a single anonymous "false".
  // FIX R2: normalise the incoming MAC to upper case BEFORE the duplicate
  // check and BEFORE persisting, so any caller that forgets to upper-case
  // (the storage API never promised the caller would) cannot bypass the
  // duplicate guard or store a mixed-case MAC alongside its upper-case twin.
  // No signature change, no struct change, no semantic change for callers
  // that already pass an upper-case validated MAC.
  if (!validateMacFormat(user.macAddress))
  {
    Serial.printf("[Storage] addUserAccount rejected: invalid MAC format \"%s\"\n",
                  user.macAddress ? user.macAddress : "(null)");
    return false;
  }
  if (!lock())
  {
    Serial.printf("[Storage] addUserAccount rejected: could not acquire io mutex (mac=%s)\n",
                  user.macAddress);
    return false;
  }

  // FIX R2: build a normalised copy of the caller's record. We never mutate
  // the caller's `user` (it's const). We only normalise the MAC field; all
  // other fields are copied verbatim.
  UserAccount candidate = user;
  {
    String normalized(candidate.macAddress);
    normalized.toUpperCase();
    // strncpy + explicit null terminator: macAddress is MAX_MAC_LENGTH(18)
    // bytes; a valid MAC is 17 chars, so byte 17 must be '\0'.
    strncpy(candidate.macAddress, normalized.c_str(), MAX_MAC_LENGTH - 1);
    candidate.macAddress[MAX_MAC_LENGTH - 1] = '\0';
  }

  AccessControlRuntime access;
  if (!loadUserAccounts(&access))
  {
    Serial.printf("[Storage] addUserAccount failed: loadUserAccounts() returned false (mac=%s)\n",
                  candidate.macAddress);
    unlock();
    return false;
  }

  for (uint8_t i = 0; i < access.userCount; i++)
  {
    if (strcmp(access.users[i].macAddress, candidate.macAddress) == 0)
    {
      Serial.printf("[Storage] addUserAccount rejected: duplicate MAC %s already at slot %u\n",
                    candidate.macAddress, static_cast<unsigned>(i));
      unlock();
      return false;
    }
  }

  if (access.userCount >= MAX_USER_ACCOUNTS)
  {
    Serial.printf("[Storage] addUserAccount rejected: roster full (%u/%u, mac=%s)\n",
                  static_cast<unsigned>(access.userCount),
                  static_cast<unsigned>(MAX_USER_ACCOUNTS),
                  candidate.macAddress);
    unlock();
    return false;
  }

  // FIX R3: zero `lastAccess` defensively. Callers (e.g. WebPortal) build
  // a `UserAccount` on the stack and may forget to initialise this field;
  // without this line, uninitialised stack bytes are persisted to NVS
  // inside saveUserAccounts() -> JSON. Other fields stay as the caller set
  // them so this does not change the semantics for well-behaved callers.
  candidate.lastAccess = 0;

  access.users[access.userCount] = candidate;
  access.userCount++;

  bool saved = saveUserAccounts(&access);
  unlock();
  if (!saved)
  {
    Serial.printf("[Storage] addUserAccount failed: saveUserAccounts() returned false (mac=%s)\n",
                  candidate.macAddress);
  }
  else
  {
    Serial.printf("[Storage] addUserAccount ok: mac=%s name=%s admin=%d manage=%d restricted=%d (now %u/%u users)\n",
                  candidate.macAddress, candidate.displayName,
                  candidate.isAdmin ? 1 : 0,
                  candidate.canManageUsers ? 1 : 0,
                  candidate.restricted ? 1 : 0,
                  static_cast<unsigned>(access.userCount),
                  static_cast<unsigned>(MAX_USER_ACCOUNTS));
  }
  return saved;
}

bool StorageLayer::removeUserAccount(const char *macAddress)
{
  if (!validateMacFormat(macAddress))
    return false;
  if (!lock())
    return false;

  AccessControlRuntime access;
  if (!loadUserAccounts(&access))
  {
    unlock();
    return false;
  }

  uint8_t adminCount = 0;
  int8_t removeIndex = -1;

  for (uint8_t i = 0; i < access.userCount; i++)
  {
    if (access.users[i].isAdmin)
      adminCount++;
    if (strcmp(access.users[i].macAddress, macAddress) == 0)
    {
      removeIndex = i;
    }
  }

  if (removeIndex < 0)
  {
    unlock();
    return false;
  }

  if (access.users[removeIndex].isAdmin && adminCount <= 1)
  {
    unlock();
    return false;
  }

  for (int8_t i = removeIndex; i < access.userCount - 1; i++)
  {
    access.users[i] = access.users[i + 1];
  }
  access.userCount--;

  bool saved = saveUserAccounts(&access);
  unlock();
  return saved;
}

bool StorageLayer::updateUserAccount(const UserAccount &user)
{
  if (!validateMacFormat(user.macAddress))
    return false;
  if (!lock())
    return false;

  AccessControlRuntime access;
  if (!loadUserAccounts(&access))
  {
    unlock();
    return false;
  }

  for (uint8_t i = 0; i < access.userCount; i++)
  {
    if (strcmp(access.users[i].macAddress, user.macAddress) == 0)
    {
      access.users[i] = user;
      bool saved = saveUserAccounts(&access);
      unlock();
      return saved;
    }
  }

  unlock();
  return false;
}

UserAccount *StorageLayer::findUserByMac(const char *macAddress)
{
  static UserAccount foundUser;

  if (!validateMacFormat(macAddress))
    return nullptr;

  AccessControlRuntime access;
  if (!loadUserAccounts(&access))
    return nullptr;

  String normalizedMac = normalizeMac(String(macAddress));

  for (uint8_t i = 0; i < access.userCount; i++)
  {
    if (strcmp(access.users[i].macAddress, normalizedMac.c_str()) == 0)
    {
      foundUser = access.users[i];
      return &foundUser;
    }
  }

  return nullptr;
}
// STORAGE MANAGEMENT START

bool StorageLayer::logsEnabled() const { return logsEnabled_; }

void StorageLayer::setLogsEnabled(bool enabled)
{
  logsEnabled_ = enabled;
  if (!lock()) return;
  preferences_.putBool(LOG_ENABLED_KEY, enabled);
  unlock();
  Serial.printf("[Storage] Activity logging %s\n",
                enabled ? "enabled" : "disabled");
}

bool StorageLayer::isUserStorageFull() const
{
  // Load fresh from NVS; do not rely on caller's cached count.
  AccessControlRuntime access;
  if (!const_cast<StorageLayer *>(this)->loadUserAccounts(&access)) return false;
  return access.userCount >= MAX_USER_ACCOUNTS;
}

// Removes non-admin (or optionally all) users whose lastAccess is either
// zero (never logged in) or older than inactiveThresholdDays before nowEpoch.
// Admins are always preserved when keepAdmins is true.
// Returns the number of accounts removed.
uint8_t StorageLayer::removeInactiveUsers(uint64_t nowEpoch,
                                           uint32_t inactiveThresholdDays,
                                           bool keepAdmins)
{
  if (!lock()) return 0;

  AccessControlRuntime access;
  if (!loadUserAccounts(&access))
  {
    unlock();
    return 0;
  }

  const uint64_t cutoff =
      (nowEpoch > static_cast<uint64_t>(inactiveThresholdDays) * 86400ULL)
          ? nowEpoch - static_cast<uint64_t>(inactiveThresholdDays) * 86400ULL
          : 0ULL;

  uint8_t kept = 0;
  uint8_t removed = 0;
  for (uint8_t i = 0; i < access.userCount; ++i)
  {
    const UserAccount &u = access.users[i];
    if (keepAdmins && u.isAdmin)
    {
      access.users[kept++] = u;
      continue;
    }
    const bool neverLoggedIn = (u.lastAccess == 0);
    const bool oldAccess     = (u.lastAccess > 0 && u.lastAccess < cutoff);
    if (neverLoggedIn || oldAccess)
    {
      Serial.printf("[Storage] removeInactiveUsers: removing %s (%s)\n",
                    u.macAddress,
                    neverLoggedIn ? "never logged in" : "inactive");
      ++removed;
    }
    else
    {
      access.users[kept++] = u;
    }
  }

  if (removed == 0)
  {
    unlock();
    return 0;
  }

  access.userCount = kept;
  saveUserAccounts(&access);
  unlock();
  Serial.printf("[Storage] removeInactiveUsers: removed %u, kept %u\n",
                static_cast<unsigned>(removed),
                static_cast<unsigned>(kept));
  return removed;
}

// STORAGE MANAGEMENT END
// ACCESS CONTROL END
