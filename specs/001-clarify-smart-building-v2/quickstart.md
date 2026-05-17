# Quickstart: Smart Building System v2

## Prerequisites

- PlatformIO (VSCode extension or CLI)
- Node.js 18+
- Supabase project (free tier sufficient)
- ESP32 dev board (tested with ESP32-WROOM-32)

## Setup Steps

### 1. Clone & Configure

```bash
git checkout -b 001-clarify-smart-building-v2
```

### 2. Configure Supabase

```bash
cp online/config.example.js online/config.js
```

Edit `online/config.js`:
```js
window.SUPABASE_URL = 'https://your-project.supabase.co'
window.SUPABASE_ANON_KEY = 'your-anon-key'
```

Create Supabase project, run schema:
```bash
# Execute supabase/smart_home_schema.sql in Supabase SQL editor
```

### 3. Configure ESP32 Firmware

```bash
python scripts/inject_cloud_env.py
```

This prompts for:
- Supabase URL
- Supabase Anon Key

These are stored in `SmartHomeAutomation/src/Config.h`.

### 4. Build & Flash ESP32

```bash
pio run -t upload --environment esp32dev
pio run -t uploadfs --environment esp32dev
```

### 5. Start Online Dashboard

```bash
node server.js
```

Visit: `http://localhost:5000`

## Development Workflow

1. Flash ESP32 → connects to Wi-Fi → registers with Supabase
2. Login to online dashboard as admin → see device
3. Edit device/relay names → stored in Supabase → pushed to ESP
4. Test offline: disconnect ESP from internet → AP mode activates
5. Test reconnection: restore internet → states sync

## Key Architecture Files

| File | Purpose |
|------|---------|
| `SmartHomeAutomation/src/main.cpp` | FreeRTOS tasks + WiFi/network management |
| `SmartHomeAutomation/src/ControlEngine.cpp` | Relay control logic (channel-only, no names) |
| `SmartHomeAutomation/src/CloudSyncService.cpp` | Supabase HTTPS + WebSocket sync |
| `SmartHomeAutomation/src/WebPortal.cpp` | Offline AP captive portal |
| `online/app.js` | Online dashboard Supabase client |
| `online/admin.html` | Admin configuration page |
| `supabase/smart_home_schema.sql` | Database schema + RLS policies |
| `server.js` | Dev HTTP server |
