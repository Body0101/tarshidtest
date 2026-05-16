# Tarshid — ESP32 Smart Home Automation System

## Full Project Documentation

---

## Table of Contents

1. [Complete Project Overview](#1-complete-project-overview)
2. [Full Technical Architecture](#2-full-technical-architecture)
3. [Full File & Folder Explanation](#3-full-file--folder-explanation)
4. [ESP Firmware Internals](#4-esp-firmware-internals)
5. [Web Dashboard Internals](#5-web-dashboard-internals)
6. [UI System](#6-ui-system)
7. [Security Architecture](#7-security-architecture)
8. [Environment Variables](#8-environment-variables)
9. [Deployment & Infrastructure](#9-deployment--infrastructure)
10. [Techniques & Engineering Decisions](#10-techniques--engineering-decisions)
11. [Known Issues & Future Improvements](#11-known-issues--future-improvements)

---

## 1. Complete Project Overview

### Purpose

Tarshid is a production-grade ESP32-based smart home automation system that provides local-first relay control, PIR motion sensor integration, timer-based automation, energy tracking, and optional cloud synchronization via Supabase. The system is designed to operate fully offline through a local Access Point (AP) with a responsive web dashboard, while also supporting hybrid online/offline modes when cloud credentials are configured.

### Architecture

The system follows a **three-tier architecture**:

1. **Firmware Layer (ESP32)** — Runs on the ESP32 microcontroller, handling GPIO control, WiFi management, local web server, WebSocket real-time communication, and optional cloud sync.
2. **Local Web Layer** — A self-contained single-page application (SPA) served from the ESP32's LittleFS filesystem, providing real-time relay control, timer management, sensor mapping, activity logging, and user administration.
3. **Cloud Layer (Optional)** — Supabase backend for remote monitoring, command dispatch, and state synchronization across multiple devices.

### Workflows

- **Boot Flow**: ESP32 initializes → scans for configured WiFi → connects to STA → validates internet → starts 30-second AP countdown → transitions to STA-only mode (online) or remains in AP mode (offline).
- **Control Flow**: User interacts with web UI → WebSocket command → ControlEngine processes → GPIO output changes → state broadcast to all connected clients.
- **Timer Flow**: User sets timer → firmware stores end epoch in NVS → ControlEngine evaluates on each tick → timer fires at target time → relay state changes → event logged.
- **Cloud Sync Flow**: ESP32 polls Supabase for commands → executes locally → pushes events back → state snapshot updated.

### System Goals

1. **Local-first operation** — All core functionality works without internet.
2. **Real-time responsiveness** — WebSocket-based live updates with sub-100ms latency.
3. **Flash wear minimization** — NVS writes are batched and infrequent; timer plans store end-epoch instead of counting down.
4. **Security by default** — MAC-based access control, SHA-256 password hashing, input validation, rate limiting.
5. **Production reliability** — Watchdog timers, health checks, automatic AP recovery, graceful degradation.

### Design Philosophy

- **Offline is the default**: The system is designed to be fully functional without any cloud dependency. Cloud sync is an additive layer.
- **State is persisted**: All user-configurable settings survive reboots via NVS (Preferences) and LittleFS.
- **Events are the source of truth**: Every state change produces a structured JSON event that flows through WebSocket, logs, and optionally cloud.
- **Defensive programming**: Every input is validated, every failure path is handled, and the system recovers automatically from transient faults.

---

## 2. Full Technical Architecture

### Frontend Architecture

The frontend is a **zero-dependency SPA** (no frameworks, no build step) served directly from the ESP32's LittleFS. It uses:

- **Vanilla JavaScript** with modular IIFE patterns (ThemeManager, I18N system).
- **CSS Custom Properties** (design tokens) for theming, dark mode, and responsive design.
- **WebSocket** for real-time bidirectional communication with the ESP32.
- **Fetch API** for initial state load, log history, and REST endpoints.
- **localStorage** for theme preference, language preference, and custom relay/sensor name overrides.

**Rendering Model**: The UI uses a **state-driven render** pattern. A single `state` object holds the authoritative system snapshot. When a `state_snapshot` WebSocket message arrives, the entire UI re-renders from this state. Individual events (relay changes, PIR motion, energy updates) trigger targeted partial updates without full re-render.

### Backend Architecture (ESP32 Firmware)

The firmware is structured as a **modular C++ system** with clearly separated concerns:

| Module | Responsibility |
|--------|---------------|
| `main.cpp` | Boot orchestration, WiFi/AP management, FreeRTOS task creation, watchdog, network mode state machine |
| `WebPortal` | HTTP server, WebSocket server, captive portal, route handlers, input validation, event queue |
| `ControlEngine` | Relay decision logic, timer evaluation, PIR input processing, night lock, energy tracking |
| `StorageLayer` | NVS persistence (Preferences), LittleFS file I/O, log management, user account storage |
| `TimeKeeper` | System clock management, NTP sync, user-device time sync, timezone handling, day/night phase |
| `CloudSyncService` | Supabase HTTP communication, event queue, command polling, state snapshot sync |
| `SystemTypes` | Shared data structures, enums, constants, PIR mapping bitmask utilities |
| `Config` | Compile-time configuration (pins, counts, ports, credentials, thresholds) |
| `Utils` | Enum-to-string conversion helpers |

### Communication Layers

1. **HTTP/REST** — Serves the web UI, handles API requests (`/api/state`, `/api/logs`, `/api/wifi/scan`, etc.), captive portal probes.
2. **WebSocket** — Real-time bidirectional channel for state snapshots, relay commands, timer operations, time sync, and event streaming.
3. **NVS (Preferences)** — Persistent key-value storage for relay modes, timer plans, user accounts, settings.
4. **LittleFS** — Filesystem for web UI assets, activity logs, pending event queue, cloud retry queue.
5. **Supabase HTTPS** — Optional cloud layer for remote state sync and command dispatch (TLS with pinned CA certificate).

### Realtime Systems

The firmware runs **three FreeRTOS tasks** pinned to specific cores:

| Task | Core | Priority | Period | Responsibility |
|------|------|----------|--------|----------------|
| `control_task` | Core 1 | 2 | 50ms | PIR reading, relay decision evaluation, output application, night lock |
| `network_task` | Core 0 | 1 | 10ms | HTTP server, WebSocket, WiFi health, AP countdown, NTP sync |
| `cloud_task` | Core 0 | 1 | 100ms | Supabase event push, state sync, command polling (only when configured) |

The main `loop()` is intentionally idle (`delay(1000)`), as all runtime behavior is owned by FreeRTOS tasks.

### Authentication Systems

**MAC-based Access Control**: Users are identified by their device's MAC address. The system supports:
- **Admin** — Full access including user management.
- **Manager** — Can add/remove users but not admin-only settings.
- **Restricted** — Limited to relay control and timer only.
- **Standard** — Full dashboard access but no user management.

Passwords are hashed with **SHA-256** (via mbedtls) before storage in NVS. Authentication is validated on every HTTP request and WebSocket connection.

### Database Schema

The Supabase database (`supabase/smart_home_schema.sql`) provides the cloud backend for ONLINE mode. It uses a **two-schema architecture** with `public` for application tables and `smart_home_private` for security-critical functions.

#### Tables

| Table | Purpose | Key Columns |
|-------|---------|-------------|
| `smart_home_profiles` | User profiles linked to `auth.users` | `id` (UUID), `username`, `display_name`, `global_role` (admin/user), `active` |
| `smart_home_devices` | Device registry with command tokens | `id` (text), `name`, `command_token_hash` (SHA-256), `active` |
| `smart_home_device_memberships` | User-to-device access control | `device_id`, `user_id`, `role` (admin/user), composite PK |
| `smart_home_device_states` | Current device state snapshot | `device_id` (PK), `updated_epoch`, `state` (JSONB) |
| `smart_home_device_events` | Event log for all device activity | `id` (bigserial), `device_id`, `event`, `event_ts`, `dedupe_key`, `payload` (JSONB) |
| `smart_home_remote_commands` | Command queue for ESP to claim | `id` (UUID), `device_id`, `user_id`, `status` (pending/processing/done/failed), `command` (JSONB), `result` (JSONB) |

#### Functions (smart_home_private schema)

| Function | Purpose |
|----------|---------|
| `touch_updated_at()` | Trigger function that updates `updated_at` on row modification |
| `current_user_is_global_admin()` | Checks if current user has `global_role = 'admin'` |
| `is_device_member(device_id)` | Checks if user is a member of the device (or global admin) |
| `is_device_admin(device_id)` | Checks if user has admin role on the device (or global admin) |
| `command_payload_shape_allowed(command)` | Validates command JSON structure against allowed shapes |
| `command_payload_allowed_for_user(device_id, command)` | Combines shape validation with role-based permission check |
| `auth_email_for_username(username)` | Resolves username to auth.users email for login |
| `device_token_ok(device_id, token)` | Validates device command token via SHA-256 comparison |
| `device_upsert_state(device_id, token, epoch, state)` | Device writes state snapshot (token-authenticated) |
| `device_insert_event(device_id, token, event, ts, dedupe_key, payload)` | Device inserts event with deduplication |
| `device_claim_commands(device_id, token, limit)` | Device claims pending commands (SELECT FOR UPDATE SKIP LOCKED) |
| `device_finish_command(device_id, token, command_id, status, ok, message, epoch)` | Device marks command as done/failed |

#### Public Wrapper Functions

All private functions have public wrappers in the `public` schema with `security invoker` that delegate to the private implementations. This provides a clean API surface while keeping implementation details isolated.

#### Row Level Security (RLS) Policies

| Table | Policy | Access |
|-------|--------|--------|
| `smart_home_profiles` | `smart_home_profiles_select` | Users can read themselves; global admins can read all |
| `smart_home_profiles` | `smart_home_profiles_update_self` | Users can update their own profile only |
| `smart_home_devices` | `smart_home_devices_select_member` | Visible to device members and global admins |
| `smart_home_device_memberships` | `memberships_select_member` | Visible to device members and global admins |
| `smart_home_device_memberships` | `memberships_admin_insert/update/delete` | Only global admins or device admins can modify |
| `smart_home_device_states` | `smart_home_device_states_select_member` | Visible to device members |
| `smart_home_device_events` | `smart_home_device_events_select_member` | Visible to device members |
| `smart_home_remote_commands` | `smart_home_remote_commands_select_member` | Visible to device members |
| `smart_home_remote_commands` | `smart_home_remote_commands_insert_member` | Users can create commands for devices they belong to (with payload validation) |

#### Command Validation

The `command_payload_shape_allowed()` function enforces strict JSON schema validation for each command type:

| Command Type | Required Keys | Validation |
|-------------|---------------|------------|
| `set_manual` | `channel`, `mode`, `type` | Mode must be ON/OFF/AUTO, channel must be numeric |
| `set_timer` | `channel`, `target`, `type` | Target must be ON/OFF, durationMinutes or durationSec required |
| `cancel_timer` | `channel`, `type` | Channel must be numeric |
| `set_energy_tracking` | `enabled`, `type` | Enabled must be boolean |
| `set_pir_mapping` | `mappings`, `type` | Mappings must be array |
| `set_rated_power` | `channel`, `powerW`, `type` | Channel numeric, powerW must be valid float |
| `reset_consumption` | `type` | No additional keys allowed |
| `get_state` | `type` | No additional keys allowed |

#### Realtime Publications

Three tables are added to the `supabase_realtime` publication for WebSocket-based live updates:
- `smart_home_device_states` — State changes broadcast to connected clients
- `smart_home_device_events` — New events streamed in real-time
- `smart_home_remote_commands` — Command status updates pushed to users

#### Device Authentication

Devices authenticate using a **SHA-256 hashed command token** stored in `smart_home_devices.command_token_hash`. The `device_token_ok()` function compares the provided token against the stored hash:

```sql
d.command_token_hash = encode(digest(convert_to(p_token, 'UTF8'), 'sha256'), 'hex')
```

This allows devices to authenticate without storing plaintext tokens in the database.

#### Privilege Model

- **`anon` role**: Can only call device-specific RPC functions (`device_upsert_state`, `device_insert_event`, `device_claim_commands`, `device_finish_command`, `auth_email_for_username`). Cannot read any tables directly.
- **`authenticated` role**: Can read profiles, devices, states, events. Can insert/select remote commands. Can manage memberships if device admin. All access is further restricted by RLS policies.
- **`smart_home_private` schema**: Revoked from public. Functions are granted execute permission selectively to `anon` and `authenticated` roles.

### Database Flow

```
User Action → WebSocket Command → WebPortal validates → ControlEngine executes
    → GPIO changes → StorageLayer persists → Event broadcast → WebSocket to all clients
    → (if online) CloudSyncService queues → Supabase HTTP POST
```

### Networking Flow

```
Boot → WiFi.scanNetworks() → Check configured SSID
    → If found: WIFI_AP_STA mode → Connect STA → Internet probe (DNS + HTTP, 3 retries)
    → If internet OK: 30s AP countdown → WIFI_STA only (online mode)
    → If internet FAIL: Stay in WIFI_AP_STA (offline mode with recovery)
    → Periodic health checks → Auto-reconnect on failure → AP recovery watchdog
```

---

## 3. Full File & Folder Explanation

### Root Directory

| File/Folder | Purpose |
|-------------|---------|
| `platformio.ini` | PlatformIO build configuration — defines ESP32 target, build flags, filesystem upload, and environment variable injection |
| `SmartHomeAutomation/` | ESP32 firmware project root |
| `online/` | Cloud-hosted web dashboard (GitHub Pages) for remote monitoring |
| `supabase/` | Supabase database schema SQL |
| `scripts/` | Build-time helper scripts (environment variable injection) |
| `.github/workflows/deploy.yml` | GitHub Actions CI/CD pipeline for online dashboard deployment |
| `.vscode/` | VS Code configuration (C/C++ properties, launch configs) |
| `.pio/` | PlatformIO build artifacts (auto-generated) |

### SmartHomeAutomation/src/

| File | Responsibility |
|------|---------------|
| `main.cpp` | **Entry point**. Boot sequence, WiFi/AP state machine, FreeRTOS task creation, watchdog initialization, system event broadcasting, network mode management, AP shutdown countdown, WiFi scanning, internet validation with retry. |
| `WebPortal.h` / `WebPortal.cpp` | **Web server layer**. HTTP route setup, WebSocket event handling, captive portal DNS, MAC authentication, command parsing, state snapshot broadcasting, event queue management, OTA endpoint gating, WiFi status/scan API endpoints. |
| `ControlEngine.h` / `ControlEngine.cpp` | **Control logic**. Relay decision evaluation (manual/timer/PIR priority), timer lifecycle, PIR input debouncing, night lock enforcement, energy tracking calculations, PIR mapping application, rated power management, consumption reset. |
| `StorageLayer.h` / `StorageLayer.cpp` | **Persistence layer**. NVS read/write via Preferences, LittleFS file I/O, log rotation/trimming, user account CRUD, activity log on/off toggle, inactive user removal, cloud queue management, factory reset, runtime WiFi credential storage (NVS `wifi_creds` namespace). |
| `TimeKeeper.h` / `TimeKeeper.cpp` | **Clock management**. NTP synchronization, user-device time sync, timezone offset handling, day/night phase calculation, epoch persistence, sync point management. |
| `CloudSyncService.h` / `CloudSyncService.cpp` | **Cloud integration**. Supabase REST API communication, event queue processing, state snapshot sync, remote command polling and execution, TLS certificate pinning, device ID generation, rate limiting. |
| `SystemTypes.h` | **Shared types**. Relay/PIR enums, runtime structs, user account struct, PIR mapping bitmask utilities, access control runtime struct. |
| `Config.h` | **Compile-time configuration**. Pin assignments, relay/PIR counts, port numbers, watchdog timeout, log limits, cloud credentials, access control settings, AP credentials, STA WiFi credentials. |
| `Utils.h` | **Utility functions**. Enum-to-string and string-to-enum conversions for relay modes, states, control sources, and day phases. |

### SmartHomeAutomation/data/

| File | Purpose |
|------|---------|
| `index.html` | **Primary offline dashboard**. Complete SPA with relay control, timer settings, power consumption, usage statistics, sensor mapping, sensor activity, activity logs, WiFi management, user administration, dark/light theme, English/Arabic i18n. |
| `restricted.html` | Restricted user page — relay and timer control only, no settings or user management. |
| `unauthorized.html` | Access denied page shown when MAC authentication fails. |

### online/

| File | Purpose |
|------|---------|
| `index.html` | Cloud-hosted dashboard for remote monitoring when ESP is in online mode. |
| `app.js` | Frontend application logic for cloud dashboard. |
| `config.js` / `config.example.js` | Supabase connection configuration. |
| `styles.css` | Cloud dashboard styles. |
| `admin.html` | Admin panel for cloud dashboard. |
| `simple.html` | Simplified view for quick status checks. |

### supabase/

| File | Purpose |
|------|---------|
| `smart_home_schema.sql` | Complete Supabase database schema — 6 tables, 15+ functions, RLS policies, realtime publications, and device token authentication. See [Database Schema](#database-schema) for full breakdown. |

### scripts/

| File | Purpose |
|------|---------|
| `inject_cloud_env.py` | Build-time script that injects Supabase credentials and WiFi STA credentials into `Config.h` via PlatformIO build flags. |

---

## 4. ESP Firmware Internals

### Boot Flow

```
1. Serial.begin(115200) — Initialize debug output
2. initRuntimeDefaults() — Zero all relay/PIR state vectors
3. xSemaphoreCreateMutex() — Allocate global state mutex
4. gStorage.begin() — Initialize LittleFS + Preferences
5. LittleFS.exists("/index.html") — Fail fast if web UI missing
6. gStorage.loadRuntime() — Restore persisted state from NVS
7. gStorage.loadUserAccounts() — Probe access control roster (diagnostic)
8. gTimeKeeper.begin() — Restore clock from NTP/user sync/NVS
9. initWatchdog() — Configure ESP-IDF task watchdog (12s timeout)
10. gControl.begin() — Configure GPIO pins, restore outputs
11. setupWiFi() — Scan networks, start AP, begin STA connection
12. gCloudSync.begin() — Initialize cloud sync if configured
13. gControl.setEventCallback() — Wire event routing
14. [If online available] Wait up to 12s for STA connection
15. [If connected] probeInternetAccessWithRetry() — DNS + HTTP, 3 retries
16. [If internet OK] Start AP shutdown countdown (30s)
17. [If internet FAIL] enterOfflineMode() — AP stays active
18. pushSystemEvent("system.boot") — Announce boot completion
19. gControl.refreshOutputs() — Apply persisted relay states
20. xTaskCreatePinnedToCore() — Spawn control_task (Core 1), network_task (Core 0), cloud_task (Core 0)
```

### Memory Usage

- **Heap**: ~200KB available after boot (varies with WebSocket client count).
- **Stack per task**: control_task = 8192 bytes, network_task = 12288 bytes, cloud_task = 12288 bytes.
- **NVS namespace**: `smart_home` for relay/timer/PIR state, `access_users` for user accounts, `wifi_creds` for runtime WiFi SSID/password.
- **LittleFS**: ~1.5MB available for web UI, logs, pending queue, cloud queue.
- **Queue sizes**: outbound event queue = 48 entries, inbound command queue = 24 entries, cloud event queue = 32 entries.

### WiFi Handling

**Connection Strategy**:
1. `WiFi.disconnect(true, true)` — Erase any previously saved WiFi driver credentials.
2. Check NVS `wifi_creds` namespace for runtime-saved credentials (takes priority over compile-time build flags).
3. `WiFi.scanNetworks()` — Verify configured SSID exists in range.
4. `WiFi.mode(WIFI_AP_STA)` — Start in dual mode (AP active + STA connecting).
5. `WiFi.begin(ssid, password)` — Begin connection attempt using runtime or compile-time credentials.
6. Wait up to 12 seconds for `WL_CONNECTED`.
7. `probeInternetAccessWithRetry()` — DNS resolution + HTTP request, up to 3 retries with 3-second delays.

**Runtime WiFi Credentials**:
- Saved via `POST /api/wifi/save` from the web UI WiFi Management panel.
- Stored in NVS namespace `wifi_creds` (keys: `sta_ssid`, `sta_pass`) — survives reboot and is independent of the `smart_home` preferences namespace.
- Takes priority over compile-time `WIFI_STA_SSID`/`WIFI_STA_PASSWORD` build flags at boot and during reconnection.
- When saved from the web UI, the ESP disconnects, clears the WiFi driver cache, and reconnects with the new credentials immediately (no reboot required).
- All reconnection paths (`maintainWiFi()`, `enterOfflineMode()`, `enterOnlineMode()`, AP recovery) resolve credentials through `getStaCredentials()` which checks NVS first.

**Internet Validation**:
- **Stage 1 (DNS)**: `WiFi.hostByName("pool.ntp.org", resolved)` — Fast, lightweight DNS resolution check.
- **Stage 2 (HTTP)**: `WiFiClient.connect("httpbin.org", 80)` → `GET /status/200` → Verify `HTTP/1.0 200` response line.
- **Retry logic**: Up to 3 attempts with 3-second delays between failures.
- **Timeout**: 5-second connect timeout, 5-second response wait.

**Reconnection**:
- `WiFi.setAutoReconnect(true)` — Built-in auto-reconnect.
- `maintainWiFi()` — Every 10 seconds in offline mode, every 10 seconds in online mode.
- If disconnected: `WiFi.reconnect()` → fallback to `WiFi.begin()` if still down.
- AP recovery watchdog: If AP IP disappears, restart SoftAP automatically.

### AP Mode Logic

**Startup**:
- AP SSID: `tarshid` (hidden), Password: `12345678`.
- `AP_HIDDEN = true` — SSID is not broadcast.
- `AP_MAX_CONNECTIONS = 4` — Maximum concurrent clients.
- `AP_CHANNEL = 1` — Fixed channel.
- AP isolation enabled via `esp_wifi_set_config()` (framework-dependent).

**30-Second Delayed Shutdown**:
- After successful STA connection + internet verification, a 30-second countdown begins.
- During countdown: AP remains active, WebSocket events broadcast remaining seconds.
- After countdown: `WiFi.softAPdisconnect(true)` → `WiFi.mode(WIFI_STA)` → AP fully disabled.
- If WiFi disconnects during countdown: countdown is cancelled, AP stays active.

**Recovery**:
- If AP IP disappears: `startWiFiApRecovery()` → `WiFi.mode(WIFI_OFF)` → wait 300ms → restart AP.
- Recovery state machine: `IDLE` → `WAITING_FOR_WIFI_OFF` → `WAITING_FOR_AP_READY` → `IDLE`.
- Cooldown: 20 seconds between recovery attempts.
- Health check interval: 5 seconds, failure threshold: 2 consecutive failures.

### STA Mode Logic

- In online mode: `WiFi.mode(WIFI_STA)` — AP is completely disabled.
- Periodic internet probe every 30 seconds.
- After 3 consecutive failures: fall back to offline AP mode.
- Reconnection every 10 seconds if disconnected.

### Retry Systems

| System | Retry Count | Delay | Trigger |
|--------|------------|-------|---------|
| Internet probe | 3 | 3000ms | Boot + periodic 30s check |
| STA reconnection | Continuous | 10000ms | Disconnection detected |
| AP recovery | Continuous | 20000ms cooldown | AP IP missing |
| NTP sync | Continuous | 45000ms | No valid time |
| Cloud event push | Queue-based | 5000ms flush | Network ready |

### Watchdog/Recovery Behavior

- **ESP-IDF Task Watchdog**: 12-second timeout, triggers panic if any task fails to reset.
- **All three tasks** call `esp_task_wdt_reset()` every cycle.
- **AP Recovery Watchdog**: Independent software watchdog in `checkWiFiHealth()` — detects AP failure and triggers recovery state machine.
- **Mutex protection**: All shared state access is guarded by `gStateMutex` (ControlEngine) and `ioMutex_` (StorageLayer).

### OTA Flow

- OTA endpoints (`/update`, `/ota`, `/api/update`, `/firmware`) are **disabled** in production.
- All OTA paths return `403` with `{"ok":false,"msg":"OTA disabled"}`.
- Firmware signature verification (HMAC-SHA256) is implemented but unused since OTA is disabled.

### Async Handling

- **FreeRTOS tasks** own all runtime behavior — the Arduino `loop()` is idle.
- **Queue-based communication**: WebPortal uses FreeRTOS queues for inbound commands and outbound events, decoupling HTTP/WebSocket handling from command execution.
- **Non-blocking design**: No `delay()` in task loops (except the intentional boot wait). All timing uses `millis()` comparisons.

### Task Scheduling

```
control_task (Core 1, priority 2, 50ms):
  gControl.tickFast() → PIR read → relay evaluate → output apply → WDT reset

network_task (Core 0, priority 1, 10ms):
  maintainNetworkMode() → gWebPortal.loop() → maintainWiFi() → checkWiFiHealth()
  → processApShutdownCountdown() → NTP sync → housekeeping → WDT reset

cloud_task (Core 0, priority 1, 100ms):
  gCloudSync.loop() → event push → state sync → command poll → WDT reset
```

---

## 5. Web Dashboard Internals

### Frontend Rendering Flow

1. **Bootstrap** (`window.onload`):
   - Initialize theme manager (system preference → stored → default).
   - Apply language (stored → English default).
   - Check MAC authentication status (`/api/auth/status`).
   - Sync device time (`/setTime` with browser clock).
   - Load initial state (`/api/state`).
   - Load activity logs (`/api/logs?limit=90`).
   - Connect WebSocket (port 81).
   - Start periodic relay re-render (1 second).
   - Start periodic WiFi status refresh (3 seconds).

2. **State-Driven Render**:
   - `state` object holds the authoritative system snapshot.
   - `renderState()` calls: `renderHeader()`, `renderRelays()`, `renderPirMapping()`, `renderSensorIndicators()`, `renderPower()`, `renderStats()`.
   - Each renderer generates HTML from `state` data and injects via `innerHTML`.

3. **WebSocket Message Handling**:
   - `state_snapshot` → Full state replacement → `renderState()`.
   - `command_ack` → Toast notification.
   - `time_request` → Trigger time sync.
   - `name_update` → Cross-client name synchronization.
   - Event types (`ON`, `OFF`, `TIMER`, `ERROR`) → Targeted updates + log entry.
   - `pir.motion` / `pir.idle` → Sensor indicator flash.
   - `energy_update` → Power panel partial update.
   - `ap.countdown` / `ap.countdown_start` / `ap.shutdown` → AP countdown banner.

### Routing

The frontend is a **single-page application** with no client-side routing. Navigation is via anchor links (`#relaysPanel`, `#timerPanel`, etc.) that scroll to sections. Panels are always visible; there are no hidden/conditional routes.

### State Management

- **Single source of truth**: The `state` object, populated by `/api/state` and `state_snapshot` WebSocket messages.
- **No reactive framework**: State changes trigger explicit render calls.
- **localStorage** for: theme, language, custom relay names, custom PIR names.
- **serverTimeOffsetSec** for clock synchronization between browser and ESP.

### API Communication

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/api/state` | GET | Full system state snapshot |
| `/api/logs` | GET | Recent activity logs (limit param) |
| `/api/time` | GET | ESP current time and validity |
| `/api/pirMapping` | POST | Save PIR-to-relay mapping |
| `/api/resetConsumption` | POST | Reset energy counters |
| `/api/ratedPower` | POST | Set relay rated power |
| `/api/wifi/status` | GET | WiFi connection + AP + countdown status |
| `/api/wifi/scan` | POST | Scan nearby WiFi networks |
| `/api/wifi/save` | POST | Save WiFi credentials to NVS and reconnect |
| `/setTime` | POST | Sync ESP clock from browser |
| `/api/auth/status` | GET | Current MAC authentication status |
| `/api/auth/login` | POST | MAC-based authentication |
| `/api/auth/addUser` | POST | Add new user (manager permission) |
| `/api/auth/users` | GET | List all users (manager permission) |
| `/api/auth/removeUser` | POST | Remove user (manager permission) |
| `/api/auth/disableLogs` | POST | Disable activity logging (admin) |
| `/api/auth/removeInactiveUsers` | POST | Remove inactive users (admin) |

### Authentication Flow

1. Browser connects to ESP AP → loads `index.html`.
2. `checkAuthStatus()` calls `/api/auth/status` with MAC from HTTP headers.
3. If not authenticated → redirect to `/unauthorized.html`.
4. If restricted → redirect to `/restricted.html`.
5. If authenticated → apply role-based UI visibility (`applyRoleVisibility()`).
6. WebSocket connections also validate MAC on connect.

### Admin Systems

- **User Management**: Add/remove users with MAC address, name, password, and role flags (admin, manage users, restricted).
- **Storage Management**: Disable activity logging, remove inactive users when roster is full.
- **Role-Based Visibility**: UI sections are shown/hidden based on user permissions.

### Device Management Logic

- **Relay Control**: Manual ON/OFF/AUTO via WebSocket `set_manual` command.
- **Timer Management**: Set/cancel timers via WebSocket `set_timer`/`cancel_timer`.
- **PIR Mapping**: Configure which relays each PIR sensor controls via REST API.
- **Energy Tracking**: Enable/disable timer-only energy estimation via WebSocket.
- **Rated Power**: Set per-relay rated wattage for energy calculations via REST API.

### Realtime Updates

- **WebSocket** is the primary realtime channel (port 81).
- **Reconnection**: Exponential backoff (1.8s → 3.6s → 7.2s → max 10s).
- **State reconciliation**: On reconnect, request full state snapshot.
- **Event buffering**: Events are queued and flushed when clients reconnect.

---

## 6. UI System

### Design System

The UI uses a **token-based design system** defined in CSS custom properties:

| Token Category | Examples |
|---------------|----------|
| Colors | `--bg`, `--surface`, `--accent`, `--on`, `--off`, `--warn`, `--error` |
| Shadows | `--shadow-xs`, `--shadow-sm`, `--shadow-md`, `--shadow-lg` |
| Radii | `--radius-xs` (8px), `--radius` (14px), `--radius-xl` (22px), `--radius-pill` (999px) |
| Typography | Inter/SF Pro Text/Segoe UI system font stack |

**Dark Mode**: Complete token override via `[data-theme="dark"]` selector. All colors, shadows, and borders are redefined for dark backgrounds.

### Spacing System

- **clamp()** for responsive padding: `clamp(16px, 2.4vw, 32px)`.
- **Consistent gaps**: 14px for grids, 10px for flex rows, 22px for panel spacing.
- **Touch targets**: Minimum 44px height for all interactive elements.

### Typography

- **Scale**: 0.66rem (badges) → 0.72rem (labels) → 0.86rem (body) → 1.1rem (panel titles) → 1.15rem (brand).
- **Weight**: 700 (regular text), 800 (headings/labels), 900 (footer logo).
- **Letter spacing**: -0.02em (brand), 0.1em-0.16em (uppercase labels).

### Reusable Components

| Component | Usage |
|-----------|-------|
| `.panel` | Content sections with icon, title, description, and tag |
| `.status-card` | Top bar status indicators with icon and pill |
| `.pill` | Status badges (good/warn variants with animated dots) |
| `.detail-chip` | Key-value display pairs in grids |
| `.toggle` | iOS-style checkbox switches |
| `.icon-btn` | Icon + text button with hover effects |
| `.advanced` | Collapsible disclosure sections |
| `.advanced-overlay` | Modal overlay for advanced details |
| `.toast` | Fixed-position notification banner |
| `.nav-chip` | Quick navigation pill buttons |

### Responsiveness

Three breakpoints:
- **1180px**: 3-column status grid, single-column work grid.
- **820px**: 2-column status grid, single-column sensor grid, stacked footer.
- **520px**: 1-column status grid, reduced panel padding, full-width relay buttons.

### Animations

- **Pulse dots**: `pulse-good` (green) and `pulse-warn` (amber) for status indicators.
- **Sensor pulse**: Radial gradient animation on active PIR cards.
- **Panel entrance**: `panelIn` keyframe for modal overlays.
- **Timer ring**: SVG stroke-dashoffset transition for countdown visualization.
- **Hover effects**: `translateY(-1px)` + shadow on cards and buttons.
- **Reduced motion**: `@media (prefers-reduced-motion: reduce)` disables all animations.

### Accessibility

- **ARIA labels** on all interactive elements.
- **Focus-visible** outlines (3px solid, 2px offset).
- **Semantic HTML**: `<main>`, `<header>`, `<nav>`, `<section>`, `<article>`, `<footer>`.
- **RTL support**: `html[dir="rtl"]` selectors for Arabic layout mirroring.
- **Touch action**: `touch-action: manipulation` on buttons to eliminate 300ms tap delay.

---

## 7. Security Architecture

### API Security

- **Input validation**: Every HTTP and WebSocket payload is validated for allowed keys, value types, and format.
- **Blocked tokens**: `&&`, `||`, `;`, `` ` ``, `\r`, `\n`, `\0` are rejected in all input.
- **Key whitelisting**: JSON payloads must contain only pre-approved keys (`hasOnlyAllowedKeys()`).
- **Query parameter validation**: URL parameters are validated against allowed lists.

### Authentication Protection

- **MAC-based access**: Clients are identified by their MAC address (extracted from HTTP headers or WebSocket connection info).
- **SHA-256 password hashing**: Passwords are hashed before NVS storage (64 hex characters).
- **Role-based permissions**: Admin, manager, restricted, and standard roles with granular access control.
- **Self-removal prevention**: Users cannot delete their own accounts.
- **Last-admin protection**: The last admin account cannot be removed.
- **Re-authentication**: Sensitive operations (add user, remove user, disable logs) require password re-entry.

### Environment Variable Handling

- **Compile-time injection**: WiFi credentials and Supabase keys are injected via PlatformIO build flags, never hardcoded.
- **Build script**: `scripts/inject_cloud_env.py` reads environment variables and generates build flags.
- **Fallback defaults**: Blank defaults for all credentials — system operates in offline mode if none are provided.

### Credential Storage

- **NVS encryption**: ESP32 NVS provides basic flash-level protection.
- **Password hashes only**: Plaintext passwords are never stored.
- **MAC normalization**: All MAC addresses are uppercased before storage and comparison.
- **Legacy hash detection**: Truncated hashes from pre-fix firmware trigger automatic roster reset.

### ESP Security Considerations

- **AP isolation**: Client-to-client traffic is blocked at the driver level (framework-dependent).
- **Hidden SSID**: AP SSID is not broadcast.
- **Captive portal**: DNS server redirects all domains to the local portal.
- **OTA disabled**: All firmware update endpoints return 403.
- **Watchdog**: 12-second task watchdog prevents hangs.
- **Input size limits**: HTTP body limited to 512 bytes, WebSocket payload limited to 384 bytes.

### Secure Communication

- **HTTPS for cloud**: Supabase communication uses `WiFiClientSecure` with pinned ISRG Root X1 CA certificate.
- **No setInsecure()**: Certificate pinning prevents MITM attacks on cloud communication.
- **Local HTTP only**: Web dashboard is served over plain HTTP (local network only).
- **WebSocket**: No encryption on local WebSocket (same network trust model).

### Database Security

- **Row Level Security (RLS)**: Every table has RLS enabled. Policies restrict access based on device membership and user roles.
- **Two-schema isolation**: Security-critical functions live in `smart_home_private` schema, revoked from public. Public wrappers use `security invoker` to delegate safely.
- **Command token authentication**: Devices authenticate via SHA-256 hashed tokens, not passwords. Tokens are validated in `device_token_ok()` before any state/command operation.
- **Command payload validation**: `command_payload_shape_allowed()` enforces strict JSON schema — only allowed keys, valid types, and correct value ranges are accepted.
- **Role-based access**: `global_role` (admin/user) on profiles, `role` (admin/user) on device memberships. Global admins bypass all device-level checks.
- **Deduplication**: `smart_home_device_events.dedupe_key` has a unique constraint with `ON CONFLICT DO NOTHING` to prevent duplicate event ingestion.
- **SKIP LOCKED for command claiming**: `device_claim_commands()` uses `SELECT FOR UPDATE SKIP LOCKED` to safely distribute commands across multiple device instances without race conditions.
- **Replica identity**: State, events, and commands tables use `REPLICA IDENTITY FULL` for reliable realtime replication.
- **Privilege minimization**: `anon` role can only call device RPC functions. `authenticated` role has SELECT on most tables but INSERT only on commands and memberships (with RLS checks).

### Validation Mechanisms

- **Relay command rate limiting**: 120ms minimum between commands on the same relay.
- **Cloud command rate limiting**: 250ms minimum between cloud relay commands.
- **Night Lock**: All configuration changes blocked during night hours (18:00–06:00).
- **Timer validation**: Duration, target state, and manual mode requirements enforced.
- **PIR mapping validation**: Exact count match required (one entry per PIR).

---

## 8. Environment Variables

### Build-Time Variables (PlatformIO)

| Variable | Source | Used In | Required | Security Impact |
|----------|--------|---------|----------|-----------------|
| `WIFI_STA_SSID` | `.env` or platformio.ini | `Config.h` → `STA_SSID` | Optional | WiFi network name — runtime-saved NVS credentials take priority; if blank and no NVS creds, offline-only mode |
| `WIFI_STA_PASSWORD` | `.env` or platformio.ini | `Config.h` → `STA_PASSWORD` | Optional | WiFi password — runtime-saved NVS credentials take priority; exposed in firmware binary |
| `CLOUD_SYNC_ENABLED` | `.env` or platformio.ini | `Config.h` | Optional | Enables/disables all cloud code (0 or 1) |
| `SUPABASE_URL` | `.env` or platformio.ini | `CloudSyncService.cpp` | Optional (required if cloud enabled) | Supabase project URL |
| `SUPABASE_PUBLISHABLE_KEY` | `.env` or platformio.ini | `CloudSyncService.cpp` | Optional (required if cloud enabled) | Supabase API key — exposed in firmware binary |
| `CLOUD_DEVICE_ID` | `.env` or platformio.ini | `CloudSyncService.cpp` | Optional | Device identifier — defaults to MAC-based ID |
| `CLOUD_COMMAND_TOKEN` | `.env` or platformio.ini | `CloudSyncService.cpp` | Optional (required if cloud enabled) | Shared secret for RPC authentication |

### Compile-Time Constants (Config.h)

| Constant | Default | Purpose |
|----------|---------|---------|
| `AP_SSID` | `"tarshid"` | Access Point network name |
| `AP_PASSWORD` | `"12345678"` | Access Point password |
| `ENABLE_ACCESS_CONTROL` | `true` | Enable MAC-based authentication |
| `RELAY_ACTIVE_LOW` | `true` | Relay board polarity (active-low = common for ESP32 boards) |
| `RELAY_CONFIG[]` | Pins 26, 27 | Relay pin assignments and names |
| `PIR_CONFIG[]` | Pins 32, 33, 25 | PIR sensor pin assignments |
| `HTTP_PORT` | `80` | Web server port |
| `WS_PORT` | `81` | WebSocket server port |
| `WATCHDOG_TIMEOUT_SECONDS` | `12` | Task watchdog timeout |
| `MAX_USER_ACCOUNTS` | `16` | Maximum users in access control roster |
| `LOG_MAX_BYTES` | `120 KB` | Activity log file size limit |
| `PENDING_MAX_BYTES` | `48 KB` | Pending event queue file size limit |

---

## 9. Deployment & Infrastructure

### Local Development Setup

1. **Install PlatformIO**: `pip install platformio` or use VS Code PlatformIO extension.
2. **Clone repository**: `git clone <repo-url>`.
3. **Configure credentials** (optional):
   - Create `.env` file with `WIFI_STA_SSID`, `WIFI_STA_PASSWORD`, Supabase credentials.
   - Or edit `platformio.ini` `build_flags` directly.
4. **Build firmware**: `pio run -e esp32dev`.
5. **Upload filesystem**: `pio run -e esp32dev --target uploadfs` (uploads `data/` to LittleFS).
6. **Flash firmware**: `pio run -e esp32dev --target upload`.

### Build Process

```
platformio.ini → build_flags → inject_cloud_env.py → Config.h defines
    → PlatformIO compiler → ESP-IDF framework → firmware.bin
data/ → mkspiffs/mklittlefs → spiffs.bin → uploaded to LittleFS partition
```

### Deployment Workflow

1. **Code changes** → commit to `main` branch.
2. **GitHub Actions** (`deploy.yml`) → builds and deploys `online/` to GitHub Pages.
3. **Firmware updates** → manual flash via PlatformIO or serial.
4. **Filesystem updates** → `pio run --target uploadfs` to update web UI on ESP.

### GitHub Pages Setup

- The `online/` directory is deployed as a static site.
- GitHub Actions workflow triggers on push to `main`.
- `config.js` contains Supabase connection details (separate from firmware credentials).

### Firmware Flashing

1. Connect ESP32 via USB.
2. `pio run --target upload` — flashes firmware.
3. `pio run --target uploadfs` — flashes LittleFS filesystem.
4. Monitor via Serial at 115200 baud.

### Production Deployment Flow

1. Set environment variables in CI/CD or `.env` (optional — runtime credentials can be set via web UI instead).
2. Build firmware with production credentials.
3. Flash to ESP32 devices.
4. Verify AP starts with correct SSID.
5. Connect to AP, access dashboard at `http://192.168.4.1`.
6. Configure WiFi via the WiFi Management panel (Scan → Select → Enter password → Save & Reconnect) — credentials are saved to NVS and applied immediately without reboot.
7. If cloud configured: verify Supabase state sync.

### Database Deployment

1. **Create Supabase project**: Set up a new project at https://supabase.com.
2. **Run schema migration**: Execute `supabase/smart_home_schema.sql` in the Supabase SQL editor or via Supabase CLI (`supabase db push`).
3. **Create initial device**: Insert a row into `smart_home_devices` with a unique `id` and a SHA-256 hashed `command_token_hash`:
   ```sql
   INSERT INTO smart_home_devices (id, name, command_token_hash)
   VALUES ('esp32-001', 'Living Room ESP', encode(digest('your-secret-token', 'sha256'), 'hex'));
   ```
4. **Create user profiles**: Link Supabase Auth users to `smart_home_profiles`:
   ```sql
   INSERT INTO smart_home_profiles (id, username, display_name, global_role)
   VALUES ('<auth-user-uuid>', 'admin', 'Admin User', 'admin');
   ```
5. **Add device memberships**: Grant users access to devices:
   ```sql
   INSERT INTO smart_home_device_memberships (device_id, user_id, role)
   VALUES ('esp32-001', '<auth-user-uuid>', 'admin');
   ```
6. **Configure firmware**: Set `SUPABASE_URL`, `SUPABASE_PUBLISHABLE_KEY`, `CLOUD_DEVICE_ID`, and `CLOUD_COMMAND_TOKEN` in `.env` or `platformio.ini`.
7. **Enable realtime**: Verify `supabase_realtime` publication includes all three tables (handled automatically by the schema).
8. **Test cloud sync**: Flash firmware, verify ESP connects to Supabase and begins polling commands.

---

## 10. Techniques & Engineering Decisions

### Architecture Patterns

- **Modular separation**: Each concern (control, storage, web, time, cloud) is in its own module with clear interfaces.
- **Event-driven design**: All state changes produce structured JSON events that flow through the system.
- **State machine for network modes**: `UNKNOWN` → `OFFLINE_LOCAL` / `ONLINE_CLOUD` with explicit transitions.
- **Queue-based decoupling**: WebPortal uses FreeRTOS queues to decouple HTTP/WebSocket handling from command execution.

### Optimization Techniques

- **Timer end-epoch storage**: Instead of counting down (which requires frequent writes), the firmware stores the target end epoch and compares against current time.
- **Batched stats persistence**: ON-duration stats are persisted every 300 seconds instead of on every change.
- **Selective log flushing**: PIR motion/idle events are excluded from flash persistence to save space.
- **Ring buffer for logs**: Recent logs are kept in a sliding window, oldest entries trimmed when size limit reached.
- **Lazy state broadcast**: State snapshots are only sent when state changes, not on a timer.

### Async Handling

- **FreeRTOS tasks** replace Arduino `loop()` for all time-critical operations.
- **Non-blocking timing**: All delays use `millis()` comparisons, never `delay()` in task loops.
- **Queue-based IPC**: Commands and events flow through FreeRTOS queues with zero-copy where possible.

### Component Reuse

- **SVG icon sprite**: All icons defined once in `<svg><defs>`, referenced via `<use href="#i-xxx">`.
- **CSS design tokens**: All colors, shadows, and radii defined as custom properties for consistent theming.
- **I18N dictionary**: Single source of truth for all translatable strings, with template variable substitution.
- **Renderer pattern**: Each UI section has its own render function that generates HTML from state data.

### Caching

- **localStorage** for theme, language, and custom names — survives page reloads without server round-trip.
- **usersCache** in frontend for duplicate MAC check before API call.
- **accessControl_** cache in WebPortal for fast MAC validation without NVS reads on every request.

### Reconnection Strategies

- **Exponential backoff**: WebSocket reconnection starts at 1.8s, doubles each attempt, caps at 10s.
- **State reconciliation**: On reconnect, full state snapshot is requested to ensure UI consistency.
- **Event buffering**: Pending events are stored in LittleFS and flushed when clients reconnect.
- **Visibility API**: WebSocket reconnects when browser tab becomes visible.

### Error Handling

- **Fail fast**: Missing `/index.html` halts boot with infinite loop.
- **Mutex failure**: If mutex allocation fails, boot halts immediately.
- **Storage failure**: If LittleFS or Preferences fails to initialize, boot halts.
- **Graceful degradation**: Cloud sync failure does not affect local operation.
- **Input validation**: Every API endpoint validates input before processing.

### Responsive Techniques

- **CSS Grid** with `minmax()` for fluid layouts.
- **clamp()** for responsive spacing without media queries.
- **Three breakpoints** (1180px, 820px, 520px) for progressive layout simplification.
- **Touch-optimized**: 44px minimum touch targets, `touch-action: manipulation` on buttons.

### Scalability Considerations

- **Configurable relay/PIR counts**: All logic uses `RELAY_COUNT` and `PIR_COUNT` — changing config arrays automatically scales the system.
- **Bitmask PIR mapping**: 64-bit relay mask supports up to 64 relays per PIR sensor.
- **Bounded queues**: All queues have fixed maximum sizes to prevent memory exhaustion.
- **File size limits**: Logs and pending queues are trimmed to configured maximums.

---

## 11. Known Issues & Future Improvements

### Current Limitations

1. **No WPA3 support**: ESP32 Arduino framework limits WiFi security to WPA2.
2. **Single AP channel**: AP is fixed to channel 1, which may cause interference in dense environments.
3. **No mDNS/Bonjour**: ESP is not discoverable by hostname on the local network.
4. **No HTTPS for local dashboard**: Web UI is served over plain HTTP (acceptable for local network but not for exposed networks).
5. **Energy tracking is estimation-only**: Based on rated power × duration, not actual current measurement.

### Technical Debt

1. **WebPortal.cpp is large** (~1700 lines): Route handlers could be extracted into separate files.
2. **Duplicate validation logic**: Input validation patterns are repeated across multiple endpoints.
3. **Static UserAccount in findUserByMac()**: Returns a pointer to a static buffer, which is not thread-safe.
4. **Magic numbers in timer validation**: `1700000000ULL` epoch threshold is hardcoded in multiple places.

### Possible Optimizations

1. **WebSocket compression**: Enable per-message deflate to reduce bandwidth.
2. **HTTP caching headers**: Add `Cache-Control` for static assets to reduce load times.
3. **Lazy panel loading**: Render panels on-demand instead of all at once on boot.
4. **Delta state updates**: Instead of full state snapshots, send only changed fields.
5. **OTA re-enablement**: With proper authentication and signature verification, OTA could be safely enabled for remote firmware updates.

### Future Feature Ideas

1. **Multi-device sync**: Coordinate multiple ESP32 devices through the cloud layer.
3. **Scene/automation engine**: User-defined rules (e.g., "if PIR A triggers at night, turn on Relay B for 5 minutes").
4. **Current sensing integration**: Add ACS712 or similar for real power measurement.
5. **MQTT support**: Alternative to Supabase for local network IoT integration.
6. **Voice assistant integration**: Home Assistant or Google Home compatibility.
7. **Over-the-air filesystem updates**: Update web UI without USB connection.
8. **Multi-language expansion**: Add more languages to the I18N dictionary.
9. **Dashboard customization**: User-configurable panel order and visibility.
10. **Historical data charts**: Graph energy consumption and relay usage over time.
11. **Database migration system**: Use Supabase CLI migrations instead of a single monolithic SQL file for version-controlled schema changes.
12. **Event archival policy**: Add a database function to automatically archive or delete events older than N days to control storage costs.
