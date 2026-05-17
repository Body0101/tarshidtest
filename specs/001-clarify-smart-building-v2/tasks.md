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

- [ ] T001 Create project structure per implementation plan
- [ ] T002 Initialize [language] project with [framework] dependencies
- [ ] T003 [P] Configure linting and formatting tools

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core infrastructure that MUST be complete before ANY user story can be implemented

**âš ï¸ CRITICAL**: No user story work can begin until this phase is complete

Examples of foundational tasks (adjust based on your project):

- [ ] T004 Setup database schema and migrations framework
- [ ] T005 [P] Implement authentication/authorization framework
- [ ] T006 [P] Setup API routing and middleware structure
- [ ] T007 Create base models/entities that all stories depend on
- [ ] T008 Configure error handling and logging infrastructure
- [ ] T009 Setup environment configuration management
- [ ] T010 [P] Configure hardware testing environment for ESP32 validation
- [ ] T011 [P] Setup performance benchmarking infrastructure

**Checkpoint**: Foundation ready - user story implementation can now begin in parallel

---

## Phase 3: User Story 1 - Admin Configuration Management (Priority: P1) ðŸŽ¯ MVP

**Goal**: As an administrator, I want to configure ESP devices and their relays through a secure admin interface so that I can manage the smart building system without exposing configuration logic to the ESP firmware.

**Independent Test**: Can be tested by verifying that admin can rename ESPs and relays through the admin interface, changes are stored in Supabase, and ESPs receive updated labels without affecting their internal logic.

### Tests for User Story 1 (OPTIONAL - only if tests requested) âš ï¸

> **NOTE: Write these tests FIRST, ensure they FAIL before implementation**

- [ ] T012 [P] [US1] Contract test for [endpoint] in tests/contract/test_[name].py
- [ ] T013 [P] [US1] Integration test for [user journey] in tests/integration/test_[name].py

### Implementation for User Story 1

- [ ] T014 [P] [US1] Create [Entity1] model in src/models/[entity1].py
- [ ] T015 [P] [US1] Create [Entity2] model in src/models/[entity2].py
- [ ] T016 [US1] Implement [Service] in src/services/[service].py (depends on T012, T013)
- [ ] T017 [US1] Create admin configuration page for managing ESP identities and relay friendly names
- [ ] T018 [US1] Implement Supabase storage for configuration changes (ESP names, relay names)
- [ ] T019 [US1] Implement server-side logic to push configuration updates to ESP devices
- [ ] T020 [US1] Ensure ESP devices receive updated labels/names from server but do NOT use them for internal control logic

---

## Phase 4: User Story 2 - Hybrid Online/Offline Operation (Priority: P1) ðŸŽ¯ MVP

**Goal**: As a user, I want ESP devices to maintain reliable relay control regardless of internet connectivity so that my smart home automation continues working during network outages.

**Independent Test**: Can be tested by simulating network loss and restoration while verifying ESP behavior and state synchronization.

### Tests for User Story 2 (OPTIONAL - only if tests requested) âš ï¸

- [ ] T021 [P] [US2] Contract test for [endpoint] in tests/contract/test_[name].py
- [ ] T022 [P] [US2] Integration test for [user journey] in tests/integration/test_[name].py

### Implementation for User Story 2

- [ ] T023 [US2] Implement ESP local cache for last known relay states and configuration
- [ ] T024 [US2] Implement periodic sync with server when online connection is available
- [ ] T025 [US2] Implement server push of configuration updates to ESP devices when connected
- [ ] T026 [US2] Implement ESP reload of last known state from local storage on power loss/restart
- [ ] T027 [US2] Implement ESP synchronization with server when connection becomes available after power restoration
- [ ] T028 [US2] Implement ESP Access Point mode activation when internet connection is lost
- [ ] T029 [US2] Ensure ESP in Access Point mode continues operating normally using cached state only
- [ ] T030 [US2] Ensure ESP in Access Point mode does NOT expose admin/user logic or authentication
- [ ] T031 [US2] Implement ESP sending of pending state updates to server when reconnecting to internet
- [ ] T032 [US2] Implement server update of database with ESP state updates and response with latest configuration
- [ ] T033 [US2] Implement ESP update of local cache with server response after synchronization
- [ ] T034 [US2] Implement conflict resolution using last-write-wins strategy with server as source of truth

---

## Phase 5: User Story 3 - Role-Based Access Control (Priority: P2) ðŸŽ¯

**Goal**: As a system administrator, I want to enforce strict role-based access control on the server side so that users can only access their assigned ESP devices while admins have full system control.

**Independent Test**: Can be tested by verifying that non-admin users cannot access admin functionality and can only operate assigned devices.

### Tests for User Story 3 (OPTIONAL - only if tests requested) âš ï¸

- [ ] T035 [P] [US3] Contract test for [endpoint] in tests/contract/test_[name].py
- [ ] T036 [P] [US3] Integration test for [user journey] in tests/integration/test_[name].py

### Implementation for User Story 3

- [ ] T037 [US3] Implement role-based access control logic strictly server-side (Supabase RLS)
- [ ] T038 [US3] Ensure ESP devices have NO knowledge of admin users, regular users, roles, or permissions
- [ ] T039 [US3] Ensure ESP devices only handle: relay states (ON/OFF), relay hardware IDs, and synchronization with server
- [ ] T040 [US3] Create admin dashboard showing full system control with configuration access
- [ ] T041 [US3] Create user dashboard showing only assigned ESPs with operational controls only

---

## Phase 6: User Story 4 - Real-time Synchronization (Priority: P2) ðŸŽ¯

**Goal**: As a user, I want near real-time synchronization between ESP devices and the server so that state changes are quickly reflected across all connected clients.

**Independent Test**: Can be tested by measuring latency between state changes on ESP and updates in server/database.

### Tests for User Story 4 (OPTIONAL - only if tests requested) âš ï¸

- [ ] T042 [P] [US4] Contract test for [endpoint] in tests/contract/test_[name].py
- [ ] T043 [P] [US4] Integration test for [user journey] in tests/integration/test_[name].py

### Implementation for User Story 4

- [ ] T044 [US4] Implement state synchronization where database is the source of truth for relay states and metadata
- [ ] T045 [US4] Implement real-time synchronization with target latencies: <200ms for command to server, <500ms for server to ESP/client
- [ ] T046 [US4] Ensure UI uses single action button that expands into multiple actions for clean interface
- [ ] T047 [US4] Ensure navigation structure is simplified and consistent between admin/user views

---

## Phase 7: User Story 5 - Power Loss Recovery (Priority: P2) ðŸŽ¯

**Goal**: As a user, I want the system to maintain relay state integrity across power cycles so that my smart home devices return to expected states after power restoration.

**Independent Test**: Can be tested by simulating power loss and verifying state recovery.

### Tests for User Story 5 (OPTIONAL - only if tests requested) âš ï¸

- [ ] T048 [P] [US5] Contract test for [endpoint] in tests/contract/test_[name].py
- [ ] T049 [P] [US5] Integration test for [user journey] in tests/integration/test_[name].py

### Implementation for User Story 5

- [ ] T050 [US5] Already covered in User Story 2 tasks (T023-T034)
- [ ] T051 [US5] Ensure local state cache persists across power cycles and maintains relay state accuracy with zero data loss

---