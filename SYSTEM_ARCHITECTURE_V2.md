# Smart Building System v2 Architecture

## Overview
This document describes the updated architecture for the Smart Building System v2, which introduces strict separation between server-side management logic and ESP runtime behavior, robust offline/online synchronization, and admin-only configuration controls.

## Core Architectural Principles

### 1. Strict Separation of Concerns
- **Server Responsibilities**: User/role management, configuration storage, access control, real-time synchronization logic
- **ESP Responsibilities**: Relay control, local execution, state caching, synchronization handling
- **Zero ESP Knowledge**: ESP firmware contains no awareness of users, roles, permissions, or admin concepts

### 2. Server as Source of Truth
- Supabase database is the authoritative source for:
  - Device identities and metadata
  - Relay friendly names (UI labels only)
  - User roles and access permissions
  - Configuration data
- ESP treats server data as guidance for UI/display only, never for control logic

### 3. Local-First Operation with Hybrid Sync
- ESP maintains full functionality offline using cached state
- Synchronization occurs when network is available
- Conflict resolution follows last-write-wins with server as final arbiter

### 4. Role-Based Access Control (RBAC) - Server Side Only
- All authentication, authorization, and role enforcement happens in Supabase
- ESP receives only device-specific commands and configuration
- Zero role/permission logic in ESP firmware

## System Components

### 1. ESP32 Device Layer (Unchanged Core Functionality)
The ESP32 firmware maintains its core responsibilities:
- **ControlEngine**: Relay state management (ON/OFF), timer execution, PIR processing
- **StorageLayer**: Local caching of relay states and configuration
- **WebPortal**: Local HTTP/WebSocket server for offline access point mode
- **CloudSyncService**: Handles synchronization with Supabase when online
- **TimeKeeper**: Manages time synchronization (NTP/browser/fallback)

#### Key ESP Responsibilities:
- Maintain relay states using hardware IDs only
- Cache last known server configuration (names for UI mapping)
- Execute local control logic (timers, PIR, manual control)
- Synchronize state changes with server when online
- Apply server-sent configuration updates to local cache
- Operate in Access Point mode when offline
- Never enforce access control or user roles

### 2. Supabase Server Layer
The Supabase backend provides:
- **Authentication**: Supabase Auth for user management
- **Authorization**: Row Level Security (RLS) policies for access control
- **Data Storage**: Device configurations, relay states, user permissions
- **Real-time Updates**: Realtime subscriptions for live UI updates
- **Remote Procedure Calls (RPCs)**: Secure interfaces for ESP-server communication

#### Key Server Responsibilities:
- Store and manage ESP device identities (server-generated UUIDs)
- Maintain relay friendly names (editable by admins)
- Enforce role-based access control (admin/user/device-specific)
- Store authoritative relay states (with ESP updates)
- Provide real-time synchronization to connected clients
- Validate and execute ESP-sent state updates
- Push configuration changes to ESPs when connected
- Maintain audit trail of all device events

### 3. Web Dashboard Layers
Two distinct web interfaces serve different user types:

#### Admin Dashboard (`online/admin.html`)
- Full system configuration capabilities
- Device management (rename ESPs, relays)
- User and role management
- System monitoring and analytics
- Access to all ESP devices regardless of assignment

#### User Dashboard (`online/simple.html` or device-specific views)
- Operational controls only (relay on/off/timer)
- View assigned ESP devices only
- No configuration access
- Simplified interface focused on device operation

## Data Flow Architecture

### 1. Device-to-Server Synchronization
```
ESP Local State Change → 
  ControlEngine detects change →
  StorageLayer updates local cache →
  CloudSyncService queues state update →
  (When online) HTTPS POST to device_upsert_state() RPC →
  Supabase updates smart_home_device_states →
  Realtime broadcast to connected clients
```

### 2. Server-to-Device Configuration Updates
```
Admin changes relay name via UI →
  Supabase updates smart_home_devices/relay_names →
  Realtime broadcast to admin dashboard →
  (When ESP polls/online) CloudSyncService detects dirty flag →
  HTTPS GET request for latest configuration →
  Supabase returns current names/settings →
  ESP updates local cache for UI mapping only
```

### 3. Command Flow (Server-to-Device)
```
User issues command via dashboard →
  Supabase Auth validates user →
  RLS checks device permissions →
  Insert into smart_home_remote_commands →
  ESP CloudSyncService claims command via device_claim_commands() →
  ControlEngine executes command using hardware IDs →
  ESP marks command done/failed via device_finish_command() →
  Dashboard receives realtime update
```

### 4. Offline Operation Flow
```
Lost internet detected →
  networkTask triggers AP mode activation →
  WebPortal begins serving local interface →
  ESP continues using cached state from StorageLayer →
  Local UI changes affect only local cache →
  ControlEngine executes relays based on local state →
  (When internet returns) Synchronization resumes
```

## Database Schema Enhancements

### Existing Tables (Extended)
1. **smart_home_devices**
   - Added: `server_generated_id` (UUID, unique)
   - Added: `device_name` (admin-editable friendly name)
   - Added: `firmware_version` (for OTA compatibility tracking)
   - Added: `last_sync_epoch` (timestamp of last ESP-server sync)
   - Added: `is_online` (real-time connectivity status)

2. **smart_home_device_states**
   - Added: `relay_names` (JSONB mapping of hardware_id → friendly_name)
   - Added: `configuration_version` (incremental for cache invalidation)
   - Added: `update_source` (ESP/server to track origin)

3. **smart_home_device_memberships**
   - Enhanced RLS policies for stricter device-user assignment enforcement

### New Tables
1. **device_configuration_cache**
   - Tracks per-ESP configuration version for efficient sync
   - Fields: device_id, config_version, last_updated_epoch

2. **device_sync_log**
   - Audit trail of all synchronization events
   - Fields: device_id, timestamp, direction (up/down), payload_size, status

3. **admin_audit_log**
   - Tracks all administrative actions for compliance
   - Fields: admin_id, action_type, target_device, timestamp, details

## Communication Protocols

### 1. ESP-to-Server HTTPS API
- **Endpoint**: `/rest/rpc/device_upsert_state`
  - Purpose: ESP reports state changes
  - Auth: Device token (SHA-256 hashed)
  - Payload: `{device_id, state_json, timestamp, config_version}`
  
- **Endpoint**: `/rest/rpc/device_fetch_config`
  - Purpose: ESP requests latest configuration
  - Auth: Device token
  - Response: `{relay_names, device_name, config_version, server_timestamp}`

### 2. Server-to-ESP Realtime Updates
- **Channel**: Supabase Realtime on `smart_home_device_states`
- **Filter**: Device-specific rows only
- **Throttling**: Minimum 500ms between updates to same device
- **Content**: State snapshot and configuration deltas

### 3. WebSocket (Local AP Mode)
- Unchanged from existing implementation for local control
- Used only when ESP is in Access Point mode
- No authentication required in AP mode (by design constraint)

## Security Architecture

### Device Authentication
- ESP authenticates to server using pre-shared `CLOUD_COMMAND_TOKEN`
- Token stored as SHA-256 hash in database (never plaintext)
- Device-specific RPC functions validate token hash
- Compromised devices can be disabled by invalidating token

### User Authentication
- Admin and regular users authenticate via Supabase Auth
- Role-based access enforced through RLS policies
- Session management handled by Supabase
- Sensitive operations require re-authentication

### Data Protection
- All communication over HTTPS/TLS
- Sensitive data encrypted at rest in Supabase
- API rate limiting to prevent abuse
- Input validation on all ESP-server interfaces

## Operational Considerations

### Power Loss Handling
1. ESP maintains relay state in NVS through StorageLayer
2. On power restoration:
   - Boot loader checks power loss flag
   - Loads last known state from NVS
   - Attempts server synchronization if online
   - Continues local operation using cached state

### Network Fluctuation Handling
- Exponential backoff for failed sync attempts
- Local queue for storing updates during temporary outages
- Configurable sync intervals (balance freshness vs. battery life)
- Automatic AP mode activation on connectivity loss

### Firmware Updates
- Configuration data (names) preserved during OTA updates
- Device ID remains constant across firmware versions
- Migration scripts handle schema changes gracefully
- Backward compatibility maintained for older firmware versions

## Performance and Resource Constraints

### ESP32 Resource Budget
- **Memory**: <180KB heap usage during normal operation
- **CPU**: <80% average load to maintain real-time control
- **Bluetooth/WiFi**: Shared resource with time slicing
- **Storage**: <50KB NVS for device state + configuration
- **Queue Sizes**: Bounded to prevent memory exhaustion

### Latency Requirements (From Constitution)
- Local relay control: <50ms
- Local UI response: <100ms  
- Server-to-ESP command: <500ms
- Server-to-client update: <500ms
- ESP-to-server state report: <200ms

### Scalability Targets
- Support 50+ concurrent ESP devices per Supabase instance
- Handle 100+ simultaneous user connections
- Sustain 1 state update per second per device average
- Burst capacity of 10 updates/second per device during configuration changes

## Implementation Phases

### Phase 1: Foundation
- Enhance device identification with server-generated UUIDs
- Add friendly name fields to device/relay schemas
- Implement basic ESP-server configuration sync
- Update admin UI for device/relay renaming

### Phase 2: Synchronization Robustness
- Implement bidirectional state sync with conflict resolution
- Add local caching and queueing mechanisms
- Develop power loss recovery procedures
- Create network fluctuation handling

### Phase 3: Security & Access Control
- Implement Row Level Security policies for device access
- Create role-based dashboard views (admin/user)
- Add audit logging for administrative actions
- Validate zero ESP knowledge of users/roles

### Phase 4: Performance Optimization
- Tune synchronization intervals and thresholds
- Implement realtime update throttling
- Optimize database queries for large device sets
- Add performance monitoring and alerting

### Phase 5: Validation & Monitoring
- Create comprehensive test suite for sync scenarios
- Implement health checks and monitoring dashboards
- Add failure simulation and recovery testing
- Document operational procedures and runbooks

## Dependencies and Integration Points

### Internal Dependencies
- Existing ControlEngine, StorageLayer, WebPortal, TimeKeeper modules
- Enhanced CloudSyncService for bidirectional sync
- Updated data models in NVS and LittleFS
- Modified WebPortal for local AP-only operation

### External Dependencies
- Supabase PostgreSQL database with Realtime extension
- Supabase Auth service for user management
- HTTP/HTTPS for ESP-server communication
- NTP servers for time synchronization (online mode)

### Backward Compatibility
- Existing ESP firmware continues to function in offline mode
- New features require firmware update to v2+
- Database schema changes are additive
- Admin UI gracefully handles mixed v1/v2 device populations

## Conclusion
This architecture successfully implements the requirements for a scalable IoT management system with:
- Strict separation between server management logic and ESP runtime behavior
- Robust offline/online synchronization with conflict resolution
- Admin-only configuration controls that don't affect ESP operation
- Role-based access control enforced entirely on the server side
- Local-first operation ensuring reliability during network outages
- Clear data ownership with server as source of truth for configuration

The design maintains all existing ESP functionality while adding the necessary enterprise-grade features for multi-device smart building management.