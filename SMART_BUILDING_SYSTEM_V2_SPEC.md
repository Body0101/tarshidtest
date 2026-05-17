# Feature Specification: Smart Building System v2 (Multi-ESP + Supabase + Separated Architecture)

**Feature Branch**: `[smart-building-v2]`

**Created**: 2026-05-17

**Status**: Draft

**Input**: User description: "Build a scalable IoT management system with strict separation between server-side management logic and ESP runtime behavior, including robust offline/online synchronization and admin-only configuration controls."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Admin Configuration Management (Priority: P1)

As an administrator, I want to configure ESP devices and their relays through a secure admin interface so that I can manage the smart building system without exposing configuration logic to the ESP firmware.

**Why this priority**: Enables secure device management while maintaining the critical separation principle where ESPs are unaware of admin/users/roles.

**Independent Test**: Can be tested by verifying that admin can rename ESPs and relays through the admin interface, changes are stored in Supabase, and ESPs receive updated labels without affecting their internal logic.

**Acceptance Scenarios**:
1. **Given** admin logs into the system, **When** admin accesses configuration page, **Then** admin sees list of ESP devices with edit capabilities
2. **Given** admin selects an ESP device, **When** admin renames the ESP, **Then** the new name is stored in Supabase and propagated to the ESP
3. **Given** admin selects a relay within an ESP, **When** admin renames the relay (e.g., "Relay 1" → "Living Room Light"), **Then** the friendly name is stored in Supabase for UI mapping only
4. **Given** admin makes configuration changes, **When** ESP reconnects to server, **Then** ESP receives updated names but continues to control relays using hardware IDs only

### User Story 2 - Hybrid Online/Offline Operation (Priority: P1)

As a user, I want ESP devices to maintain reliable relay control regardless of internet connectivity so that my smart home automation continues working during network outages.

**Why this priority**: Addresses the fundamental requirement for local-first operation with seamless cloud synchronization.

**Independent Test**: Can be tested by simulating network loss and restoration while verifying ESP behavior and state synchronization.

**Acceptance Scenarios**:
1. **Given** ESP device online and synchronized with server, **When** internet connectivity is lost, **Then** ESP activates Access Point mode using last cached server state
2. **Given** ESP device in offline/AP mode, **When** user changes relay states via local UI, **Then** changes are stored locally and persist across reboots
3. **Given** ESP device in offline mode with local state changes, **When** internet connectivity is restored, **Then** ESP syncs local changes to server using last-write-wins conflict resolution
4. **Given** ESP device loses power and restarts, **When** device boots up, **Then** ESP loads last known local state and syncs with server when connection available

### User Story 3 - Role-Based Access Control (Priority: P2)

As a system administrator, I want to enforce strict role-based access control on the server side so that users can only access their assigned ESP devices while admins have full system control.

**Why this priority**: Implements critical security separation where ESP firmware remains completely unaware of users/roles/permissions.

**Independent Test**: Can be tested by verifying that non-admin users cannot access admin functionality and can only operate assigned devices.

**Acceptance Scenarios**:
1. **Given** admin user logs in, **When** admin accesses system, **Then** admin sees all ESP devices and has configuration access
2. **Given** regular user logs in, **When** user accesses system, **Then** user sees only assigned ESP devices and has no configuration access
3. **Given** user attempts to access admin endpoints, **When** request is processed, **Then** server rejects with appropriate authorization error
4. **Given** ESP device receives commands, **When** ESP processes relay control, **Then** ESP uses hardware IDs only and is completely unaware of user/role context

### User Story 4 - Real-time Synchronization (Priority: P2)

As a user, I want near real-time synchronization between ESP devices and the server so that state changes are quickly reflected across all connected clients.

**Why this priority**: Ensures responsive user experience while maintaining system reliability.

**Independent Test**: Can be tested by measuring latency between state changes on ESP and updates in server/database.

**Acceptance Scenarios**:
1. **Given** user changes relay state via web UI, **When** command is sent to server, **Then** server updates database within <200ms
2. **Given** server database is updated, **When** ESP is online, **Then** ESP receives state update within <500ms
3. **Given** multiple clients connected, **When** one client changes relay state, **Then** all connected clients see update within <500ms
4. **Given** ESP changes relay state locally (timer/PIR), **When** ESP processes change, **Then** ESP sends update to server within <200ms

### User Story 5 - Power Loss Recovery (Priority: P2)

As a user, I want the system to maintain relay state integrity across power cycles so that my smart home devices return to expected states after power restoration.

**Why this priority**: Critical for reliability in real-world deployments where power outages occur.

**Independent Test**: Can be tested by simulating power loss and verifying state recovery.

**Acceptance Scenarios**:
1. **Given** ESP device is operating with specific relay states, **When** power is lost, **Then** ESP preserves last known state in local storage
2. **Given** ESP device loses power, **When** power is restored, **Then** ESP boots and loads last known local state
3. **Given** ESP device restores from power loss, **When** internet is available, **Then** ESP synchronizes with server to resolve any conflicts
4. **Given** ESP device restores from power loss offline, **When** internet becomes available later, **Then** ESP syncs with server using last-write-wins strategy

## Edge Cases

- What happens when multiple admins simultaneously rename the same ESP/relay?
- How does system handle ESP firmware updates while maintaining configuration separation?
- What is the behavior when ESP has corrupted local cache but server has valid state?
- How does system handle network partitions where ESP can reach server but server cannot reach ESP (asymmetric connectivity)?
- What happens when ESP clock drifts significantly affecting timestamp-based conflict resolution?
- How does system handle ESP devices with duplicate or conflicting IDs?

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST support multiple ESP devices, each with a unique server-generated identifier
- **FR-002**: System MUST provide an admin-only configuration page for managing ESP identities and relay friendly names
- **FR-003**: Configuration changes (ESP names, relay names) MUST be stored exclusively in Supabase database
- **FR-004**: ESP devices MUST receive updated labels/names from server but MUST NOT use them for internal control logic
- **FR-005**: ESP devices MUST have NO knowledge of admin users, regular users, roles, or permissions
- **FR-006**: ESP devices MUST only handle: relay states (ON/OFF), relay hardware IDs, and synchronization with server
- **FR-007**: ALL role-based access control logic MUST be implemented strictly server-side (Supabase RLS)
- **FR-008**: System MUST implement state synchronization where database is the source of truth for relay states and metadata
- **FR-009**: ESP devices MUST maintain local cache of last known relay states and configuration
- **FR-010**: ESP devices MUST periodically sync with server when online connection is available
- **FR-011**: Server MUST push configuration updates to ESP devices when connected
- **FR-012**: When ESP loses power and restarts, it MUST reload last known state from local storage
- **FR-013**: After power restoration, ESP MUST synchronize with server when connection becomes available
- **FR-014**: When internet connection is lost, ESP MUST activate Access Point mode using last cached server state
- **FR-015**: ESP in Access Point mode MUST continue operating normally using cached state only
- **FR-016**: ESP in Access Point mode MUST NOT expose admin/user logic or authentication
- **FR-017**: When ESP reconnects to internet, it MUST send all pending state updates to server
- **FR-018**: Server MUST update database with ESP state updates and respond with latest configuration
- **FR-019**: ESP MUST update local cache with server response after synchronization
- **FR-020**: Conflict resolution MUST use last-write-wins strategy with server as source of truth
- **FR-021**: System MUST implement real-time synchronization with target latencies: <200ms for command to server, <500ms for server to ESP/client
- **FR-022**: Admin dashboard MUST show full system control with configuration access
- **FR-023**: User dashboard MUST show only assigned ESPs with operational controls only
- **FR-024**: UI MUST use single action button that expands into multiple actions for clean interface
- **FR-025**: Navigation structure MUST be simplified and consistent between admin/user views

### Key Entities

- **ESP Device**: Physical device with unique server-generated ID controlling relay channels
- **Relay**: Individual output channel on ESP device with fixed hardware ID and server-editable friendly name
- **Admin User**: Authorized personnel with full system access and configuration privileges
- **Regular User**: Authorized personnel with access only to assigned ESP devices
- **Supabase Server**: Central database storing device configurations, states, and enforcing access policies
- **Access Point Mode**: Local WiFi hotspot mode activated when ESP has no internet connectivity
- **Local Cache**: Stored relay states and configuration timestamps on ESP device for offline operation
- **State Synchronization**: Bidirectional communication protocol between ESP and Supabase server
- **Friendly Name**: Server-defined, editable label for relays used only in UI/display contexts

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: System can support management of 50+ ESP devices through single admin interface
- **SC-002**: Configuration changes (ESP/relay names) persist in Supabase and survive system restarts with <5s propagation delay
- **SC-003**: ESP devices maintain relay control functionality with <50ms latency for local operations in both online and offline modes
- **SC-004**: System recovers from network loss and restores synchronization within 3 seconds of internet restoration
- **SC-005**: Timestamp-based conflict resolution correctly handles concurrent state changes with <100ms clock drift tolerance
- **SC-006**: Access Point mode activates within 2 seconds of internet loss and deactivates within 2 seconds of internet restoration
- **SC-007**: Local state cache persists across power cycles and maintains relay state accuracy with zero data loss
- **SC-008**: Admin-only configuration page enforces role-based access control with zero false positives/negatives
- **SC-009**: ESP firmware contains zero references to user/admin/access control concepts (verified via code inspection)
- **SC-010**: System correctly implements separation principle: server manages identity/config/roles, ESP manages execution/sync
- **SC-011**: Real-time synchronization meets latency targets: <200ms for UI→server, <500ms for server→ESP, <500ms for ESP→other clients
- **SC-012**: Role-based access control prevents unauthorized access with 100% effectiveness in testing scenarios

## Assumptions

- Administrators and users have secure authentication through Supabase Auth service
- ESP devices have sufficient storage capacity for local state caching (relay states + timestamps + configuration)
- Supabase database is properly configured with Row Level Security (RLS) policies for role enforcement
- Network connectivity monitoring is reliable and detects internet availability accurately within 2-second windows
- ESP devices maintain reasonably accurate timekeeping (±100ms drift) for timestamp-based conflict resolution
- Admin users understand that ESP/relay names in configuration are for UI mapping only and don't affect device logic
- System operates in environments where occasional network loss (minutes to hours) is expected and tolerated
- Power outages are infrequent (<24 hours duration) and ESP local storage remains intact during outages