#pragma once

#include "SystemTypes.h"

#ifndef ALLOW_RATED_RESET
#define ALLOW_RATED_RESET false
#endif
// ACCESS CONTROL START
#ifndef ENABLE_ACCESS_CONTROL
#define ENABLE_ACCESS_CONTROL true
#endif

constexpr char PREF_ACCESS_NAMESPACE[] = "access_users";
constexpr char FILE_UNAUTHORIZED[] = "/unauthorized.html";
constexpr uint8_t MAX_AUTH_ATTEMPTS = 5;
constexpr uint32_t AUTH_LOCKOUT_SECONDS = 300;
// ACCESS CONTROL END
constexpr char AP_SSID[] = "tarshid";
constexpr char AP_PASSWORD[] = "12345678";

// Keep blank if no infrastructure Wi-Fi is available. The build script can
// inject WIFI_STA_SSID/WIFI_STA_PASSWORD for ONLINE mode without changing the
// offline firmware defaults or local MAC-auth behavior.
#ifndef WIFI_STA_SSID
#define WIFI_STA_SSID ""
#endif

#ifndef WIFI_STA_PASSWORD
#define WIFI_STA_PASSWORD ""
#endif

constexpr char STA_SSID[] = WIFI_STA_SSID;
constexpr char STA_PASSWORD[] = WIFI_STA_PASSWORD;

constexpr RelayConfig RELAY_CONFIG[] = {
    {26, "Relay A", 60.0f},
    {27, "Relay B", 100.0f},
};
constexpr size_t RELAY_COUNT = sizeof(RELAY_CONFIG) / sizeof(RELAY_CONFIG[0]);
static_assert(RELAY_COUNT <= RELAY_MASK_BITS, "Relay count exceeds PIR relay-mask capacity.");

// Most ESP32 relay boards are active-low:
// LOW energizes the relay, HIGH releases it.
// Keeping this configurable here lets the rest of the control logic
// use natural ON/OFF semantics without inverting button behavior.
constexpr bool RELAY_ACTIVE_LOW = true;

constexpr PirConfig PIR_CONFIG[] = {
    {32, relayMaskForRelay(0), "PIR A"},                              // drives Relay A
    {33, relayMaskForRelay(1), "PIR B"},                              // drives Relay B
    {25, relayMaskForRelay(0) | relayMaskForRelay(1), "PIR C"},       // shared area drives both relays
};
constexpr size_t PIR_COUNT = sizeof(PIR_CONFIG) / sizeof(PIR_CONFIG[0]);

constexpr uint32_t CONTROL_TASK_PERIOD_MS = 50;
constexpr uint32_t WEB_TASK_PERIOD_MS = 10;
constexpr uint32_t PIR_DEBOUNCE_MS = 180;
constexpr uint32_t PIR_HOLD_SECONDS = 30;
constexpr uint32_t HOUSEKEEPING_PERIOD_MS = 1500;

constexpr uint32_t LOG_MAX_BYTES = 120 * 1024;
constexpr uint32_t PENDING_MAX_BYTES = 48 * 1024;
constexpr uint16_t LOG_FETCH_MAX_ITEMS = 200;
constexpr uint8_t LOG_RETENTION_DAYS = 2;
constexpr uint32_t STATS_FLUSH_INTERVAL_SECONDS = 300;

constexpr uint8_t DAY_START_HOUR = 6;
constexpr uint8_t NIGHT_START_HOUR = 18;

constexpr uint16_t HTTP_PORT = 80;
constexpr uint16_t WS_PORT = 81;
constexpr uint8_t WS_MAX_CLIENTS = 8;

constexpr uint8_t WATCHDOG_TIMEOUT_SECONDS = 12;

// CLOUD SYNC START
// Hybrid online/offline mode is additive. When Supabase credentials are not
// injected at build time, all cloud code stays disabled and offline behavior is
// unchanged.
#ifndef CLOUD_SYNC_ENABLED
#define CLOUD_SYNC_ENABLED 0
#endif

#ifndef SUPABASE_URL
#define SUPABASE_URL ""
#endif

#ifndef SUPABASE_PUBLISHABLE_KEY
#define SUPABASE_PUBLISHABLE_KEY ""
#endif

#ifndef CLOUD_DEVICE_ID
#define CLOUD_DEVICE_ID ""
#endif

// Required shared device token for Supabase RPC calls. It is never stored in
// frontend code; the ESP uses it only to authenticate state/event/command RPCs.
#ifndef CLOUD_COMMAND_TOKEN
#define CLOUD_COMMAND_TOKEN ""
#endif

constexpr uint32_t CLOUD_TASK_PERIOD_MS = 100;
constexpr uint32_t CLOUD_HTTP_TIMEOUT_MS = 1200;
constexpr uint32_t CLOUD_STATE_SYNC_INTERVAL_MS = 15000;
constexpr uint32_t CLOUD_COMMAND_POLL_INTERVAL_MS = 2500;
constexpr uint32_t CLOUD_QUEUE_FLUSH_INTERVAL_MS = 5000;
constexpr uint32_t CLOUD_QUEUE_MAX_BYTES = 32 * 1024;
constexpr uint8_t CLOUD_EVENT_QUEUE_LENGTH = 32;
constexpr uint8_t CLOUD_MAX_COMMANDS_PER_POLL = 2;
constexpr uint16_t CLOUD_MAX_RESPONSE_BYTES = 4096;
constexpr char FILE_CLOUD_QUEUE[] = "/cloud_queue.jsonl";
constexpr char CLOUD_LAST_COMMAND_KEY[] = "cloud_last_cmd";
// CLOUD SYNC END

constexpr char PREF_NAMESPACE[] = "smart_home";
constexpr char FILE_LOGS[] = "/logs.jsonl";
constexpr char FILE_PENDING[] = "/pending.jsonl";

// STORAGE MANAGEMENT START
// Days without login before a non-admin account is considered inactive.
constexpr uint32_t LOG_INACTIVITY_DAYS = 30;
// NVS key used to persist the activity-log enabled flag across reboots.
constexpr char LOG_ENABLED_KEY[] = "log_en";
// STORAGE MANAGEMENT END
