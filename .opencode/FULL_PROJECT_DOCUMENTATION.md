# Tarshid ESP32 Smart Home — Full Project Documentation

## Table of Contents
1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Wi-Fi & Network Management](#wi-fi--network-management)
4. [Supabase / Backend](#supabase--backend)
5. [Time Handling](#time-handling)
6. [Web UI](#web-ui)
7. [API Reference](#api-reference)
8. [Configuration & Build Variables](#configuration--build-variables)
9. [NVS Keys Reference](#nvs-keys-reference)
10. [FreeRTOS Task Layout](#freertos-task-layout)
11. [LittleFS File Layout](#littlefs-file-layout)
12. [Supabase Schema Summary](#supabase-schema-summary)
13. [Extending the Project](#extending-the-project)

---

## Overview

Tarshid is an ESP32-based smart home automation controller. It manages up to `RELAY_COUNT` relay channels and `PIR_COUNT` passive-infrared motion sensors. It has two distinct operating modes:

| Mode | Condition | Features |
|------|-----------|----------|
| **OFFLINE_LOCAL** | No internet / no Wi-Fi credentials | SoftAP + WebSocket local control |
| **ONLINE_CLOUD** | Internet reachable + Supabase configured | SoftAP closed, state synced to Supabase, remote commands accepted |

The device always boots into `OFFLINE_LOCAL` first. It switches to `ONLINE_CLOUD` only after internet is confirmed.

---

## Architecture

### Source Files

| File | Role |
|------|------|
| `src/main.cpp` | FreeRTOS tasks, Wi-Fi lifecycle, mode transitions, boot sequence |
| `src/Config.h` | All compile-time constants, NVS key names, pin mappings |
| `src/SystemTypes.h` | Shared data types (`RelayRuntime`, `TimerPlan`, `PIRMapping`, …) |
| `src/ControlEngine.h/.cpp` | Relay state machine, timer logic, PIR debounce |
| `src/StorageLayer.h/.cpp` | LittleFS + NVS Preferences persistence |
| `src/WebPortal.h/.cpp` | WebServer (port 80) + WebSocketsServer (port 81) + captive portal |
| `src/CloudSyncService.h/.cpp` | Supabase HTTPS RPC client |
| `src/TimeKeeper.h/.cpp` | NTP sync + device-clock fallback |

### FreeRTOS Tasks

```
controlTask  (core 1, priority 2) — 8 KB stack
  └─ ControlEngine::tickFast()  every CONTROL_TASK_PERIOD_MS
  └─ Watchdog reset

networkTask  (core 0, priority 1) — 12 KB stack
  └─ processProvisionRequest()  — consume AP setup page request
  └─ maintainNetworkMode()      — internet probe + mode switch
  └─ WebPortal::loop()          — WebServer + WebSocket + DNS (OFFLINE only)
  └─ maintainWiFi()             — reconnect / AP health
  └─ checkWiFiHealth()          — AP watchdog (OFFLINE only)
  └─ TimeKeeper sync + persist

cloudTask    (core 0, priority 1) — 12 KB stack  [only when cloud configured]
  └─ CloudSyncService::loop()   every CLOUD_TASK_PERIOD_MS
      └─ processRealtimeEventQueue()
      └─ syncStateSnapshot()    when stateDirty_
      └─ flushStoredEventQueue()
      └─ pollRemoteCommands()
```

---

## Wi-Fi & Network Management

### Boot Sequence

```
setup()
  │
  ├─ gStorage.begin()             — LittleFS + NVS
  ├─ gStorage.loadRuntime()       — relay/PIR/timer state
  ├─ gTimeKeeper.begin()          — restore persisted epoch
  ├─ gControl.begin()
  │
  ├─ setupWiFi()
  │   ├─ loadWifiCredentials()    — read NVS (w_ssid, w_pass, w_always…)
  │   ├─ if alwaysConnect && credentials:
  │   │     WiFi.mode(WIFI_AP_STA)
  │   │     startSecureSoftAp()
  │   │     WiFi.begin(primary SSID)
  │   └─ else:
  │         WiFi.mode(WIFI_AP)
  │         startSecureSoftAp()
  │
  ├─ gCloudSync.begin()
  │
  ├─ if alwaysConnect && credentials: wait up to 8 s for WL_CONNECTED
  │
  └─ if connected && internet: enterOnlineMode()
     else:                     enterOfflineMode()
```

### AP Mode (OFFLINE_LOCAL)

- SoftAP always starts at boot and stays up until internet is confirmed.
- SSID: `AP_SSID` (compile-time, default hidden).
- Password: `AP_PASSWORD` (compile-time).
- The captive portal redirects all unknown hosts to `http://<AP_IP>/`.
- Both `index.html` (relay dashboard) and `wifi.html` (Wi-Fi setup) are served from LittleFS.

### Connecting to a Network

Two ways to provide credentials:

**1. Via `/wifi.html` at runtime (recommended)**  
User visits `http://192.168.4.1/wifi.html` while connected to the AP, scans for networks, enters password, saves. The page POSTs to `/api/wifi/connect`; `WebPortal` stores the request in `wifiProvision_`; `networkTask` picks it up via `processProvisionRequest()` on the next iteration.

**2. Via compile-time build variables (fallback)**  
Set `WIFI_STA_SSID` and `WIFI_STA_PASSWORD` in `platformio.ini`. These are used only when no runtime credentials are stored in NVS.

### alwaysConnect Toggle

When `alwaysConnect = true` (saved to NVS key `w_always`):
- `setupWiFi()` starts in `WIFI_AP_STA` and immediately calls `WiFi.begin()`.
- The background STA reconnect loop in `maintainWiFi()` runs every 10 s.
- The AP is closed only after internet is confirmed (`enterOnlineMode()`).

When `alwaysConnect = false` (default on fresh device):
- The device stays in `WIFI_AP` only.
- The user must manually trigger a connection via `/wifi.html`.

### Backup Network

If a backup SSID is saved (`w_bak_ssid` in NVS), and the primary network fails internet probes `INTERNET_FAILURE_THRESHOLD` consecutive times:
1. `gUsingBackupNetwork` is set to `true`.
2. `WiFi.begin(backupSsid, backupPass)` is called.
3. `gNetworkMode` is reset to `UNKNOWN` so a fresh probe runs.
4. If internet succeeds on backup → `enterOnlineMode()`.
5. If backup also fails → `enterOfflineMode()`, AP opens.

### Internet Probe (`probeInternetAccess`)

1. **DNS**: resolve `pool.ntp.org` → must return a non-zero IP.
2. **HTTP**: TCP connect to `httpbin.org:80`, send `GET /status/200`, expect `200` in response.

Probe runs every `INTERNET_PROBE_INTERVAL_MS` (30 s) from `maintainNetworkMode()`.

### Mode Transitions

```
enterOfflineMode()
  ├─ Start/keep SoftAP
  ├─ WiFi mode: WIFI_AP (or WIFI_AP_STA if alwaysConnect)
  └─ WebPortal::begin()

enterOnlineMode()
  ├─ WebPortal::end()
  ├─ WiFi.softAPdisconnect(true)
  ├─ WiFi mode: WIFI_STA
  ├─ CloudSyncService::registerDevice()  [first time only]
  ├─ CloudSyncService::syncConfigToCloud()
  ├─ TimeKeeper::trySyncFromNtp(force=true)
  └─ CloudSyncService::requestStateSync()
```

---

## Supabase / Backend

### Prerequisites

The following must be defined in `platformio.ini` (via environment variables or `build_flags`):

```ini
-D CLOUD_SYNC_ENABLED=1
-D SUPABASE_URL=\"https://your-project.supabase.co\"
-D SUPABASE_PUBLISHABLE_KEY=\"eyJ...\"
-D CLOUD_COMMAND_TOKEN=\"your-secret-command-token\"
-D DEVICE_ID=\"esp32-home-01\"
```

### Device Registration (`registerDevice`)

Called once per online session (guarded by `gDeviceRegistered`).  
Calls the Supabase RPC `device_self_register(p_device_id, p_token, p_name)`:
- Creates the device row in `smart_home_devices` if it does not exist.
- Updates `command_token_hash` = SHA-256 of `p_token`.
- Creates a `smart_home_device_states` row if absent.
- Granted to the `anon` role — no user session required.

### Config Sync (`syncConfigToCloud`)

Called immediately after first registration.  
Sets `stateDirty_ = true` and calls `syncStateSnapshot()` which POSTs the full engine state JSON (relay names, modes, timer configs, PIR mappings) to `device_upsert_state` RPC.

### Ongoing Sync

| Action | Trigger | RPC |
|--------|---------|-----|
| State snapshot | stateDirty_ or every 60 s | `device_upsert_state` |
| Relay/timer event | ControlEngine callback | `device_insert_event` |
| Remote commands | every `CLOUD_POLL_INTERVAL_MS` | `device_claim_commands` / `device_finish_command` |

### Offline Event Queue

When internet is temporarily unavailable, events are persisted to `/cloud_queue.jsonl` in LittleFS (bounded by `CLOUD_QUEUE_MAX_BYTES`). They are replayed via `flushStoredEventQueue()` after reconnection. PIR motion/idle events are **not** persisted to avoid unbounded writes.

---

## Time Handling

Priority order (highest → lowest):

1. **NTP** (`pool.ntp.org`, forced on every `enterOnlineMode()` call).  
   Stored as `gNtpEpoch` via `setEpoch()`.

2. **User/browser clock** — sent by the UI via `time_sync` WebSocket message or `POST /api/setTime`.  
   Stored separately as `gUserEpoch` so it can be distinguished from the NTP time.

3. **Device clock fallback** — persisted to NVS every `TimeKeeper::maybePersistSyncPoint()` call (≥ 1 h apart). Restored at boot from `prefs.getULong64("epoch_persist")`.

API functions return `nowUserEpoch()` when user/browser time is available, falling back to `nowEpoch()` (NTP or persisted). This ensures timestamps in logs, events, and API responses are always coherent regardless of internet availability.

---

## Web UI

### Pages (served from LittleFS)

| Path | Description |
|------|-------------|
| `/` → `index.html` | Main relay dashboard (all features) |
| `/wifi.html` | Wi-Fi setup page |
| `/restricted.html` | Relay-only view for restricted users |
| `/unauthorized.html` | Access denied page |

### Shared Assets

| Path | Description |
|------|-------------|
| `/styles.css` | Shared CSS extracted from `index.html` (all design tokens + components) |

All pages link `<link rel="stylesheet" href="/styles.css">` so new pages inherit the full design system without duplicating CSS.

### Wi-Fi Setup Page (`/wifi.html`)

- **Status bar** — live connection state via `GET /api/wifi/status` (polls every 15 s).
- **Scan** — `POST /api/wifi/scan` → returns up to 20 networks with SSID, RSSI, open/secured.
- **Credential form** — SSID (pre-filled from scan click) + password + show/hide button.
- **Save toggle** — persist credentials to NVS; required for alwaysConnect.
- **Always Connect toggle** — shows/hides backup network section.
- **Backup network section** — separate SSID + password saved via `POST /api/wifi/backup`.
- **Clear** — `DELETE /api/wifi/credentials` wipes all Wi-Fi NVS keys.

### Design System (CSS variables)

| Variable | Purpose |
|----------|---------|
| `--accent` | Brand colour (buttons, borders, highlights) |
| `--surface`, `--surface-2` | Card and nested card backgrounds |
| `--text`, `--text-2`, `--muted` | Typography hierarchy |
| `--on`, `--off` | Relay state colours |
| `--warn`, `--warn-soft` | Timer active state |
| `--border`, `--radius`, `--radius-pill` | Layout primitives |

Dark mode is the default (`data-theme="dark"` on `<html>`).

---

## API Reference

### Wi-Fi Setup (AP mode only)

| Method | Path | Body | Response |
|--------|------|------|----------|
| GET | `/api/wifi/status` | — | `{connected, ssid, ip, rssi, alwaysConnect, savedSsid, backupSsid, hasBackup}` |
| POST | `/api/wifi/scan` | — | `{networks:[{ssid, rssi, open}]}` |
| POST | `/api/wifi/connect` | `{ssid, pass, save, alwaysConnect}` | `{ok, msg}` |
| POST | `/api/wifi/backup` | `{ssid, pass}` | `{ok, msg}` |
| DELETE | `/api/wifi/credentials` | — | `{ok, msg}` |

### Relay & Timer Control (WebSocket, port 81)

All commands are JSON messages sent over WebSocket.

| type | Required fields | Effect |
|------|----------------|--------|
| `set_manual` | `channel`, `mode` (ON/OFF/AUTO) | Set relay manual override |
| `set_timer` | `channel`, `target`, `durationMinutes` or `durationSec` | Start countdown timer |
| `cancel_timer` | `channel` | Cancel active timer |
| `set_rated_power` | `channel`, `powerW` | Set rated power for energy tracking |
| `set_energy_tracking` | `enabled` | Enable/disable energy tracking |
| `set_pir_mapping` | `mappings: [{relayMask}]` | Configure PIR → relay assignments |
| `reset_consumption` | — | Reset all energy accumulation counters |
| `get_state` | — | Request full state snapshot |
| `time_sync` | `epoch`, `tzOffsetMinutes` | Synchronise device clock from browser |

### State & Logs (HTTP, port 80)

| Method | Path | Response |
|--------|------|----------|
| GET | `/api/state` | Full engine state JSON |
| GET | `/api/logs?limit=N` | Recent JSONL log entries |
| GET | `/api/time` | `{epoch, valid, userValid, dayPhase}` |
| POST | `/api/setTime` | Set device time from JSON body |
| POST | `/api/pirMapping` | Set PIR → relay mappings |
| POST | `/api/resetConsumption` | Reset energy counters |
| POST | `/api/ratedPower` | Set rated power per channel |

---

## Configuration & Build Variables

Defined in `platformio.ini` via `build_flags`:

| Variable | Default | Description |
|----------|---------|-------------|
| `AP_SSID` | `"ESP32-Home"` | SoftAP network name |
| `AP_PASSWORD` | `"setup1234"` | SoftAP password |
| `STA_SSID` | `""` | Compile-time primary SSID (overridden by NVS) |
| `STA_PASSWORD` | `""` | Compile-time primary password |
| `RELAY_COUNT` | `4` | Number of relay channels |
| `PIR_COUNT` | `2` | Number of PIR sensors |
| `CLOUD_SYNC_ENABLED` | `0` | Enable Supabase sync |
| `SUPABASE_URL` | `""` | `https://xxx.supabase.co` |
| `SUPABASE_PUBLISHABLE_KEY` | `""` | Supabase anon key |
| `CLOUD_COMMAND_TOKEN` | `""` | Device authentication token |
| `DEVICE_ID` | `""` | Unique device identifier |
| `ENABLE_ACCESS_CONTROL` | `0` | Enable MAC-based access control |
| `WATCHDOG_TIMEOUT_SECONDS` | `30` | FreeRTOS task watchdog timeout |

---

## NVS Keys Reference

All keys live in the `smart_home` Preferences namespace.

| Key | Type | Description |
|-----|------|-------------|
| `w_ssid` | String | Runtime primary SSID |
| `w_pass` | String | Runtime primary password |
| `w_bak_ssid` | String | Backup SSID |
| `w_bak_pass` | String | Backup password |
| `w_always` | Bool | Connect automatically on every boot |
| `cloud_reg` | Bool | Device successfully registered in Supabase |
| `log_en` | Bool | Activity-log enabled flag |
| `epoch_persist` | UInt64 | Last known epoch (device-clock fallback) |
| `tz_offset` | Int32 | Timezone offset in minutes |
| `relay_N_*` | Various | Per-relay mode, state, stats, rated power |
| `timer_N_*` | Various | Per-relay timer plan |
| `pir_N_map` | UInt64 | PIR → relay bitmask |
| `users_json` | String | Serialised access control roster |

---

## FreeRTOS Task Layout

```
Core 0                          Core 1
──────────────────────          ──────────────────────
networkTask (prio 1)            controlTask (prio 2)
  • Wi-Fi management              • ControlEngine::tickFast()
  • WebServer/WebSocket           • Relay GPIO writes
  • DNS captive portal            • Timer expiry
  • Internet probing              • PIR debounce
  • Provision requests            • Energy tracking

cloudTask (prio 1)
  • Supabase HTTPS RPCs
  • Event queue flush
  • Remote command polling
```

Tasks communicate through:
- `gRuntime` (protected by `gStateMutex`)
- `gWebPortal.outboundQueue_` / `inboundQueue_` (FreeRTOS queues)
- `gCloudSync.eventQueue_` (FreeRTOS queue)
- `gWebPortal.wifiProvision_` (single-producer, consumed by networkTask)

---

## LittleFS File Layout

```
/
├── index.html          Main relay dashboard
├── styles.css          Shared CSS (extracted from index.html)
├── wifi.html           Wi-Fi setup page
├── restricted.html     Relay-only view (restricted users)
├── unauthorized.html   Access denied page
├── logs.jsonl          Recent activity log (bounded)
├── pending.jsonl       Buffered offline WebSocket events
└── cloud_queue.jsonl   Persisted cloud events awaiting upload
```

---

## Supabase Schema Summary

| Table | Primary Key | Description |
|-------|-------------|-------------|
| `smart_home_devices` | `id (text)` | Device registry |
| `smart_home_device_states` | `device_id` | Latest full state snapshot |
| `smart_home_device_events` | `id (bigserial)` | Relay/timer/system event log |
| `smart_home_remote_commands` | `id (uuid)` | Pending commands from UI users |
| `smart_home_profiles` | `id (uuid)` | User profiles linked to `auth.users` |
| `smart_home_device_memberships` | `(device_id, user_id)` | Role-based access per device |

### Key RPCs (callable by ESP32 via anon key)

| Function | Auth | Description |
|----------|------|-------------|
| `device_self_register(id, token, name)` | anon | Upsert device + initial state row |
| `device_upsert_state(id, token, epoch, state)` | anon | Push full state snapshot |
| `device_insert_event(id, token, event, ts, dedupe, payload)` | anon | Append an event |
| `device_claim_commands(id, token, limit)` | anon | Claim pending remote commands |
| `device_finish_command(id, token, cmd_id, status, ok, msg, epoch)` | anon | Mark command done/failed |

---

## Extending the Project

### Adding a New Web Page

1. Create `SmartHomeAutomation/data/your_page.html`.
2. Add `<link rel="stylesheet" href="/styles.css">` in `<head>`.
3. Add a route in `WebPortal::setupRoutes()`:
   ```cpp
   server_.on("/your_page.html", HTTP_GET, [this]() {
     File f = LittleFS.open("/your_page.html", FILE_READ);
     if (!f) { server_.send(404, "text/plain", "Not found"); return; }
     server_.streamFile(f, "text/html");
     f.close();
   });
   ```
4. Optionally add a nav chip to `index.html`'s `<nav class="quicknav">`.

### Adding a New Relay or PIR Channel

1. Update `RELAY_COUNT` / `PIR_COUNT` in `platformio.ini`.
2. Add entries to `RELAY_CONFIG[]` and `PIR_CONFIG[]` in `Config.h`.
3. Re-upload the firmware.

### Adding a New NVS Setting

1. Add a `constexpr char` key to `Config.h`.
2. Use `StorageLayer::saveBoolSetting()` / `saveStringSetting()` / `saveIntSetting()`.
3. Read back with the corresponding `readXxxSetting()` method.

### Remote Commands (from Supabase)

New command types must be added to:
1. `smart_home_private.command_payload_shape_allowed()` in the Supabase schema.
2. `CloudSyncService::applyRemoteCommand()` in `CloudSyncService.cpp`.
