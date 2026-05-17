# ESP ↔ Supabase REST API Contract

## Base URL
`https://[PROJECT].supabase.co/rest/v1/`

## Authentication
- Header: `apikey: <anon_key>`
- Header: `Authorization: Bearer <device_token>` (for device endpoints)
- Header: `Authorization: Bearer <user_token>` (for user endpoints)

## Endpoints

### 1. Device Registration

**POST** `/devices`

Register a new ESP32 device on first online connection.

**Request Body:**
```json
{
  "hardware_id": "ESP32_ABC123",
  "name": "Living Room ESP",
  "device_type": "ESP32",
  "relay_count": 4,
  "metadata": {
    "firmware_version": "2.0.0",
    "chip_id": "0x1234ABCD"
  }
}
```

**Response (201):**
```json
{
  "id": "uuid-here",
  "hardware_id": "ESP32_ABC123",
  "name": "Living Room ESP",
  "device_type": "ESP32",
  "relay_count": 4,
  "device_token": "generated-device-token",
  "created_at": "2026-05-17T00:00:00Z"
}
```

**Error (409):** Device already registered → return existing device with updated metadata

### 2. State Sync

**POST** `/states`

Send pending relay state changes from ESP to server.

**Request Body:**
```json
{
  "device_id": "uuid",
  "states": [
    {
      "relay_hardware_channel": 0,
      "state": "ON",
      "source": "local",
      "timestamp": 1715900000
    },
    {
      "relay_hardware_channel": 1,
      "state": "OFF",
      "source": "timer",
      "timestamp": 1715900001
    }
  ]
}
```

**Response (200):**
```json
{
  "accepted": 2,
  "rejected": 0,
  "latest_config": {
    "sync_interval_seconds": 30,
    "relays": [
      {"channel": 0, "state": "ON"},
      {"channel": 1, "state": "OFF"},
      {"channel": 2, "state": "OFF"},
      {"channel": 3, "state": "OFF"}
    ]
  }
}
```

### 3. Config Sync

**GET** `/devices/{device_id}/config`

Fetch latest configuration from server.

**Response (200):**
```json
{
  "device_id": "uuid",
  "sync_interval_seconds": 30,
  "ap_mode_enabled": true,
  "relays": [
    {
      "channel": 0,
      "friendly_name": "Living Room Light",
      "rated_power_watts": 60
    }
  ],
  "updated_at": "2026-05-17T00:00:00Z"
}
```

> Note: ESP ignores `friendly_name` — used for UI mapping only.

### 4. Heartbeat

**PATCH** `/devices/{device_id}`

Update device online status and metadata.

**Request Body:**
```json
{
  "is_online": true,
  "last_seen_at": "2026-05-17T00:01:00Z",
  "metadata": {
    "rssi": -65,
    "heap_free": 45000,
    "uptime_seconds": 3600
  }
}
```

**Response (200):** `{}`

### 5. State Subscription (WebSocket)

**CONNECT** `wss://[PROJECT].supabase.co/realtime/v1`

Subscribe to real-time state changes for assigned devices.

**Channel:** `realtime:public:states`

**Event:** `INSERT` on `states` table for device's relays

**Message:**
```json
{
  "type": "INSERT",
  "table": "states",
  "record": {
    "relay_id": "uuid",
    "esp_device_id": "uuid",
    "state": "ON",
    "source": "cloud",
    "recorded_at": "2026-05-17T00:00:00Z"
  }
}
```

## Supabase RLS Policies

### `devices` table
- Admin: ALL operations
- User: SELECT only (assigned devices)
- Device token: SELECT, UPDATE (own record)

### `relays` table
- Admin: ALL operations
- User: SELECT only (child of assigned device)
- Device token: SELECT (own relays)

### `states` table
- Admin: ALL operations
- User: SELECT, INSERT (assigned devices)
- Device token: INSERT (own states)
