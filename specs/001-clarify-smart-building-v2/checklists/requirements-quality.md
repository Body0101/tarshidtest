# Requirements Quality Checklist: Smart Building System v2

**Purpose**: Validate completeness, clarity, consistency, and measurability of requirements across all 5 user stories (Admin Config, Hybrid Offline/Online, RBAC, Real-time Sync, Power Loss Recovery)
**Created**: 2026-05-17
**Feature**: `/specs/001-clarify-smart-building-v2/spec.md`
**Audience**: QA / Tester
**Depth**: Deep

## Requirement Completeness

- [ ] CHK001 Are requirements defined for the admin configuration page's full CRUD lifecycle (create, read, update, delete) for both ESP identities and relay names? [Completeness, Spec §FR-002]
- [ ] CHK002 Are requirements specified for WebSocket connection lifecycle management (connect, reconnect, timeout, graceful disconnect) between ESP and server? [Gap, Spec §FR-021]
- [ ] CHK003 Are requirements defined for rate limiting behavior on both server-side (per-device) and ESP-side (relay commands)? [Completeness, Spec §Edge Cases]
- [ ] CHK004 Are requirements specified for ESP device deregistration or decommissioning from Supabase? [Gap]
- [ ] CHK005 Are requirements defined for how the system handles duplicate device registration attempts? [Gap, Spec §Edge Cases]
- [ ] CHK006 Are requirements specified for the ESP's local cache eviction policy when storage is full? [Gap, Spec §FR-009]
- [ ] CHK007 Are requirements defined for user account provisioning and deactivation workflows? [Gap, Spec §FR-023, FR-022]
- [ ] CHK008 Are requirements defined for how the admin configuration page handles concurrent admin edits (locking, conflict detection)? [Gap, Spec §Edge Cases]
- [ ] CHK009 Are requirements specified for firmware update orchestration and its interaction with configuration separation? [Gap, Spec §Edge Cases]

## Requirement Clarity

- [ ] CHK010 Is "Access Point mode" clearly defined in terms of network configuration (SSID visibility, IP assignment, client limits, security)? [Clarity, Spec §FR-014]
- [ ] CHK011 Is "periodically sync" (FR-010) quantified with a specific interval, jitter tolerance, or sync trigger condition? [Clarity, Spec §FR-010]
- [ ] CHK012 Is "last known state" (FR-012) clearly defined — does it include relay states only, or also timer configs, PIR mappings, and energy tracking settings? [Clarity, Spec §FR-012]
- [ ] CHK013 Is "pending state updates" (FR-017) clearly scoped — includes queued relay changes only, or also system events logged while offline? [Clarity, Spec §FR-017]
- [ ] CHK014 Is "latest configuration" (FR-018) defined — does it include only relay state, or also friendly names, timer configs, and PIR mappings? [Clarity, Spec §FR-018]
- [ ] CHK015 Is "single action button that expands into multiple actions" (FR-024) specified with enough detail to implement consistently across admin/user dashboards? [Clarity, Spec §FR-024]
- [ ] CHK016 Is "simplified and consistent navigation structure" (FR-025) defined with specific navigation elements, order, and naming conventions? [Clarity, Spec §FR-025]

## Requirement Consistency

- [ ] CHK017 Do FR-004 and FR-005/FR-006 conflict on the scope of data ESP can receive? FR-004 says ESP receives labels/names, FR-006 says ESP only handles states/IDs/sync. Is the boundary clearly resolved? [Conflict, Spec §FR-004 vs FR-006]
- [ ] CHK018 Is SC-002's "<5s propagation delay" consistent with the sync interval assumed by the periodic sync requirements (FR-010)? [Consistency, Spec §SC-002 vs FR-010]
- [ ] CHK019 Is SC-006's "AP mode activates within 2 seconds of internet loss" consistent with the internet failure threshold defined in acceptance scenarios (US2 assumes multi-probe detection)? [Consistency, Spec §SC-006 vs US2 AS-1]
- [ ] CHK020 Do SC-003 (<50ms local operation) and SC-011 (<200ms ESP→server) latency targets conflict with the resource constraints (<180KB heap, <80% CPU) used in the same relay control task? [Consistency, Spec §SC-003 vs SC-011]
- [ ] CHK021 Is "hardware ID" terminology used consistently across all user stories? US1 uses "hardware IDs", US3 uses "hardware IDs only" — are these the same concept? [Consistency]

## Acceptance Criteria Measurability

- [ ] CHK022 Can SC-004 ("recovers from network loss within 3 seconds") be objectively measured — what is the start/end event for this timer? [Measurability, Spec §SC-004]
- [ ] CHK023 Can SC-005 ("<100ms clock drift tolerance") be verified without specialized test equipment? Is the test methodology specified? [Measurability, Spec §SC-005]
- [ ] CHK024 Can SC-007 ("zero data loss") be objectively verified — what constitutes a data loss event? Is there a measurement protocol? [Measurability, Spec §SC-007]
- [ ] CHK025 Can SC-008 ("zero false positives/negatives") be verified — what is the sample size and test coverage for access control scenarios? [Measurability, Spec §SC-008]
- [ ] CHK026 Can SC-009 ("zero references to user/admin/access control concepts") be verified through automated code scanning? Is the scanning methodology (e.g., grep patterns, static analysis) specified? [Measurability, Spec §SC-009]
- [ ] CHK027 Are the latency targets in SC-011 defined as p99, p95, or average measurements? Different percentiles give very different test requirements. [Measurability, Spec §SC-011]

## Scenario Coverage

- [ ] CHK028 Are requirements defined for the alternate flow where admin renames an ESP that is currently offline — is the name stored and queued for delivery? [Coverage, Alternate Flow]
- [ ] CHK029 Are requirements defined for the alternate flow where ESP reconnects but its cached state is older than the server's — is there a full state reconciliation or incremental update? [Coverage, Spec §FR-019]
- [ ] CHK030 Are requirements defined for the exception flow where an admin's session expires mid-configuration edit? [Coverage, Exception Flow]
- [ ] CHK031 Are requirements defined for the recovery flow after a Supabase outage of >1 hour — does ESP's local operation continue indefinitely? [Coverage, Spec §Clarifications, Edge Cases]
- [ ] CHK032 Are requirements defined for the scenario where a non-admin user is assigned to a device that was previously admin-only — is there any state transition requirement? [Coverage, Spec §FR-023]
- [ ] CHK033 Are requirements defined for concurrent command issuance from multiple users (two users toggling the same relay simultaneously)? [Coverage, Spec §FR-020]
- [ ] CHK034 Are requirements defined for ESP behavior when server rejects state update (e.g., token expired, device deactivated)? [Coverage, Exception Flow]

## Edge Case Coverage

- [ ] CHK035 Is the behavior specified when ESP has corrupted local cache but server has valid state? [Gap, Spec §Edge Cases]
- [ ] CHK036 Is the behavior specified for asymmetric network connectivity (ESP can reach server, server cannot reach ESP)? [Edge Case, Spec §Clarifications → WebSocket persistent connection]
- [ ] CHK037 Is the behavior specified when ESP clock drifts significantly affecting timestamp-based conflict resolution? [Edge Case, Spec §Edge Cases]
- [ ] CHK038 Is the behavior specified for ESP devices with duplicate or conflicting IDs? [Edge Case, Spec §Edge Cases]
- [ ] CHK039 Is the behavior specified when multiple admins rename the same ESP/relay simultaneously? [Edge Case, Spec §Edge Cases]
- [ ] CHK040 Is the behavior specified for ESP firmware updates while maintaining configuration separation? [Edge Case, Spec §Edge Cases]
- [ ] CHK041 Is the behavior specified when Supabase (external service) is unavailable for extended periods — is the exponential backoff strategy defined with parameters? [Edge Case, Spec §Edge Cases]
- [ ] CHK042 Is the behavior specified for sudden burst of state updates from 100+ ESPs simultaneously — is the per-device rate limit quantified? [Edge Case, Spec §Edge Cases]
- [ ] CHK043 Is the behavior specified when an ESP's power is lost mid-sync (after sending states but before receiving server acknowledgment)? [Gap, Spec §FR-017, FR-018]
- [ ] CHK044 Is the behavior specified when NVS (ESP local storage) write fails due to flash wear or corruption? [Gap, Spec §FR-009]

## Non-Functional Requirements

- [ ] CHK045 Are performance requirements specified under degraded conditions (high CPU, low heap, WiFi interference)? [Coverage, Spec §Plan — Performance Goals]
- [ ] CHK046 Are security requirements specified for the AP mode communication (no TLS)? FR-016 states no auth, but is there any protection against MAC spoofing or local replay? [Coverage, Spec §FR-016]
- [ ] CHK047 Are scalability requirements specified for the web dashboard with 50+ concurrent users polling device states? [Coverage, Spec §SC-001]
- [ ] CHK048 Are observability requirements specified for debugging sync issues — is structured logging format defined with required fields? [Coverage, Spec §Clarifications]
- [ ] CHK049 Are requirements specified for the Supabase Realtime channel capacity — can it handle 100 devices each updating once per second? [Gap, Spec §Clarifications → 1 update/sec/device]

## Dependencies & Assumptions

- [ ] CHK050 Is the assumption that "Supabase Auth service provides secure authentication" validated — are there requirements for auth token refresh, expiry, and revocation? [Assumption, Spec §Assumptions]
- [ ] CHK051 Is the assumption that "ESP devices maintain ±100ms clock drift" validated against typical ESP32 RTC performance without external RTC chip? [Assumption, Spec §Assumptions]
- [ ] CHK052 Is the assumption that "network connectivity monitoring detects availability within 2-second windows" validated against common ESP32 WiFi behavior during signal fades? [Assumption, Spec §Assumptions]
- [ ] CHK053 Is the dependency on "Supabase API availability" documented with SLAs, expected downtimes, and degradation modes? [Dependency, Gap]
- [ ] CHK054 Is the dependency on "ESP32 LittleFS/NVS reliability" documented with expected write cycle limits and failure modes? [Dependency, Gap]

## Ambiguities & Conflicts

- [ ] CHK055 Is "server updates database within <200ms" (US4 AS-1) ambiguous — does this mean the database write completes, or the change is replicated via Realtime? [Ambiguity, Spec §US4 AS-1]
- [ ] CHK056 Is "ESP receives state update within <500ms" (US4 AS-2) ambiguous — does this mean the ESP receives the command, or the ESP acknowledges execution? [Ambiguity, Spec §US4 AS-2]
- [ ] CHK057 Is "system recovers from network loss" (SC-004) ambiguous — does recovery mean ESP re-establishes AP mode, or cloud sync resumes? [Ambiguity, Spec §SC-004]
- [ ] CHK058 Is "multiple clients" (US4 AS-3) ambiguous — are these web UI clients, ESPs, or both? Different clients have different synchronization requirements. [Ambiguity, Spec §US4 AS-3]

## Notes

- Check items off as completed: `[x]`
- Reference spec sections using [Spec §X] notation
- Mark identified gaps with explicit attention for implementation
- Items that reveal hard issues should escalate to spec clarification
