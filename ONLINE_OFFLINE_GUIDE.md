# Online / Offline Guide

## Short Version

Tarshid has two operating modes. They are intentionally separate:

- Offline mode: ESP32 local AP, local pages, MAC authentication, ESP NVS/LittleFS storage.
- Online mode: infrastructure WiFi, AP disabled, GitHub Pages frontend, Supabase Auth/database, NTP time, cloud synchronization.

No mode shares authentication credentials with the other.

## Offline Mode

Offline mode is the ESP local mode. It is active only when the ESP32 does not have an internet/server path.

### Offline Startup

The firmware starts offline mode when:

- STA WiFi credentials are missing.
- cloud credentials are missing.
- STA WiFi cannot connect.
- internet probe fails.
- online mode fails repeatedly after boot.

### Offline Services

Offline mode starts:

- ESP SoftAP
- captive DNS
- local HTTP server
- local WebSocket server
- LittleFS pages
- MAC-address account authentication

Offline mode uses the existing AP values from `SmartHomeAutomation/src/Config.h`.

### Offline Files

ESP LittleFS files:

- `SmartHomeAutomation/data/index.html`
- `SmartHomeAutomation/data/restricted.html`
- `SmartHomeAutomation/data/unauthorized.html`

Firmware modules:

- `SmartHomeAutomation/src/WebPortal.cpp`
- `SmartHomeAutomation/src/StorageLayer.cpp`
- `SmartHomeAutomation/src/ControlEngine.cpp`
- `SmartHomeAutomation/src/TimeKeeper.cpp`
- `SmartHomeAutomation/src/main.cpp`

### Offline Authentication

Offline users are identified by MAC address. The user records are stored only inside ESP NVS through `StorageLayer` key `users_json`.

Offline account fields include:

- MAC address
- display name
- password hash
- admin flag
- user-management permission
- restricted flag

The existing MAC registration and authorization behavior remains firmware-owned. Supabase is never called for offline login.

### Offline Controls

Offline mode supports:

- local advanced dashboard
- restricted/simple local dashboard
- relay manual ON/OFF/AUTO
- local timers
- PIR mapping
- energy tracking
- rated power
- consumption reset
- local logs
- Night Lock behavior

## Online Mode

Online mode is the server/internet mode. It is active only when the ESP32 is connected to infrastructure WiFi with internet access.

### Online Startup

The firmware enters online mode when:

- `WIFI_STA_SSID` and `WIFI_STA_PASSWORD` are compiled.
- `SUPABASE_URL`, `SUPABASE_ANON_KEY`, `CLOUD_DEVICE_ID`, and `CLOUD_COMMAND_TOKEN` are compiled.
- WiFi connects in STA mode.
- DNS/internet reachability succeeds.

### Online Services

Online mode stops:

- ESP SoftAP
- captive DNS
- local HTTP server
- local WebSocket server
- offline client tracking
- offline MAC authentication

Online mode continues:

- relay actuation
- PIR logic
- timers
- NTP time
- Supabase state/event sync
- Supabase command polling

### Online Files

GitHub Pages/static frontend:

- `online/index.html`
- `online/admin.html`
- `online/simple.html`
- `online/app.js`
- `online/styles.css`
- `online/config.js`
- `online/config.example.js`

Online backend:

- `supabase/smart_home_schema.sql`

Firmware online node:

- `SmartHomeAutomation/src/CloudSyncService.cpp`
- `SmartHomeAutomation/src/CloudSyncService.h`

### Online Authentication

Online login uses:

- username from `smart_home_profiles.username`
- password from Supabase Auth
- role from `smart_home_profiles.global_role` and `smart_home_device_memberships.role`

The browser calls `auth_email_for_username()` only to map username to the Supabase Auth email. Password verification remains Supabase Auth.

### Online Role Routing

Admin users:

- route to `online/admin.html`
- can use advanced controls equivalent to offline advanced behavior
- can insert advanced command types under RLS

Normal users:

- route to `online/simple.html`
- can use relay and timer controls only
- cannot access the admin page
- cannot insert advanced command rows because RLS checks role and command type

## Build-Time Configuration

Set these values before building firmware for online mode:

```powershell
$env:WIFI_STA_SSID="YourWiFi"
$env:WIFI_STA_PASSWORD="YourWiFiPassword"
$env:SUPABASE_URL="https://YOUR_PROJECT_REF.supabase.co"
$env:SUPABASE_ANON_KEY="your-supabase-publishable-or-anon-key"
$env:CLOUD_DEVICE_ID="esp32-main"
$env:CLOUD_COMMAND_TOKEN="strong-random-device-token"
platformio run
```

If any required value is missing, cloud sync is not configured and the ESP remains offline/local.

## GitHub Pages Deployment

The Pages workflow is `.github/workflows/deploy-online.yml`.

It:

- runs on `main` and `master`
- publishes only the `online/` directory
- writes `online/config.js` from GitHub Secrets
- creates `.nojekyll`
- creates `404.html` from `index.html` for static fallback behavior

Required GitHub repository secrets:

- `SUPABASE_URL`
- `SUPABASE_ANON_KEY`

Static hosting rules:

- use `index.html`, `admin.html`, and `simple.html`
- keep paths relative, such as `./app.js` and `./styles.css`
- do not use root-only asset paths
- do not expose `CLOUD_COMMAND_TOKEN`
- do not use Supabase service-role keys in browser code

## Supabase Runtime Behavior

Browser:

1. signs in through Supabase Auth
2. reads profile and memberships under RLS
3. subscribes to device state/events/command completion
4. inserts command rows allowed by role

ESP:

1. forces NTP in online mode
2. uploads state snapshots through `device_upsert_state()`
3. uploads events through `device_insert_event()`
4. claims commands through `device_claim_commands()`
5. executes commands through `ControlEngine`
6. marks results through `device_finish_command()`

## Failure Behavior

### Internet Lost

The ESP records repeated internet failures. After the configured failure threshold it switches back to offline mode, restarts AP/captive portal/local pages, and offline MAC authentication becomes available again.

### Supabase Unavailable

Relay/PIR/timer control continues locally. Cloud sync fails until connectivity returns or the firmware falls back to offline mode.

### GitHub Pages Unavailable

Online browser access is unavailable, but ESP local automation continues. If the ESP loses internet, offline AP mode returns.

## Validation Checklist

- In offline mode, ESP AP is visible and local pages load.
- In offline mode, Supabase login is not required.
- In offline mode, MAC registration/auth remains active.
- In online mode, ESP AP is not visible.
- In online mode, local ESP pages do not serve.
- In online mode, GitHub Pages login uses username/password.
- Admin lands on `admin.html`.
- Normal user lands on `simple.html`.
- Normal user cannot insert advanced command JSON.
- ESP state appears in `smart_home_device_states`.
- ESP events appear in `smart_home_device_events`.
- Completed commands update `smart_home_remote_commands`.
