# System Architecture

## Purpose

Tarshid Smart Home has two separate operating modes:

1. OFFLINE MODE / ESP Local Mode
2. ONLINE MODE / Server + Internet Mode

The ESP32 is always the physical authority for relays, PIR sensors, timers, Night Lock, and persisted local state. Online mode adds server authentication, remote commands, realtime state synchronization, and NTP-backed time. It does not replace the firmware control engine.

## Architecture Boundary

### Offline Mode

Offline mode is used only when the ESP32 does not have a usable internet/server path. In this mode the ESP32 runs its local access point, captive portal, local HTML files, local WebSocket, local HTTP APIs, and MAC-address authentication.

Offline mode uses only ESP flash/NVS/LittleFS for credentials and settings. It does not use Supabase, GitHub Pages, server username/password accounts, or internet APIs.

### Online Mode

Online mode is used only when the ESP32 connects to configured STA WiFi and confirms internet reachability. In this mode the ESP32 disables AP mode, stops the offline web server, stops captive DNS, stops offline WebSocket service, and communicates with Supabase as an online device node.

Online mode uses Supabase Auth/database for browser login and role resolution. The ESP sends state/events to Supabase and claims pending command rows from Supabase through token-checked RPC functions.

## Runtime Mode Rules

### Enter Offline Mode

`main.cpp` enters offline mode when one of these is true:

- STA WiFi credentials are not compiled.
- Supabase/cloud settings are incomplete.
- STA WiFi cannot connect.
- DNS/internet probe fails.
- Online mode later records repeated reachability failures.

Offline entry starts:

- `WiFi.mode(WIFI_AP)` or `WIFI_AP_STA`
- SoftAP using `AP_SSID` and `AP_PASSWORD` from `Config.h`
- Captive DNS
- `WebPortal::begin()`
- Local HTTP routes and WebSocket
- ESP-local MAC authentication

### Enter Online Mode

`main.cpp` enters online mode only when all of these are true:

- STA WiFi credentials are compiled.
- `CLOUD_SYNC_ENABLED` is true.
- `SUPABASE_URL`, a public Supabase key, and `CLOUD_COMMAND_TOKEN` are compiled.
- STA WiFi is connected.
- Internet probe succeeds.

Online entry performs:

- `gWebPortal.end()`
- `WiFi.softAPdisconnect(true)`
- `WiFi.mode(WIFI_STA)`
- forced NTP sync with `gTimeKeeper.trySyncFromNtp(true)`
- cloud state sync through `CloudSyncService`

In online mode, `networkTask` does not run the offline portal loop and `cloudTask` is the only server communication path.

## Firmware Responsibilities

### `SmartHomeAutomation/src/main.cpp`

- Initializes storage, runtime state, time, control engine, web portal, and cloud sync.
- Selects offline or online mode.
- Enforces AP shutdown in online mode.
- Starts FreeRTOS tasks.
- Calls offline portal loop only in offline mode.
- Calls cloud loop only in online mode.

### `SmartHomeAutomation/src/ControlEngine.cpp`

- Owns relay state and actuation.
- Enforces `MANUAL > TIMER > PIR` priority.
- Starts, cancels, and expires timers.
- Applies PIR-to-relay mapping.
- Enforces Night Lock.
- Tracks timer statistics and energy values.
- Builds the JSON state snapshot shared by offline and online UIs.

### `SmartHomeAutomation/src/WebPortal.cpp`

Offline-only web layer:

- Serves LittleFS pages from `SmartHomeAutomation/data`.
- Runs captive DNS.
- Runs HTTP routes and WebSocket.
- Preserves the existing MAC registration/authentication behavior.
- Handles offline admin/user/restricted access.
- Sends validated commands to `ControlEngine`.

`WebPortal::end()` stops DNS, WebSocket clients, HTTP server, and client counters so offline pages are not served during online mode.

### `SmartHomeAutomation/src/StorageLayer.cpp`

- Stores offline MAC user records in NVS key `users_json`.
- Stores local relay/timer/PIR settings in NVS.
- Stores local log/queue files in LittleFS.
- Does not store online username/password credentials.
- Does not replace Supabase Auth.

### `SmartHomeAutomation/src/CloudSyncService.cpp`

Online-only cloud layer:

- Sends state to `device_upsert_state()`.
- Sends events to `device_insert_event()`.
- Claims commands from `device_claim_commands()`.
- Finishes commands through `device_finish_command()`.
- Uses the compiled device token, never browser credentials.
- Applies commands by calling `ControlEngine`, not by duplicating relay logic.

### `SmartHomeAutomation/src/TimeKeeper.cpp`

- Supports local/browser time in offline mode.
- Forces NTP sync when entering online mode.
- Lets online timer commands seed the clock from internet/NTP epoch values.

## File Separation

### Uploaded To ESP LittleFS

- `SmartHomeAutomation/data/index.html`
- `SmartHomeAutomation/data/restricted.html`
- `SmartHomeAutomation/data/unauthorized.html`

These files are offline-only and must not be used by GitHub Pages.

### Compiled/Flashed To ESP Firmware

- `SmartHomeAutomation/src/*.cpp`
- `SmartHomeAutomation/src/*.h`
- `platformio.ini`
- `scripts/inject_cloud_env.py`

The script injects build-time WiFi/Supabase/device-token defines into firmware.

### Online-Only Static Frontend

- `online/index.html`
- `online/admin.html`
- `online/simple.html`
- `online/app.js`
- `online/styles.css`
- `online/config.example.js`
- `online/config.js`
- `.github/workflows/deploy-online.yml`

These files are for static hosting, including GitHub Pages. They are not uploaded to ESP LittleFS.

### Online Server/Database

- `supabase/smart_home_schema.sql`

This file creates the Supabase Auth/profile/device/command/state/event architecture. It is not flashed to ESP.

## Supabase Database Architecture

Public Data API tables:

- `smart_home_profiles`
- `smart_home_devices`
- `smart_home_device_memberships`
- `smart_home_device_states`
- `smart_home_device_events`
- `smart_home_remote_commands`

Private server logic:

- schema `smart_home_private`
- security-definer helper/RPC implementations
- token checks
- role checks
- command validation

Public RPC wrappers keep firmware/frontend compatibility while privileged logic stays outside the exposed `public` schema.

## Authentication Separation

Offline authentication:

- MAC address based.
- Stored in ESP NVS only.
- Managed by `WebPortal` and `StorageLayer`.
- Active only while the ESP offline portal is running.

Online authentication:

- Username/password.
- Supabase Auth user plus `smart_home_profiles`.
- Device roles from `smart_home_device_memberships`.
- Active only in GitHub Pages/Supabase online frontend.

The ESP local credential store and the Supabase credential store never read from each other.

## Role System

Admin is true when:

- `smart_home_profiles.global_role = 'admin'`, or
- the current device membership role is `admin`.

Admin users route to `online/admin.html`, the advanced control page. This page exposes the online equivalent of the offline advanced controls.

Normal users route to `online/simple.html`. They get relay and timer controls only. Supabase RLS also blocks normal users from inserting advanced commands such as mapping, rated power, energy tracking, or consumption reset.

## Data Flows

### Offline Command Flow

Browser on ESP AP -> local HTTP/WebSocket -> MAC auth in `WebPortal` -> `ControlEngine` -> relay/timer/PIR state -> NVS/LittleFS -> WebSocket state/events.

### Online Command Flow

GitHub Pages dashboard -> Supabase Auth -> RLS insert into `smart_home_remote_commands` -> ESP `CloudSyncService` claims command by token -> `ControlEngine` executes -> ESP marks done/failed -> dashboard receives realtime update.

### Online State Sync Flow

`ControlEngine::buildStateJson()` -> `CloudSyncService::syncStateSnapshot()` -> `device_upsert_state()` -> `smart_home_device_states` -> Supabase Realtime -> online dashboards.

## GitHub Pages Architecture

The online frontend is plain static HTML/CSS/JS. It does not require Vite, React, server-side rendering, or history routing.

Deployment workflow:

- triggers on `main` and `master`
- writes `online/config.js` from GitHub Secrets
- adds `.nojekyll`
- copies `online/index.html` to `online/404.html`
- uploads the `online/` directory through GitHub Pages Actions

This avoids stale Vite assets such as `main.jsx` and `%BASE_URL%favicon.svg`.

## Time And Relay Behavior

Offline:

- Browser/user time remains valid for local timers.
- Local relays and timers work without internet.
- Offline pages send commands directly to firmware.

Online:

- NTP is forced on mode entry.
- Timer commands include current epoch from the browser/server path.
- ESP remains the only physical relay authority.
- Server state is a synchronized mirror, not the source of GPIO truth.

## Safety Guarantees

- AP mode is disabled during online mode.
- Offline pages are not served during online mode.
- Offline MAC auth is not active during online mode.
- Supabase is not required during offline mode.
- Browser code never receives the device token.
- Device RPC writes require the token hash check.
- RLS protects all browser-accessible tables.
