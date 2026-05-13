#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <functional>

#include "SystemTypes.h"

class StorageLayer {
 public:
  bool begin();
  Preferences *prefs();

  void loadRuntime(SystemRuntime *runtime);
  void persistManualMode(size_t relayIndex, RelayMode mode);
  void persistRelayState(size_t relayIndex, RelayState state, ControlSource source);
  void persistTimer(size_t relayIndex, const TimerPlan &plan);
  void persistRelayStats(size_t relayIndex, const RelayStats &stats);
  void persistRelayEnergyStats(size_t relayIndex, float totalEnergyWh, float lastEnergyWh);
  void persistRatedPower(size_t relayIndex, float watts, bool locked);
  // PIR MAPPING START
  void persistPirMapping(size_t pirIndex, const PIRMapping &mapping);
  // PIR MAPPING END
  // STORAGE START
  // Save small configuration values in NVS/Preferences. These survive reboot.
  bool saveBoolSetting(const char *key, bool value);
  bool saveIntSetting(const char *key, int32_t value);
  bool saveStringSetting(const char *key, const String &value);

  // Read key-value settings back from NVS at boot/runtime.
  bool readBoolSetting(const char *key, bool defaultValue, bool *outValue);
  bool readIntSetting(const char *key, int32_t defaultValue, int32_t *outValue);
  bool readStringSetting(const char *key, const String &defaultValue, String *outValue);

  // Save/load structured data as JSON files in LittleFS.
  bool writeJsonFile(const char *path, const JsonDocument &doc);
  bool readJsonFile(const char *path, JsonDocument *doc) const;

  // Delete one key, one file, log files, or all application data.
  bool deleteSettingKey(const char *key);
  bool deleteFile(const char *path) const;
  bool clearLogFiles();
  bool factoryReset();
  // STORAGE END
// ACCESS CONTROL START
bool loadUserAccounts(AccessControlRuntime *access);
bool saveUserAccounts(const AccessControlRuntime *access);
bool addUserAccount(const UserAccount &user);
bool removeUserAccount(const char *macAddress);
bool updateUserAccount(const UserAccount &user);
UserAccount* findUserByMac(const char *macAddress);
bool validateMacFormat(const char *macAddress);
// STORAGE MANAGEMENT START
// Returns true when the roster has reached MAX_USER_ACCOUNTS.
bool isUserStorageFull() const;
// Removes non-admin users whose lastAccess is 0 or older than
// inactiveThresholdDays days before nowEpoch.
// Returns the number of accounts removed.
uint8_t removeInactiveUsers(uint64_t nowEpoch, uint32_t inactiveThresholdDays, bool keepAdmins);
// Activity-log on/off flag (persisted to NVS).
bool logsEnabled() const;
void setLogsEnabled(bool enabled);
// STORAGE MANAGEMENT END
// ACCESS CONTROL END
  void persistEnergyTrackingEnabled(bool enabled);
  void persistLastCleanupDay(uint32_t dayToken);
  uint32_t loadLastCleanupDay();

  void appendEvent(uint64_t epoch, const String &type, const String &message, int channel = -1);
  void appendEventJson(const String &jsonLine);
  void appendPending(const String &jsonLine);
  // CLOUD SYNC START
  // Separate bounded FIFO for cloud retry events. PIR motion/idle history is
  // intentionally never written here by CloudSyncService.
  void appendCloudQueue(const String &jsonLine);
  bool readCloudQueueHead(String *lineOut) const;
  void dropCloudQueueHead();
  // CLOUD SYNC END
  String readRecentLogsJson(uint16_t limit) const;
  void flushPending(const std::function<void(const String &line)> &sender);
  void cleanupDaily(uint64_t nowEpoch);

 private:
  bool lock() const;
  void unlock() const;
  void appendLine(const char *path, const String &line) const;
  void trimFileBySize(const char *path, uint32_t maxBytes) const;
  bool parseEpochFromLine(const String &line, uint64_t *epochOut) const;
  void compactByAge(const char *path, uint64_t minEpochToKeep) const;

  Preferences preferences_;
  mutable SemaphoreHandle_t ioMutex_ = nullptr;
  bool logsEnabled_ = true; // runtime cache; NVS is authoritative
};
