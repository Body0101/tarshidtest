# Tarshid Technical Documentation

This file is the legacy documentation entrypoint. The current project architecture is documented in the focused markdown files below:

- `SYSTEM_ARCHITECTURE.md`
- `ONLINE_OFFLINE_GUIDE.md`
- `SERVER_SETUP_GUIDE.md`
- `AUTH_FLOW.md`

## Current Architecture

Tarshid has two separate modes:

- Offline ESP Local Mode: ESP SoftAP, captive portal, local LittleFS pages, local MAC authentication, ESP NVS/LittleFS storage.
- Online Server Mode: infrastructure WiFi, AP disabled, GitHub Pages frontend, Supabase Auth/database, NTP time, realtime sync.

The ESP32 firmware remains the authority for relay control, PIR logic, timers, Night Lock, and physical GPIO actuation. Supabase stores online accounts, roles, remote command rows, state snapshots, and synced events. Supabase does not replace the firmware control engine.

## Key Files

ESP firmware:

- `SmartHomeAutomation/src/main.cpp`
- `SmartHomeAutomation/src/ControlEngine.cpp`
- `SmartHomeAutomation/src/WebPortal.cpp`
- `SmartHomeAutomation/src/StorageLayer.cpp`
- `SmartHomeAutomation/src/CloudSyncService.cpp`
- `SmartHomeAutomation/src/TimeKeeper.cpp`

ESP LittleFS offline pages:

- `SmartHomeAutomation/data/index.html`
- `SmartHomeAutomation/data/restricted.html`
- `SmartHomeAutomation/data/unauthorized.html`

Online static frontend:

- `online/index.html`
- `online/admin.html`
- `online/simple.html`
- `online/app.js`
- `online/styles.css`

Server/database:

- `supabase/smart_home_schema.sql`

Deployment:

- `platformio.ini`
- `scripts/inject_cloud_env.py`
- `.github/workflows/deploy-online.yml`

## Build

Use PlatformIO:

```powershell
platformio run
platformio run --target upload
platformio run --target uploadfs
```

For online-capable firmware, set the WiFi and Supabase environment variables described in `ONLINE_OFFLINE_GUIDE.md`.

## Authentication

Offline authentication is MAC-address based and stored in ESP NVS. Online authentication uses Supabase Auth username/password and roles stored in Supabase tables. These systems are intentionally isolated.
