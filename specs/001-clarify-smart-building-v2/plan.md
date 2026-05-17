# Implementation Plan: Smart Building System v2 (Multi-ESP + Supabase + Separated Architecture)

**Branch**: `001-clarify-smart-building-v2` | **Date**: 2026-05-17 | **Spec**: `/specs/001-clarify-smart-building-v2/spec.md`

**Input**: Feature specification from `/specs/001-clarify-smart-building-v2/spec.md`

**Note**: This template is filled in by the `/speckit.plan` command. See `.specify/templates/plan-template.md` for the execution workflow.

## Summary

Build a scalable IoT management system with strict separation between server-side management logic and ESP runtime behavior, including robust offline/online synchronization and admin-only configuration controls. The system supports 100+ ESP32 devices with real-time state synchronization, role-based access control, and power-loss recovery.

## Technical Context

**Language/Version**: C++17 (Arduino framework for ESP32), JavaScript (Node.js 18+), HTML5/CSS3/ES6

**Primary Dependencies**:
- ESP32: ArduinoJson, arduinoWebSockets, LittleFS, WiFi (ESP-IDF)
- Server: Node.js built-in http module (dev server), Supabase JS client v2
- Frontend: Vanilla JS (no framework), Chart.js (optional for energy tracking)

**Storage**:
- ESP32: LittleFS (filesystem), NVS (preferences) for local cache/persistence
- Cloud: Supabase PostgreSQL with Row Level Security (RLS), Supabase Auth
- Schema: `supabase/smart_home_schema.sql` (existing)

**Testing**:
- ESP32: PlatformIO test framework, hardware-in-the-loop on physical devices
- Server: Node.js test runner, Supertest for HTTP endpoints
- E2E: Simulated network conditions, power cycle testing

**Target Platform**: ESP32 dev board (espressif32), Node.js server (any OS), Web browser (Chrome/Firefox/Safari)

**Project Type**: IoT firmware + Web application (backend API + frontend dashboard)

**Performance Goals**:
- Local relay control: <50ms
- Local UI interaction: <100ms
- Cloud-synced operations: <500ms
- Real-time sync: <200ms command→server, <500ms server→ESP/client
- ESP32 heap usage: <180KB
- ESP32 CPU load: <80% average
- Control task period: 50ms

**Constraints**:
- ESP firmware must have ZERO knowledge of users, roles, admins, permissions
- ESP only handles: relay states (ON/OFF), relay hardware IDs, sync with server
- Conflict resolution: last-write-wins with server as source of truth
- Max 100 devices, 1 update/sec/device
- TLS 1.2+ with certificate pinning for ESP→Server communication
- WebSocket persistent connection for asymmetric connectivity

**Scale/Scope**: 100+ ESP32 devices, 50+ concurrent web clients, 1 update/sec/device

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

1. Code Quality Excellence
   - Embedded systems: Resource constraints, deterministic behavior, clean architecture
2. Rigorous Testing Standards
   - Hardware-in-the-loop testing, ESP32 validation, 80% unit test coverage
3. Consistent User Experience
   - Dual-mode consistency (local/online), WCAG 2.1 AA accessibility
4. Performance Requirements
   - <50ms local control, <100ms UI, <500ms cloud, <180KB heap, <80% CPU
5. Development Workflow
   - GitHub Flow, PR reviews, CI/CD, semantic versioning
6. Quality Gates
   - Tests pass, review approved, benchmarks met, security scanned

All gates pass. No violations requiring complexity tracking.

## Project Structure

### Documentation (this feature)

```text
specs/001-clarify-smart-building-v2/
  plan.md              # This file (/speckit.plan command output)
  research.md          # Phase 0 output (/speckit.plan command)
  data-model.md        # Phase 1 output (/speckit.plan command)
  quickstart.md        # Phase 1 output (/speckit.plan command)
  contracts/           # Phase 1 output (/speckit.plan command)
  tasks.md             # Phase 2 output (/speckit.tasks command)
```

### Source Code (repository root)

```text
# ESP32 Firmware
SmartHomeAutomation/
  src/
    main.cpp              # FreeRTOS task orchestration
    Config.h              # Compile-time constants + WiFi/NVS credentials
    CloudSyncService.h/.cpp  # Supabase HTTPS + WebSocket sync
    ControlEngine.h/.cpp  # Relay/PIR/Auto control logic engine
    StorageLayer.h/.cpp   # LittleFS + NVS persistence
    SystemTypes.h         # Shared enums, structs, RelayConfig[]
    TimeKeeper.h/.cpp     # NTP sync, user-epoch tracking
    WebPortal.h/.cpp      # Offline AP captive portal + WebSockets
    Utils.h               # Date, URL helpers
  data/
    index.html            # Offline AP UI - relay control, timers
    wifi.html             # Wi-Fi provisioning page
    styles.css            # Offline AP styles
    unauthorized.html     # Access denied page
    restricted.html       # Restricted access page

# Server/Backend (Node.js)
server.js                 # Dev HTTP server for online dashboard
scripts/
  inject_cloud_env.py     # Pre-build script to inject Supabase URL/key

# Online Web Dashboard
online/
  index.html              # User dashboard
  admin.html              # Admin configuration dashboard
  app.js                  # Main app logic (Supabase client, WebSocket)
  config.js               # Runtime config (auto-generated)
  config.example.js       # Example config template
  styles.css              # Dashboard styles
  simple.html             # Minimal standalone UI

# Database
supabase/
  smart_home_schema.sql   # PostgreSQL schema (tables, RLS, functions)

# CI/CD
.github/
  workflows/
    deploy.yml            # GitHub Actions deployment

# Build
platformio.ini            # PlatformIO project configuration
```

**Structure Decision**: Multi-component project with three main subsystems:
1. ESP32 firmware (PlatformIO/C++) in `SmartHomeAutomation/`
2. Online web dashboard (Vanilla JS) in `online/`
3. Server (Node.js) at root (`server.js`)
4. Database schema in `supabase/`

No changes to this structure are needed for v2 — the spec extends existing capabilities.

## Complexity Tracking

> No violations. All constitution gates pass.


