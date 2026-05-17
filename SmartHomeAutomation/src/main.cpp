#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>
#include <LittleFS.h>
#include <type_traits>
#include <utility>
#include "CloudSyncService.h"
#include "Config.h"
#include "ControlEngine.h"
#include "StorageLayer.h"
#include "SystemTypes.h"
#include "TimeKeeper.h"
#include "WebPortal.h"

StorageLayer gStorage;
TimeKeeper gTimeKeeper;
ControlEngine gControl;
WebPortal gWebPortal;
CloudSyncService gCloudSync;

SystemRuntime gRuntime{};
SemaphoreHandle_t gStateMutex = nullptr;

TaskHandle_t gControlTaskHandle = nullptr;
TaskHandle_t gNetworkTaskHandle = nullptr;
TaskHandle_t gCloudTaskHandle = nullptr;

namespace {
enum class WiFiApRecoveryState : uint8_t {
  IDLE,
  WAITING_FOR_WIFI_OFF,
  WAITING_FOR_AP_READY,
};

enum class NetworkMode : uint8_t {
  UNKNOWN,
  OFFLINE_LOCAL,
  ONLINE_CLOUD,
};

// Keep Arduino SoftAP defaults for channel/max clients so only security
// posture changes here (hidden SSID + client isolation).
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONNECTIONS = 4;
constexpr bool AP_HIDDEN = true;

template <typename T, typename = void>
struct WifiApConfigHasIsolate : std::false_type {};

template <typename T>
struct WifiApConfigHasIsolate<T, decltype((void) std::declval<T &>().ap_isolate, void())> : std::true_type {};

template <typename T>
typename std::enable_if<WifiApConfigHasIsolate<T>::value, bool>::type enableDriverApIsolation(T &apConfig) {
  apConfig.ap_isolate = 1;
  return true;
}

template <typename T>
typename std::enable_if<!WifiApConfigHasIsolate<T>::value, bool>::type enableDriverApIsolation(T &) {
  return false;
}

WiFiApRecoveryState gWiFiApRecoveryState = WiFiApRecoveryState::IDLE;
NetworkMode gNetworkMode = NetworkMode::UNKNOWN;
uint32_t gLastWiFiHealthCheckMs = 0;
uint32_t gLastWiFiRecoveryMs = 0;
uint32_t gWiFiRecoveryStateMs = 0;
uint32_t gLastInternetProbeMs = 0;
uint8_t gConsecutiveWiFiHealthFailures = 0;
uint8_t gConsecutiveInternetFailures = 0;

// WIFI RUNTIME START
// Credentials loaded from NVS at boot. Override compile-time STA_SSID/STA_PASSWORD.
String gRuntimePrimarySSID;
String gRuntimePrimaryPass;
String gRuntimeBackupSSID;
String gRuntimeBackupPass;
bool gAlwaysConnect       = false; // persist+auto-connect on every boot
bool gUsingBackupNetwork  = false; // true while the backup SSID is active
bool gDeviceRegistered    = false; // true once registerDevice() succeeded
// WIFI RUNTIME END

bool hasStaCredentials() {
  return !gRuntimePrimarySSID.isEmpty() || strlen(STA_SSID) > 0;
}

bool hasBackupNetwork() {
  return !gRuntimeBackupSSID.isEmpty();
}

// The SSID/pass the STA should connect to right now (primary unless we fell
// back to the backup because the primary lost internet).
String activeStaSsid() {
  if (gUsingBackupNetwork && !gRuntimeBackupSSID.isEmpty()) return gRuntimeBackupSSID;
  return gRuntimePrimarySSID.isEmpty() ? String(STA_SSID) : gRuntimePrimarySSID;
}

String activeStaPass() {
  if (gUsingBackupNetwork && !gRuntimeBackupSSID.isEmpty()) return gRuntimeBackupPass;
  return gRuntimePrimarySSID.isEmpty() ? String(STA_PASSWORD) : gRuntimePrimaryPass;
}

bool onlineModeAvailable() {
  return hasStaCredentials() && gCloudSync.isConfigured();
}

const char *networkModeName(NetworkMode mode) {
  switch (mode) {
    case NetworkMode::OFFLINE_LOCAL:
      return "OFFLINE_LOCAL";
    case NetworkMode::ONLINE_CLOUD:
      return "ONLINE_CLOUD";
    default:
      return "UNKNOWN";
  }
}

bool applySoftApSecurityConfig() {
  wifi_config_t wifiConfig{};
  if (esp_wifi_get_config(WIFI_IF_AP, &wifiConfig) != ESP_OK) {
    return false;
  }

  // Keep the SoftAP hidden after boot and AP recovery.
  // Reassert hidden SSID at driver level so AP recovery keeps the network
  // hidden even after Wi-Fi stack restarts.
  wifiConfig.ap.ssid_hidden = 1;
  const bool isolationSupported = enableDriverApIsolation(wifiConfig.ap);
  const bool configApplied = esp_wifi_set_config(WIFI_IF_AP, &wifiConfig) == ESP_OK;
  if (!isolationSupported) {
    Serial.println("[WiFi] Warning: current framework does not expose driver-level AP isolation.");
  }
  return configApplied;
}

bool startSecureSoftAp() {
  const bool apOk = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, AP_HIDDEN, AP_MAX_CONNECTIONS);
  if (!apOk) {
    return false;
  }
  if (!applySoftApSecurityConfig()) {
    Serial.println("[WiFi] Warning: failed to apply AP isolation/hidden config.");
  }
  return true;
}
}  // namespace

String buildSystemEvent(const String &eventName, const String &message, const String &logType) {
  JsonDocument doc;
  doc["type"] = logType;
  doc["event"] = eventName;
  doc["msg"] = message;
  // Prefer the user-derived clock when available so system events line up with
  // timer and log timestamps after the first browser/device synchronization.
  const uint64_t eventTs = gTimeKeeper.nowUserEpoch() > 0 ? gTimeKeeper.nowUserEpoch() : gTimeKeeper.nowEpoch();
  doc["ts"] = eventTs;
  String payload;
  serializeJson(doc, payload);
  return payload;
}

void pushSystemEvent(const String &eventName, const String &message, bool bufferIfOffline = false, bool isError = false) {
  const String eventJson = buildSystemEvent(eventName, message, isError ? "ERROR" : "TIMER");
  if (gNetworkMode == NetworkMode::ONLINE_CLOUD) {
    gCloudSync.enqueueLocalEvent(eventJson);
  }
  // AP remains active in both modes — always forward to local portal so
  // AP-connected clients see real-time state changes even while online.
  if (gWebPortal.isRunning()) {
    gWebPortal.enqueueEvent(eventJson, bufferIfOffline);
  }
}

void initRuntimeDefaults() {
  gRuntime.relays.assign(RELAY_COUNT, RelayRuntime{});
  gRuntime.pirs.assign(PIR_COUNT, PirRuntime{});
  gRuntime.pirMap.assign(PIR_COUNT, PIRMapping{});
  gRuntime.energyTrackingEnabled = false;
  gRuntime.connectedClients = 0;
  gRuntime.dayPhase = DayPhase::DAY;
  gRuntime.timeValid = false;
  gRuntime.nightLockActive = false;

  for (size_t i = 0; i < RELAY_COUNT; ++i) {
    gRuntime.relays[i].manualMode = RelayMode::AUTO;
    gRuntime.relays[i].appliedState = RelayState::OFF;
    gRuntime.relays[i].appliedSource = ControlSource::NONE;
    gRuntime.relays[i].timer.active = false;
    gRuntime.relays[i].timer.startEpoch = 0;
    gRuntime.relays[i].timer.endEpoch = 0;
    gRuntime.relays[i].timer.targetState = RelayState::OFF;
    gRuntime.relays[i].timer.previousState = RelayState::OFF;
    gRuntime.relays[i].timer.previousManualMode = RelayMode::AUTO;
    gRuntime.relays[i].timer.durationMinutes = 0;
    gRuntime.relays[i].timer.restorePending = false;
    gRuntime.relays[i].autoHoldUntilEpoch = 0;
    gRuntime.relays[i].ratedPowerWatts = RELAY_CONFIG[i].ratedPowerWatts;
    gRuntime.relays[i].ratedPowerLocked = false;
    gRuntime.relays[i].energyTrackingActive = false;
    gRuntime.relays[i].energyStartEpoch = 0;
    gRuntime.relays[i].stats.timerUses = 0;
    gRuntime.relays[i].stats.totalTimerMinutes = 0;
    gRuntime.relays[i].stats.accumulatedOnSeconds = 0;
    gRuntime.relays[i].stats.lastOnEpoch = 0;
    gRuntime.relays[i].stats.totalEnergyWh = 0.0f;
    gRuntime.relays[i].stats.lastEnergyWh = 0.0f;
  }

  for (size_t i = 0; i < PIR_COUNT; ++i) {
    gRuntime.pirs[i].rawValue = false;
    gRuntime.pirs[i].stableValue = false;
    gRuntime.pirs[i].lastChangeMs = 0;
    gRuntime.pirs[i].lastTriggerEpoch = 0;
    // PIR MAPPING START
    gRuntime.pirMap[i].relayMask = PIR_CONFIG[i].relayMask & relayMaskForCount(RELAY_COUNT);
    // PIR MAPPING END
  }
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  (void)info;
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      pushSystemEvent("wifi.sta_connected", "Connected to upstream Wi-Fi.");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      pushSystemEvent("wifi.sta_ip", String("STA IP: ") + WiFi.localIP().toString());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      pushSystemEvent("wifi.sta_disconnected", "Disconnected from upstream Wi-Fi.", false, true);
      break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      pushSystemEvent("wifi.ap_client_connected", "A device joined the ESP32 AP.");
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      pushSystemEvent("wifi.ap_client_disconnected", "A device left the ESP32 AP.");
      break;
    default:
      break;
  }
}

void setupWiFi() {
  // Load runtime credentials from NVS first — they override compile-time build vars.
  gStorage.loadWifiCredentials(gRuntimePrimarySSID, gRuntimePrimaryPass,
                               gRuntimeBackupSSID, gRuntimeBackupPass,
                               gAlwaysConnect);
  if (!gRuntimePrimarySSID.isEmpty()) {
    Serial.printf("[WiFi] Runtime credentials loaded — primary: \"%s\", alwaysConnect: %s\n",
                  gRuntimePrimarySSID.c_str(), gAlwaysConnect ? "YES" : "NO");
  }
  if (!gRuntimeBackupSSID.isEmpty()) {
    Serial.printf("[WiFi] Backup network: \"%s\"\n", gRuntimeBackupSSID.c_str());
  }

  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWiFiEvent);
  // Erase any credentials the Arduino Wi-Fi stack may have cached.
  WiFi.disconnect(true, true);
  delay(100);

  // DESIGN: always start the AP so the setup page is reachable.
  // If alwaysConnect is set AND we have credentials, also begin STA in
  // WIFI_AP_STA mode so the device can reach the internet in the background.
  // The AP is closed only after internet is confirmed in enterOnlineMode().
  if (gAlwaysConnect && hasStaCredentials()) {
    WiFi.mode(WIFI_AP_STA);
    startSecureSoftAp();
    WiFi.begin(activeStaSsid().c_str(), activeStaPass().c_str());
    Serial.printf("[WiFi] AP+STA: connecting to \"%s\"\n", activeStaSsid().c_str());
  } else {
    WiFi.mode(WIFI_AP);
    startSecureSoftAp();
    Serial.println("[WiFi] AP-only mode (no alwaysConnect or no credentials).");
  }
}

bool probeInternetAccess() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Internet] Probe skipped: WiFi not connected");
    return false;
  }

  // Stage 1: DNS resolution (fast, lightweight)
  IPAddress resolved;
  if (WiFi.hostByName("pool.ntp.org", resolved) != 1 || resolved == IPAddress(0, 0, 0, 0)) {
    Serial.println("[Internet] DNS resolution failed (pool.ntp.org)");
    return false;
  }
  Serial.printf("[Internet] DNS OK: pool.ntp.org -> %s\n", resolved.toString().c_str());

  // Stage 2: HTTP connectivity check — confirm we can reach an actual server,
  // not just resolve DNS through a captive portal.
  WiFiClient client;
  client.setTimeout(5);
  if (!client.connect("httpbin.org", 80)) {
    Serial.println("[Internet] HTTP connection to httpbin.org:80 failed");
    return false;
  }
  client.print("GET /status/200 HTTP/1.0\r\nHost: httpbin.org\r\nConnection: close\r\n\r\n");

  uint32_t httpStart = millis();
  while (!client.available() && (millis() - httpStart) < 5000) {
    delay(50);
  }

  bool httpOk = false;
  if (client.available()) {
    String statusLine = client.readStringUntil('\n');
    httpOk = statusLine.indexOf("200") >= 0;
    Serial.printf("[Internet] HTTP response: %s -> %s\n",
                  statusLine.c_str(), httpOk ? "OK" : "FAIL");
  } else {
    Serial.println("[Internet] HTTP response timeout");
  }
  client.stop();
  return httpOk;
}

void enterOfflineMode() {
  if (gNetworkMode == NetworkMode::OFFLINE_LOCAL && gWebPortal.isRunning() &&
      WiFi.softAPIP() != IPAddress(0, 0, 0, 0)) {
    return;
  }

  const bool keepSta = hasStaCredentials() && gAlwaysConnect;
  WiFi.mode(keepSta ? WIFI_AP_STA : WIFI_AP);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  const bool apOk = startSecureSoftAp();

  Serial.println("[Mode] OFFLINE_LOCAL activating");
  if (apOk) {
    Serial.printf("[Mode]   AP SSID: %s  IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  } else {
    Serial.println("[Mode]   WARNING: SoftAP failed to start!");
  }
  Serial.printf("[Mode]   WiFi mode: %s\n", keepSta ? "WIFI_AP_STA" : "WIFI_AP");
  if (keepSta) {
    Serial.printf("[Mode]   STA background reconnect to \"%s\" enabled\n", activeStaSsid().c_str());
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.begin(activeStaSsid().c_str(), activeStaPass().c_str());
    }
  }

  gNetworkMode = NetworkMode::OFFLINE_LOCAL;
  if (!gWebPortal.isRunning()) {
    gWebPortal.begin(&gControl, &gStorage, &gTimeKeeper);
  }
  pushSystemEvent("mode.offline", "Offline local AP mode active.");
}

void enterOnlineMode() {
  if (!onlineModeAvailable() || WiFi.status() != WL_CONNECTED) {
    enterOfflineMode();
    return;
  }

  if (gNetworkMode == NetworkMode::ONLINE_CLOUD) {
    return;
  }

  // Keep AP+STA so the setup page and local dashboard remain reachable
  // even when Supabase is the primary control channel.
  // The AP is intentionally NOT closed — it serves wifi.html for reconfiguration
  // and allows local clients to use the dashboard as a fallback.
  WiFi.mode(WIFI_AP_STA);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  if (WiFi.softAPIP() == IPAddress(0, 0, 0, 0)) {
    startSecureSoftAp();
  }
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(activeStaSsid().c_str(), activeStaPass().c_str());
  }

  gNetworkMode = NetworkMode::ONLINE_CLOUD;
  gConsecutiveInternetFailures = 0;
  gCloudSync.requestStateSync();
  gTimeKeeper.trySyncFromNtp(true);

  // Keep local portal running for AP-connected clients (setup page + fallback UI).
  if (!gWebPortal.isRunning()) {
    gWebPortal.begin(&gControl, &gStorage, &gTimeKeeper);
  }

  // WIFI RUNTIME: Register device in Supabase on first successful online connection.
  if (!gDeviceRegistered) {
    Serial.println("[Boot] Registering device in Supabase…");
    if (gCloudSync.registerDevice()) {
      gDeviceRegistered = true;
      gStorage.saveBoolSetting(WIFI_NVS_REGISTERED, true);
      Serial.println("[Boot] Device registered. Syncing config to cloud…");
      gCloudSync.syncConfigToCloud();
    } else {
      Serial.println("[Boot] Device registration failed (will retry on next online transition).");
    }
  }

  pushSystemEvent("mode.online", "Online cloud mode active. AP remains up for local access.");

  Serial.println("[Mode] ONLINE_CLOUD active (AP+STA)");
  Serial.printf("[Mode]   STA IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[Mode]   AP  IP: %s\n", WiFi.softAPIP().toString().c_str());
}

void maintainNetworkMode() {
  constexpr uint32_t INTERNET_PROBE_INTERVAL_MS = 30000UL;
  constexpr uint8_t  INTERNET_FAILURE_THRESHOLD  = 3;

  // With no credentials and no cloud config there is nothing to do online.
  // Always ensure the offline AP is running.
  if (!hasStaCredentials()) {
    enterOfflineMode();
    return;
  }

  const uint32_t nowMs = millis();

  // First probe after boot or after a provision request changed credentials.
  if (gNetworkMode == NetworkMode::UNKNOWN) {
    if (WiFi.status() == WL_CONNECTED && probeInternetAccess()) {
      gUsingBackupNetwork = false;
      if (onlineModeAvailable()) {
        enterOnlineMode();
      } else {
        // STA connected but no cloud config — stay offline AP (user pages).
        enterOfflineMode();
      }
    } else {
      enterOfflineMode();
    }
    gLastInternetProbeMs = nowMs;
    return;
  }

  if (nowMs - gLastInternetProbeMs < INTERNET_PROBE_INTERVAL_MS) {
    return;
  }
  gLastInternetProbeMs = nowMs;

  const bool internetOk = probeInternetAccess();
  if (internetOk) {
    gConsecutiveInternetFailures = 0;
    if (gNetworkMode != NetworkMode::ONLINE_CLOUD && onlineModeAvailable()) {
      gUsingBackupNetwork = false;
      enterOnlineMode();
    }
    return;
  }

  ++gConsecutiveInternetFailures;

  // Try switching to backup network when the primary keeps failing.
  if (gConsecutiveInternetFailures == INTERNET_FAILURE_THRESHOLD &&
      !gUsingBackupNetwork && hasBackupNetwork()) {
    Serial.printf("[Mode] Primary lost after %u failures — trying backup \"%s\".\n",
                  gConsecutiveInternetFailures, gRuntimeBackupSSID.c_str());
    gUsingBackupNetwork = true;
    gConsecutiveInternetFailures = 0;
    gNetworkMode = NetworkMode::UNKNOWN; // re-probe on next tick
    WiFi.disconnect(false, false);
    WiFi.begin(gRuntimeBackupSSID.c_str(), gRuntimeBackupPass.c_str());
    if (gNetworkMode == NetworkMode::ONLINE_CLOUD) {
      // Reopen AP while we try the backup
      enterOfflineMode();
    }
    return;
  }

  if (gNetworkMode == NetworkMode::ONLINE_CLOUD &&
      gConsecutiveInternetFailures >= INTERNET_FAILURE_THRESHOLD) {
    Serial.printf("[Mode] Internet lost (%u consecutive failures); falling back to offline AP.\n",
                  gConsecutiveInternetFailures);
    gUsingBackupNetwork = false;
    enterOfflineMode();
  }
}

void maintainWiFi() {
  static uint32_t lastApCheckMs = 0;
  static uint32_t lastStaReconnectMs = 0;
  const uint32_t nowMs = millis();

  if (gNetworkMode == NetworkMode::ONLINE_CLOUD) {
    // Keep AP+STA — AP serves setup/local UI alongside Supabase cloud control.
    if (WiFi.getMode() != WIFI_AP_STA) {
      WiFi.mode(WIFI_AP_STA);
    }
    if (nowMs - lastApCheckMs >= 2500UL) {
      lastApCheckMs = nowMs;
      if (WiFi.softAPIP() == IPAddress(0, 0, 0, 0)) {
        if (startSecureSoftAp()) {
          pushSystemEvent("wifi.ap_restarted", "SoftAP restarted in online mode.", false, true);
        }
      }
    }
    if (hasStaCredentials() && WiFi.status() != WL_CONNECTED &&
        (nowMs - lastStaReconnectMs) >= 10000UL) {
      lastStaReconnectMs = nowMs;
      WiFi.reconnect();
      if (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect(false, false);
        WiFi.begin(activeStaSsid().c_str(), activeStaPass().c_str());
      }
    }
    return;
  }

  if (nowMs - lastApCheckMs >= 2500UL) {
    lastApCheckMs = nowMs;
    if (WiFi.softAPIP() == IPAddress(0, 0, 0, 0)) {
      WiFi.mode(hasStaCredentials() && gAlwaysConnect ? WIFI_AP_STA : WIFI_AP);
      WiFi.setSleep(false);
      if (startSecureSoftAp()) {
        pushSystemEvent("wifi.ap_restarted", "SoftAP restarted automatically after a connection failure.", false, true);
      }
    }
  }

  if (hasStaCredentials() && gAlwaysConnect && WiFi.status() != WL_CONNECTED &&
      (nowMs - lastStaReconnectMs) >= 10000UL) {
    lastStaReconnectMs = nowMs;
    WiFi.reconnect();
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect(false, false);
      WiFi.begin(activeStaSsid().c_str(), activeStaPass().c_str());
    }
  }
}

bool isWiFiApHealthy() {
  const wifi_mode_t mode = WiFi.getMode();
  const bool apModeOk = mode == WIFI_AP || mode == WIFI_AP_STA;
  const bool apIpOk = WiFi.softAPIP() != IPAddress(0, 0, 0, 0);

  wifi_sta_list_t wifiStaList;
  memset(&wifiStaList, 0, sizeof(wifiStaList));
  const bool staQueryOk = esp_wifi_ap_get_sta_list(&wifiStaList) == ESP_OK;

  // Read station count as part of the watchdog path. This keeps the health
  // check aligned with the AP connection bookkeeping used elsewhere.
  (void)WiFi.softAPgetStationNum();

  return apModeOk && apIpOk && staQueryOk;
}

void startWiFiApRecovery() {
  if (gNetworkMode != NetworkMode::OFFLINE_LOCAL) {
    return;
  }
  if (gWiFiApRecoveryState != WiFiApRecoveryState::IDLE) {
    return;
  }

  gLastWiFiRecoveryMs = millis();
  gWiFiRecoveryStateMs = gLastWiFiRecoveryMs;
  gConsecutiveWiFiHealthFailures = 0;
  gWiFiApRecoveryState = WiFiApRecoveryState::WAITING_FOR_WIFI_OFF;

  pushSystemEvent("wifi.ap_watchdog",
                  "WiFi AP watchdog triggered automatic AP recovery.",
                  false,
                  true);

  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
}

void processWiFiApRecovery() {
  if (gNetworkMode != NetworkMode::OFFLINE_LOCAL) {
    gWiFiApRecoveryState = WiFiApRecoveryState::IDLE;
    return;
  }
  const uint32_t nowMs = millis();

  if (gWiFiApRecoveryState == WiFiApRecoveryState::WAITING_FOR_WIFI_OFF) {
    if (nowMs - gWiFiRecoveryStateMs < 300UL) {
      return;
    }

    WiFi.mode(hasStaCredentials() && gAlwaysConnect ? WIFI_AP_STA : WIFI_AP);
    WiFi.persistent(false);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    startSecureSoftAp();
    if (hasStaCredentials() && gAlwaysConnect) {
      WiFi.begin(activeStaSsid().c_str(), activeStaPass().c_str());
    }

    gWiFiApRecoveryState = WiFiApRecoveryState::WAITING_FOR_AP_READY;
    gWiFiRecoveryStateMs = nowMs;
    return;
  }

  if (gWiFiApRecoveryState == WiFiApRecoveryState::WAITING_FOR_AP_READY) {
    if (WiFi.softAPIP() != IPAddress(0, 0, 0, 0)) {
      gWebPortal.recoverAfterAccessPointRestart();
      pushSystemEvent("wifi.ap_recovered", "WiFi AP recovered automatically.");
      gWiFiApRecoveryState = WiFiApRecoveryState::IDLE;
      return;
    }

    // Abort the current recovery cycle and wait for the cooldown before trying
    // again. This prevents an infinite restart loop if the radio is genuinely down.
    if (nowMs - gWiFiRecoveryStateMs >= 4000UL) {
      pushSystemEvent("wifi.ap_recovery_timeout",
                      "WiFi AP recovery timed out; watchdog will retry later.",
                      false,
                      true);
      gWiFiApRecoveryState = WiFiApRecoveryState::IDLE;
    }
  }
}

void checkWiFiHealth() {
  constexpr uint32_t WIFI_HEALTH_CHECK_INTERVAL_MS = 5000UL;
  constexpr uint32_t WIFI_RECOVERY_COOLDOWN_MS = 20000UL;
  constexpr uint8_t WIFI_HEALTH_FAILURE_THRESHOLD = 2;

  if (gNetworkMode != NetworkMode::OFFLINE_LOCAL) {
    return;
  }

  processWiFiApRecovery();
  if (gWiFiApRecoveryState != WiFiApRecoveryState::IDLE) {
    return;
  }

  const uint32_t nowMs = millis();
  if (nowMs - gLastWiFiHealthCheckMs < WIFI_HEALTH_CHECK_INTERVAL_MS) {
    return;
  }
  gLastWiFiHealthCheckMs = nowMs;

  if (isWiFiApHealthy()) {
    gConsecutiveWiFiHealthFailures = 0;
    return;
  }

  if (nowMs - gLastWiFiRecoveryMs < WIFI_RECOVERY_COOLDOWN_MS) {
    return;
  }

  if (++gConsecutiveWiFiHealthFailures >= WIFI_HEALTH_FAILURE_THRESHOLD) {
    startWiFiApRecovery();
  }
}

void initWatchdog() {
#if defined(ESP_IDF_VERSION_MAJOR) && (ESP_IDF_VERSION_MAJOR >= 5)
  esp_task_wdt_config_t config = {
      .timeout_ms = WATCHDOG_TIMEOUT_SECONDS * 1000,
      .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
      .trigger_panic = true,
  };
  esp_err_t result = esp_task_wdt_init(&config);
#else
  esp_err_t result = esp_task_wdt_init(WATCHDOG_TIMEOUT_SECONDS, true);
#endif
  if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
    Serial.printf("[WDT] init warning: %d\n", result);
  }
}

// WIFI PROVISION START
// Called each iteration of networkTask to process any Wi-Fi setup request
// that the user submitted via /wifi.html.  Runs on the network task so all
// WiFi.begin() calls are serialised with the rest of the Wi-Fi management.
void processProvisionRequest() {
  if (!gWebPortal.hasProvisionRequest()) return;
  const WiFiProvisionRequest req = gWebPortal.getAndClearProvisionRequest();

  if (req.isBackup) {
    // Save backup credentials only — no immediate connection attempt.
    gRuntimeBackupSSID = req.ssid;
    gRuntimeBackupPass = req.pass;
    gStorage.saveWifiBackup(gRuntimeBackupSSID, gRuntimeBackupPass);
    Serial.printf("[Provision] Backup network saved: \"%s\"\n", req.ssid);
    return;
  }

  // Primary network request — optionally save, then initiate connection.
  if (req.save) {
    gRuntimePrimarySSID = req.ssid;
    gRuntimePrimaryPass = req.pass;
    gAlwaysConnect      = req.alwaysConnect;
    gStorage.saveWifiPrimary(gRuntimePrimarySSID, gRuntimePrimaryPass, gAlwaysConnect);
    Serial.printf("[Provision] Primary network saved: \"%s\", alwaysConnect=%d\n",
                  req.ssid, req.alwaysConnect);
  }

  gUsingBackupNetwork = false;
  gConsecutiveInternetFailures = 0;
  // Force a fresh probe on the next maintainNetworkMode() tick.
  gNetworkMode = NetworkMode::UNKNOWN;
  gLastInternetProbeMs = 0;

  // Switch to AP+STA so the setup page remains accessible while connecting.
  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect(false, false);
  const String targetSsid = req.ssid;
  const String targetPass = req.pass;
  WiFi.begin(targetSsid.c_str(), targetPass.c_str());
  Serial.printf("[Provision] Connecting to \"%s\"…\n", req.ssid);
}
// WIFI PROVISION END

void controlTask(void *parameter) {
  (void)parameter;
  esp_task_wdt_add(NULL);
  while (true) {
    gControl.tickFast();
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(CONTROL_TASK_PERIOD_MS));
  }
}

void networkTask(void *parameter) {
  (void)parameter;
  esp_task_wdt_add(NULL);
  uint32_t lastHousekeeping = 0;
  while (true) {
    // WIFI PROVISION: handle any setup request from /wifi.html BEFORE probing
    // internet so the new credentials are live when maintainNetworkMode runs.
    processProvisionRequest();

    maintainNetworkMode();
    gWebPortal.loop(); // AP stays up in both modes — portal always runs
    maintainWiFi();
    checkWiFiHealth(); // AP watchdog active in both modes
    gTimeKeeper.trySyncFromNtp(gNetworkMode == NetworkMode::ONLINE_CLOUD);
    gTimeKeeper.maybePersistSyncPoint();

    const uint32_t nowMs = millis();
    if (nowMs - lastHousekeeping >= HOUSEKEEPING_PERIOD_MS) {
      lastHousekeeping = nowMs;
      gControl.tickHousekeeping();
    }
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(WEB_TASK_PERIOD_MS));
  }
}

void cloudTask(void *parameter) {
  (void)parameter;
  esp_task_wdt_add(NULL);
  while (true) {
    // Cloud sync is intentionally isolated from the local network task so slow
    // Supabase HTTP/retry work cannot stall WebServer/WebSocket handling.
    if (gNetworkMode == NetworkMode::ONLINE_CLOUD) {
      gCloudSync.loop();
    }
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(CLOUD_TASK_PERIOD_MS));
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  initRuntimeDefaults();
  gStateMutex = xSemaphoreCreateMutex();
  if (!gStateMutex) {
    Serial.println("[Init] Failed to allocate state mutex.");
    while (true) {
      delay(1000);
    }
  }

  if (!gStorage.begin()) {
    Serial.println("[Init] LittleFS/Preferences initialization failed.");
    while (true) {
      delay(1000);
    }
  }
    // Fail fast if the web UI is missing, instead of bringing up a broken AP/server.
  if (!LittleFS.exists("/index.html")) {
    Serial.println("[Init] Missing /index.html in LittleFS. Upload filesystem image again.");
    while (true) {
      delay(1000);
    }
  }

  gStorage.loadRuntime(&gRuntime);

  // FIX R5: probe the access-control roster at boot and report it on Serial.
  // gStorage.loadRuntime() above only restores relay/PIR/timer state -- it does
  // NOT load user accounts (verified in StorageLayer::loadRuntime). Without
  // this probe, an NVS issue with the "users_json" key is invisible until a
  // user tries to log in via the web UI. WebPortal::begin() loads the roster
  // into its own cache later; this call is purely diagnostic and re-uses the
  // same StorageLayer API (no new function added).
  {
    AccessControlRuntime accessProbe{};
    if (gStorage.loadUserAccounts(&accessProbe)) {
      Serial.printf("[Boot] Access control: %u/%u user account(s) loaded from NVS\n",
                    static_cast<unsigned>(accessProbe.userCount),
                    static_cast<unsigned>(MAX_USER_ACCOUNTS));
    } else {
      Serial.println("[Boot] Access control: loadUserAccounts() FAILED — "
                     "roster unavailable until next successful load.");
    }
  }

  gTimeKeeper.begin(gStorage.prefs());

  initWatchdog();

  gControl.begin(&gRuntime, &gStorage, &gTimeKeeper, gStateMutex);
  setupWiFi();
  gCloudSync.begin(&gControl, &gStorage, &gTimeKeeper);
  gControl.setEventCallback([](const String &json, bool bufferIfOffline) {
    if (gNetworkMode == NetworkMode::ONLINE_CLOUD) {
      gCloudSync.enqueueLocalEvent(json);
    }
    // Always forward to local portal — AP stays up in both modes so
    // AP-connected clients receive real-time relay/timer state updates.
    if (gWebPortal.isRunning()) {
      gWebPortal.enqueueEvent(json, bufferIfOffline);
    }
  });

  // Only block on STA connection at boot if the user chose alwaysConnect and
  // we have runtime credentials.  On a fresh device (no credentials saved) we
  // skip the wait entirely and go straight to AP mode so the setup page is
  // available within seconds of powering on.
  if (gAlwaysConnect && hasStaCredentials()) {
    Serial.printf("[Boot] alwaysConnect=true — waiting for STA \"%s\"…\n",
                  activeStaSsid().c_str());
    const uint32_t staWaitStart = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - staWaitStart) < 8000UL) {
      delay(100);
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[Boot] STA connected in %lu ms  IP: %s\n",
                    millis() - staWaitStart, WiFi.localIP().toString().c_str());
    } else {
      Serial.printf("[Boot] STA connection FAILED after %lu ms (status: %d)\n",
                    millis() - staWaitStart, WiFi.status());
    }
  }

  if (onlineModeAvailable() && WiFi.status() == WL_CONNECTED && probeInternetAccess()) {
    Serial.println("[Boot] Internet verified — entering ONLINE mode");
    enterOnlineMode();
  } else {
    if (onlineModeAvailable() && gAlwaysConnect) {
      Serial.println("[Boot] Internet probe failed — falling back to OFFLINE AP mode");
    }
    enterOfflineMode();
  }

  pushSystemEvent("system.boot", "System boot completed.");
  gControl.refreshOutputs();

  xTaskCreatePinnedToCore(controlTask, "control_task", 8192, nullptr, 2, &gControlTaskHandle, 1);
  xTaskCreatePinnedToCore(networkTask, "network_task", 12288, nullptr, 1, &gNetworkTaskHandle, 0);
  if (gCloudSync.isConfigured()) {
    xTaskCreatePinnedToCore(cloudTask, "cloud_task", 16384, nullptr, 1, &gCloudTaskHandle, 0);
  }
}

void loop() {
  // Main loop remains idle because FreeRTOS tasks own runtime behavior.
  delay(1000);
}
