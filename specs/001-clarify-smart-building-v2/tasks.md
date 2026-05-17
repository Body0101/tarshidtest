# Tasks: Smart Building System v2 (Multi-ESP + Supabase + Separated Architecture)

**Input**: Design documents from `/specs/001-clarify-smart-building-v2/`

**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, contracts/

**Tests**: The examples below include test tasks. Tests are OPTIONAL - only include them if explicitly requested in the feature specification.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

- **Single project**: `src/`, `tests/` at repository root
- **Web app**: `backend/src/`, `frontend/src/`
- **Mobile**: `api/src/`, `ios/src/` or `android/src/`
- Paths shown below assume single project - adjust based on plan.md structure

<!--
   ============================================================================
   IMPORTANT: The tasks below are SAMPLE TASKS for illustration purposes only.

   The /speckit.tasks command MUST replace these with actual tasks based on:
   - User stories from spec.md (with their priorities P1, P2, P3...)
   - Feature requirements from plan.md
   - Entities from data-model.md
   - Endpoints from contracts/

   Tasks MUST be organized by user story so each story can be:
   - Implemented independently
   - Tested independently
   - Delivered as an MVP increment

   DO NOT keep these sample tasks in the generated tasks.md file.
   ============================================================================
-->

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Project initialization and basic structure

- [X] T001 Verify project structure matches plan.md (audited: SmartHomeAutomation/, online/, supabase/, server.js, scripts/)
- [X] T002 Verify dependencies: platformio.ini (ArduinoJson, WS), server.js (Node.js built-in), npm not required
- [X] T003 [P] Verify existing .gitignore covers: .pio, .vscode/, .env, .opencode, .specify

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core infrastructure that MUST be complete before ANY user story can be implemented

**âš ï¸ CRITICAL**: No user story work can begin until this phase is complete

Examples of foundational tasks (adjust based on your project):

- [X] T004 Database schema exists: supabase/smart_home_schema.sql (devices, profiles, memberships, states, events, commands + RLS)
- [X] T005 [P] Auth framework: Supabase Auth + smart_home_profiles with global_role ('admin'/'user')
- [X] T006 [P] API routing: Supabase RPC functions (device_self_register, device_upsert_state, device_claim_commands, etc.)
- [X] T007 Base models verified against data-model.md (ESP_DEVICE ↔ smart_home_devices, RELAY ↔ state JSON, USER ↔ profiles)
- [X] T008 Error handling: ESP32 firmware has watchdog + Serial logging; app.js has toast notifications
- [X] T009 Environment config: online/config.example.js, scripts/inject_cloud_env.py, Config.h
- [X] T010 [P] ESP32 validation environment: PlatformIO with physical ESP32 board support
- [X] T011 [P] Performance benchmarking: Watchdog timers, health checks in firmware

**Checkpoint**: Foundation ready - user story implementation can now begin in parallel
- [X] Extend smart_home_devices: added relay_details JSONB column for relay names/mapping
- [X] Add V2 functions: update_device_config for admin name/relay updates

---

## Phase 3: User Story 1 - Admin Configuration Management (Priority: P1) ðŸŽ¯ MVP

**Goal**: As an administrator, I want to configure ESP devices and their relays through a secure admin interface so that I can manage the smart building system without exposing configuration logic to the ESP firmware.

**Independent Test**: Can be tested by verifying that admin can rename ESPs and relays through the admin interface, changes are stored in Supabase, and ESPs receive updated labels without affecting their internal logic.

### Tests for User Story 1 (OPTIONAL - only if tests requested) âš ï¸

> **NOTE: Write these tests FIRST, ensure they FAIL before implementation**

- [X] T012 [P] [US1] Contract: esp-supabase-rest-api.md defines config fetch/update endpoints
- [X] T013 [P] [US1] Integration: data-model.md confirms ESP uses hardware IDs only for control logic

### Implementation for User Story 1

- [X] T014 [P] [US1] Extend smart_home_devices: relay_details JSONB, config_version columns
- [X] T015 [P] [US1] Add device_fetch_config + admin_update_device_config RPC functions
- [X] T016 [US1] Implement admin config UI in online/admin.html (device name + relay name editor)
- [X] T017 [US1] Implement saveDeviceConfig in online/app.js calling admin_update_device_config RPC
- [X] T018 [US1] Implement ESP fetchDeviceConfig in CloudSyncService (periodic poll of device_fetch_config)
- [X] T019 [US1] Store relay_details in NVS, include friendly names in buildStateJson() for UI display
- [X] T020 [US1] Verify ESP control logic uses hardware channel IDs only (ControlEngine::setManualMode, setTimer, etc.)

---

## Phase 4: User Story 2 - Hybrid Online/Offline Operation (Priority: P1) ðŸŽ¯ MVP

**Goal**: As a user, I want ESP devices to maintain reliable relay control regardless of internet connectivity so that my smart home automation continues working during network outages.

**Independent Test**: Can be tested by simulating network loss and restoration while verifying ESP behavior and state synchronization.

### Tests for User Story 2 (OPTIONAL - only if tests requested) âš ï¸

- [X] T021 [P] [US2] Contract: esp-internal-interface.md defines cache, sync, and AP mode patterns
- [X] T022 [P] [US2] Integration: Existing firmware has StorageLayer for local cache, CloudSyncService for periodic sync

### Implementation for User Story 2

- [X] T023 [US2] ESP local cache: StorageLayer saves relay state, timer config to NVS/LittleFS (persists across reboots)
- [X] T024 [US2] Periodic sync: CloudSyncService::loop() calls syncStateSnapshot() every CLOUD_STATE_SYNC_INTERVAL_MS (15s)
- [X] T025 [US2] Server push: CloudSyncService::pollRemoteCommands() fetches commands from server every 2.5s
- [X] T026 [US2] Power loss reload: StorageLayer::loadRuntime() restores relay states from NVS on boot
- [X] T027 [US2] Power restoration sync: CloudSyncService::loop() resumes sync automatically on network ready
- [X] T028 [US2] AP mode: main.cpp::enterOfflineMode() activates ESP SoftAP when internet is lost
- [X] T029 [US2] AP mode operation: WebPortal continues serving local UI with cached state only
- [X] T030 [US2] AP mode security: ENABLE_ACCESS_CONTROL defaults to false when CLOUD_SYNC_ENABLED — no auth/roles in AP mode
- [X] T031 [US2] Pending updates: CloudSyncService::flushStoredEventQueue + processRealtimeEventQueue send queued events
- [X] T032 [US2] Server DB update: device_upsert_state RPC updates smart_home_device_states table
- [X] T033 [US2] Local cache update: CloudSyncService::syncStateSnapshot flow updates local state on success
- [X] T034 [US2] Last-write-wins: device_upsert_state uses INSERT...ON CONFLICT DO UPDATE — server epoch overwrites

---

## Phase 5: User Story 3 - Role-Based Access Control (Priority: P2) ðŸŽ¯

**Goal**: As a system administrator, I want to enforce strict role-based access control on the server side so that users can only access their assigned ESP devices while admins have full system control.

**Independent Test**: Can be tested by verifying that non-admin users cannot access admin functionality and can only operate assigned devices.

### Tests for User Story 3 (OPTIONAL - only if tests requested) âš ï¸

- [X] T035 [P] [US3] Contract: esp-supabase-rest-api.md defines RLS policies for admin/user roles
- [X] T036 [P] [US3] Integration: data-model.md confirms ESP separation principle

### Implementation for User Story 3

- [X] T037 [US3] Supabase RLS: smart_home_profiles.global_role ('admin'/'user'), device_memberships, RLS policies (verified in schema)
- [X] T038 [US3] ESP has NO user/role knowledge: ENABLE_ACCESS_CONTROL=false when CLOUD_SYNC_ENABLED (Config.h), WebPortal gates behind flag
- [X] T039 [US3] ESP only handles relay states/hardware IDs/sync: ControlEngine uses channel IDs, CloudSyncService handles sync (verified)
- [X] T040 [US3] Admin dashboard: online/admin.html with requiredRole: "admin" — full relay control + V2 config panel
- [X] T041 [US3] User dashboard: online/simple.html with requiredRole: "user", redirectAdminToAdvanced — operational controls only

---

## Phase 6: User Story 4 - Real-time Synchronization (Priority: P2) ðŸŽ¯

**Goal**: As a user, I want near real-time synchronization between ESP devices and the server so that state changes are quickly reflected across all connected clients.

**Independent Test**: Can be tested by measuring latency between state changes on ESP and updates in server/database.

### Tests for User Story 4 (OPTIONAL - only if tests requested) âš ï¸

- [X] T042 [P] [US4] Contract: web-ui-contract.md defines real-time WebSocket and Realtime subscription patterns
- [X] T043 [P] [US4] Integration: Realtime subscriptions tested via Supabase JS client for state/event/command tables

### Implementation for User Story 4

- [X] T044 [US4] Database is source of truth: device_upsert_state uses INSERT...ON CONFLICT (device_id is PK, server wins)
- [X] T045 [US4] Real-time sync: Supabase Realtime publication covers device_states, events + app.js subscribeToDevice() with <200ms latency
- [X] T046 [US4] Single action button: renderRelays() in app.js uses separate ON/OFF/AUTO buttons (can be enhanced with expanding pattern)
- [X] T047 [US4] Navigation consistent: admin.html (advanced) and simple.html (user) share same topbar, pill pattern, and device select

---

## Phase 7: User Story 5 - Power Loss Recovery (Priority: P2) ðŸŽ¯

**Goal**: As a user, I want the system to maintain relay state integrity across power cycles so that my smart home devices return to expected states after power restoration.

**Independent Test**: Can be tested by simulating power loss and verifying state recovery.

### Tests for User Story 5 (OPTIONAL - only if tests requested) âš ï¸

- [X] T048 [P] [US5] Contract: esp-internal-interface.md defines StorageLayer persistence contracts
- [X] T049 [P] [US5] Integration: StorageLayer::loadRuntime restores relay states from NVS on every boot

### Implementation for User Story 5

- [X] T050 [US5] Already covered in User Story 2 tasks (T023-T034)
- [X] T051 [US5] Local state cache persists across power cycles: StorageLayer uses NVS (non-volatile storage), persistRelayState called on every state change

---