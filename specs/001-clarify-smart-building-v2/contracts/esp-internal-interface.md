# ESP32 Internal Interface Contract

## Core Components

### SystemTypes.h — Shared Types

```cpp
// Relay state
enum class RelayState : uint8_t { OFF, ON };
enum class ControlSource : uint8_t { NONE, BROWSER, TIMER, PIR, CLOUD };
enum class RelayMode : uint8_t { AUTO, MANUAL };

// Runtime state per relay
struct RelayRuntime {
  RelayMode manualMode;
  RelayState appliedState;
  ControlSource appliedSource;
  TimerRuntime timer;
  uint32_t autoHoldUntilEpoch;
  uint16_t ratedPowerWatts;
  bool ratedPowerLocked;
  bool energyTrackingActive;
  uint32_t energyStartEpoch;
  RelayStats stats;
};
```

### ControlEngine — Relay Control

```cpp
class ControlEngine {
public:
  void begin(SystemRuntime* runtime, StorageLayer* storage, TimeKeeper* tk, SemaphoreHandle_t mutex);
  void tickFast();        // Called every 50ms from control task
  void tickHousekeeping(); // Called every 1s from network task
  
  // Relay control — uses hardware_channel ONLY (no friendly names)
  void setRelay(uint8_t channel, RelayState state, ControlSource source);
  void setRelayManual(uint8_t channel, RelayMode mode);
  RelayState getRelayState(uint8_t channel);
  
  // Timer operations
  void setTimer(uint8_t channel, uint32_t durationMinutes, RelayState targetState);
  void cancelTimer(uint8_t channel);
  
  // Refresh all outputs from runtime state
  void refreshOutputs();
};
```

### StorageLayer — Persistence

```cpp
class StorageLayer {
public:
  bool begin();
  Preferences* prefs();  // NVS handle
  
  // Runtime state
  bool loadRuntime(SystemRuntime* runtime);
  bool saveRuntime(const SystemRuntime& runtime);
  
  // WiFi credentials
  bool loadWifiCredentials(String& ssid, String& pass, String& backupSsid, String& backupPass, bool& alwaysConnect);
  void saveWifiPrimary(const String& ssid, const String& pass, bool alwaysConnect);
  void saveWifiBackup(const String& ssid, const String& pass);
  
  // Settings
  bool saveBoolSetting(const char* key, bool value);
  bool loadBoolSetting(const char* key, bool defaultVal);
  
  // User access (limited — for local AP basic auth, not used in V2)
  bool loadUserAccounts(AccessControlRuntime* runtime);
};
```

### CloudSyncService — Supabase Communication

```cpp
class CloudSyncService {
public:
  void begin(ControlEngine* control, StorageLayer* storage, TimeKeeper* tk);
  void loop();        // Called from cloud task
  
  bool isConfigured();
  bool registerDevice();       // POST /devices
  void syncConfigToCloud();    // Send ESP config
  void requestStateSync();     // Trigger immediate sync
  void enqueueLocalEvent(const String& json);  // Queue outgoing event
  
  // State sync
  void syncStates();           // POST pending states
  void fetchConfig();          // GET device config
};
```

### WebPortal — Offline AP Mode

```cpp
class WebPortal {
public:
  void begin(ControlEngine* control, StorageLayer* storage, TimeKeeper* tk);
  void end();                  // Shutdown AP mode
  void loop();                 // Handle HTTP+WS requests
  
  bool isRunning();
  void recoverAfterAccessPointRestart();
  
  // Event queue
  void enqueueEvent(const String& json, bool bufferIfOffline);
  
  // Wi-Fi provisioning
  bool hasProvisionRequest();
  WiFiProvisionRequest getAndClearProvisionRequest();
};
```

### TimeKeeper — Clock & Sync

```cpp
class TimeKeeper {
public:
  void begin(Preferences* prefs);
  void trySyncFromNtp(bool force);
  void maybePersistSyncPoint();
  
  uint64_t nowEpoch();         // System epoch (millis-based)
  uint64_t nowUserEpoch();     // NTP-synced epoch (0 if not synced)
};
```

## Task Architecture (FreeRTOS)

| Task | Core | Priority | Stack | Period | Purpose |
|------|------|----------|-------|--------|---------|
| controlTask | 1 | 2 | 8KB | 50ms | Relay control, timer evaluation, PIR monitoring |
| networkTask | 0 | 1 | 12KB | 10ms | Web portal, WiFi management, health checks |
| cloudTask | 0 | 1 | 12KB | 50ms | Supabase HTTP/WS sync (only in online mode) |

## Data Flow

```
WebSocket Client (AP) ←→ WebPortal ←→ ControlEngine ←→ Hardware (GPIO)
                                      ↕
                                  StorageLayer
                                      ↕
CloudSyncService ←→ Supabase (HTTPS/WS) ←→ Online Dashboard
```

## Key Design Rules

1. ControlEngine NEVER accesses friendly names, user accounts, roles, or permissions
2. CloudSyncService sends relay channel IDs (0-based), NEVER friendly names
3. StorageLayer persists relay states as channel+state pairs, NEVER with display names
4. WebPortal in AP mode provides relay control via hardware channel numbers only
5. Timestamps for conflict resolution are set by TimeKeeper (NTP-synced epoch when available)
