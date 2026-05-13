#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
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

bool hasStaCredentials() {
  return strlen(STA_SSID) > 0;
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
    return;
  }
  gWebPortal.enqueueEvent(eventJson, bufferIfOffline);
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
  // Keep credentials/runtime Wi-Fi state in RAM only so reconnect attempts do
  // not generate extra flash churn or stale network state across brownouts.
  WiFi.persistent(false);
  // Disable Wi-Fi sleep so websocket and AP responsiveness stay stable under
  // bursts of commands and frequent browser interaction.
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWiFiEvent);

  if (hasStaCredentials()) {
    // ONLINE mode probe starts from STA only. If internet/Supabase are not
    // available, enterOfflineMode() starts the AP and local portal explicitly.
    WiFi.mode(WIFI_STA);
    WiFi.begin(STA_SSID, STA_PASSWORD);
    Serial.printf("[WiFi] Connecting STA to %s\n", STA_SSID);
  } else {
    WiFi.mode(WIFI_OFF);
    Serial.println("[WiFi] STA credentials empty; offline AP mode will start.");
  }
}

bool probeInternetAccess() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  IPAddress resolved;
  // DNS resolution is a lightweight internet reachability check. It is called
  // on a slow cadence only, never from relay/PIR timing paths.
  return WiFi.hostByName("pool.ntp.org", resolved) == 1 && resolved != IPAddress(0, 0, 0, 0);
}

void enterOfflineMode() {
  if (gNetworkMode == NetworkMode::OFFLINE_LOCAL && gWebPortal.isRunning() &&
      WiFi.softAPIP() != IPAddress(0, 0, 0, 0)) {
    return;
  }

  const bool keepSta = hasStaCredentials();
  WiFi.mode(keepSta ? WIFI_AP_STA : WIFI_AP);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  const bool apOk = startSecureSoftAp();
  if (apOk) {
    Serial.printf("[Mode] OFFLINE_LOCAL AP ready SSID=%s IP=%s\n",
                  AP_SSID,
                  WiFi.softAPIP().toString().c_str());
  } else {
    Serial.println("[Mode] OFFLINE_LOCAL failed to start SoftAP.");
  }
  if (keepSta && WiFi.status() != WL_CONNECTED) {
    WiFi.begin(STA_SSID, STA_PASSWORD);
  }

  gNetworkMode = NetworkMode::OFFLINE_LOCAL;
  gWebPortal.begin(&gControl, &gStorage, &gTimeKeeper);
  pushSystemEvent("mode.offline", "Offline local AP mode active.");
}

void enterOnlineMode() {
  if (!onlineModeAvailable() || WiFi.status() != WL_CONNECTED) {
    enterOfflineMode();
    return;
  }

  if (gNetworkMode == NetworkMode::ONLINE_CLOUD && !gWebPortal.isRunning() &&
      WiFi.getMode() == WIFI_STA) {
    return;
  }

  // ONLINE mode must not expose local offline pages, captive DNS, WebSocket, or
  // MAC-auth routes. Stop the portal first, then remove SoftAP completely.
  gWebPortal.end();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(STA_SSID, STA_PASSWORD);
  }

  gNetworkMode = NetworkMode::ONLINE_CLOUD;
  gConsecutiveInternetFailures = 0;
  gCloudSync.requestStateSync();
  gTimeKeeper.trySyncFromNtp(true);
  pushSystemEvent("mode.online", "Online cloud mode active.");
  Serial.println("[Mode] ONLINE_CLOUD active; SoftAP and local portal are disabled.");
}

void maintainNetworkMode() {
  constexpr uint32_t INTERNET_PROBE_INTERVAL_MS = 30000UL;
  constexpr uint8_t INTERNET_FAILURE_THRESHOLD = 3;

  if (!onlineModeAvailable()) {
    enterOfflineMode();
    return;
  }

  const uint32_t nowMs = millis();
  if (gNetworkMode == NetworkMode::UNKNOWN) {
    if (probeInternetAccess()) {
      enterOnlineMode();
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
    if (gNetworkMode != NetworkMode::ONLINE_CLOUD) {
      enterOnlineMode();
    }
    return;
  }

  if (gNetworkMode == NetworkMode::ONLINE_CLOUD &&
      ++gConsecutiveInternetFailures >= INTERNET_FAILURE_THRESHOLD) {
    Serial.println("[Mode] Internet/cloud reachability lost; falling back to offline AP.");
    enterOfflineMode();
  }
}

void maintainWiFi() {
  static uint32_t lastApCheckMs = 0;
  static uint32_t lastStaReconnectMs = 0;
  const uint32_t nowMs = millis();

  if (gNetworkMode == NetworkMode::ONLINE_CLOUD) {
    if (WiFi.getMode() != WIFI_STA) {
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
    }
    if (hasStaCredentials() && WiFi.status() != WL_CONNECTED &&
        (nowMs - lastStaReconnectMs) >= 10000UL) {
      lastStaReconnectMs = nowMs;
      WiFi.reconnect();
      if (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect(false, false);
        WiFi.begin(STA_SSID, STA_PASSWORD);
      }
    }
    return;
  }

  if (nowMs - lastApCheckMs >= 2500UL) {
    lastApCheckMs = nowMs;
    if (WiFi.softAPIP() == IPAddress(0, 0, 0, 0)) {
      // Re-assert the AP mode before restart so the captive portal and websocket
      // server stay reachable even after transient Wi-Fi stack faults.
      WiFi.mode(hasStaCredentials() ? WIFI_AP_STA : WIFI_AP);
      WiFi.setSleep(false);
      if (startSecureSoftAp()) {
        pushSystemEvent("wifi.ap_restarted", "SoftAP restarted automatically after a connection failure.", false, true);
      }
    }
  }

  if (hasStaCredentials() && WiFi.status() != WL_CONNECTED && (nowMs - lastStaReconnectMs) >= 10000UL) {
    lastStaReconnectMs = nowMs;
    // Retry STA reconnection without blocking the network task. A soft reconnect
    // is attempted first, then a full begin() refresh if the station is still down.
    WiFi.reconnect();
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect(false, false);
      WiFi.begin(STA_SSID, STA_PASSWORD);
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

    WiFi.mode(hasStaCredentials() ? WIFI_AP_STA : WIFI_AP);
    WiFi.persistent(false);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    startSecureSoftAp();
    if (hasStaCredentials()) {
      WiFi.begin(STA_SSID, STA_PASSWORD);
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
    maintainNetworkMode();
    if (gNetworkMode == NetworkMode::OFFLINE_LOCAL) {
      gWebPortal.loop();
    }
    maintainWiFi();
    if (gNetworkMode == NetworkMode::OFFLINE_LOCAL) {
      checkWiFiHealth();
    }
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
    } else {
      gWebPortal.enqueueEvent(json, bufferIfOffline);
    }
  });

  if (onlineModeAvailable()) {
    const uint32_t staWaitStart = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - staWaitStart) < 8000UL) {
      delay(100);
    }
  }
  if (onlineModeAvailable() && probeInternetAccess()) {
    enterOnlineMode();
  } else {
    enterOfflineMode();
  }

  pushSystemEvent("system.boot", "System boot completed.");
  gControl.refreshOutputs();

  xTaskCreatePinnedToCore(controlTask, "control_task", 8192, nullptr, 2, &gControlTaskHandle, 1);
  xTaskCreatePinnedToCore(networkTask, "network_task", 12288, nullptr, 1, &gNetworkTaskHandle, 0);
  if (gCloudSync.isConfigured()) {
    xTaskCreatePinnedToCore(cloudTask, "cloud_task", 12288, nullptr, 1, &gCloudTaskHandle, 0);
  }
}

void loop() {
  // Main loop remains idle because FreeRTOS tasks own runtime behavior.
  delay(1000);
}
