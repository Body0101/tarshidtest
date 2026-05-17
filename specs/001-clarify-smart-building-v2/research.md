# Research Notes: Smart Building System v2

## Language & Framework Decisions

### ESP32 Firmware
- **Decision**: C++17 with Arduino framework on PlatformIO
- **Rationale**: Existing codebase already uses Arduino framework with PlatformIO; no need to change. Provides deterministic real-time behavior required for relay control.
- **Alternatives considered**: ESP-IDF directly (more control but more verbose), MicroPython (too slow, non-deterministic for 50ms control loop)
- **Dependencies**: ArduinoJson (JSON serialization), arduinoWebSockets (WebSocket server for AP mode), LittleFS (filesystem), ESP-NVS (preferences)

### Server/Backend
- **Decision**: Node.js with Supabase JS client
- **Rationale**: Existing `server.js` is a minimal dev server; Supabase handles auth, database, and real-time subscriptions via Postgres replication.
- **Alternatives considered**: Express.js (could be added later), Python/FastAPI (not in current stack)
- **Note**: For production, upgrade to a proper Node.js framework (Express/Fastify) with Supabase Management API

### Frontend
- **Decision**: Vanilla JavaScript (no framework)
- **Rationale**: Existing codebase uses vanilla JS; minimal complexity for admin/user dashboards. Supabase JS client handles auth and real-time.
- **Alternatives considered**: React/Vue (overkill for this scale), Alpine.js (possible future upgrade)

## Storage & Persistence

### ESP32 Local Storage
- **Decision**: LittleFS for web UI files + NVS (Preferences) for runtime data
- **Rationale**: Existing implementation already uses LittleFS for data files and NVS for key-value runtime state (relay states, timers, WiFi credentials)
- **Capacity**: 1-2MB LittleFS partition, NVS has ~15KB effective for preferences
- **Data persisted**: Relay states, timer configs, PIR mappings, WiFi credentials, user access roster, energy tracking config

### Cloud Database
- **Decision**: Supabase PostgreSQL with Row Level Security (RLS)
- **Rationale**: Existing schema at `supabase/smart_home_schema.sql` already defines devices, relays, users tables with RLS policies.
- **Schema approach**: Extend existing schema with V2 columns (friendly names, metadata) rather than new tables

## Communication Protocols

### ESP ↔ Server (Online Mode)
- **Decision**: HTTPS REST for device registration + config sync; WebSocket for real-time state updates
- **Rationale**: REST for asynchronous/idempotent operations (register, sync config), WebSocket for bidirectional real-time relay state push
- **Security**: TLS 1.2+ with certificate pinning, device token authentication
- **Existing implementation**: `CloudSyncService` handles HTTPS calls; WebSocket support needs enhancement for real-time

### ESP ↔ Browser (Offline/AP Mode)
- **Decision**: ESP32 as WebSocket server + HTTP server (captive portal)
- **Rationale**: Existing `WebPortal` class provides both HTTP (serving pages) and WebSocket (real-time relay control) in AP mode
- **Design**: No authentication in AP mode (physical access assumed secure), no cloud dependency

## Synchronization Strategy

- **Decision**: Last-write-wins with server as source of truth
- **Rationale**: Simple, predictable, matches existing implementation approach. Clock drift tolerance of ±100ms.
- **Conflict handling**: Timestamps compared; most recent write wins. Server resolves ties.
- **Queue strategy**: Pending updates queued locally when offline, sent in batch on reconnection
- **Sync interval**: Periodic poll + server push (WebSocket) + triggered sync on reconnection

## Access Control

- **Decision**: Server-side only (Supabase RLS + application logic)
- **Rationale**: ESP firmware must have zero knowledge of users/roles/permissions per FR-005 through FR-007
- **Implementation**: Supabase RLS policies on database tables + client-side route guards in web UI
- **Existing**: Access control roster in NVS (for local AP mode basic auth), but V2 removes auth from ESP entirely

## Security

- **Decision**: TLS 1.2+ with certificate pinning for ESP→Supabase
- **Rationale**: Protects against MITM attacks on the cloud path. No TLS in AP mode (local network assumed trusted within physical premises)
- **Alternatives considered**: mTLS (too complex for ESP32 resource constraints), custom encryption (security through obscurity)
- **Firmware**: `Config.h` holds certificate fingerprints; `inject_cloud_env.py` injects Supabase URL and anon key at build time
