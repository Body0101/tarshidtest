# Implementation Plan: [FEATURE]

**Branch**: `[###-feature-name]` | **Date**: [DATE] | **Spec**: [link]

**Input**: Feature specification from `/specs/[###-feature-name]/spec.md`

**Note**: This template is filled in by the `/speckit.plan` command. See `.specify/templates/plan-template.md` for the execution workflow.

## Summary

[Extract from feature spec: primary requirement + technical approach from research]

## Technical Context

<!--
  ACTION REQUIRED: Replace the content in this section with the technical details
  for the project. The structure here is presented in advisory capacity to guide
  the iteration process.
-->

**Language/Version**: [e.g., Python 3.11, Swift 5.9, Rust 1.75 or NEEDS CLARIFICATION]

**Primary Dependencies**: [e.g., FastAPI, UIKit, LLVM or NEEDS CLARIFICATION]

**Storage**: [if applicable, e.g., PostgreSQL, CoreData, files or N/A]

**Testing**: [e.g., pytest, XCTest, cargo test or NEEDS CLARIFICATION]

**Target Platform**: [e.g., Linux server, iOS 15+, WASM or NEEDS CLARIFICATION]

**Project Type**: [e.g., library/cli/web-service/mobile-app/compiler/desktop-app or NEEDS CLARIFICATION]

**Performance Goals**: [domain-specific, e.g., 1000 req/s, 10k lines/sec, 60 fps or NEEDS CLARIFICATION]

**Constraints**: [domain-specific, e.g., <200ms p95, <100MB memory, offline-capable or NEEDS CLARIFICATION]

**Scale/Scope**: [domain-specific, e.g., 10k users, 1M LOC, 50 screens or NEEDS CLARIFICATION]

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

1. Code Quality Excellence ✓\n   - Embedded systems: Resource constraints, deterministic behavior, clean architecture\n2. Rigorous Testing Standards ✓\n   - Hardware-in-the-loop testing, ESP32 validation, 80% unit test coverage\n3. Consistent User Experience ✓\n   - Dual-mode consistency (local/online), WCAG 2.1 AA accessibility\n4. Performance Requirements ✓\n   - <50ms local control, <100ms UI, <500ms cloud, <180KB heap, <80% CPU\n5. Development Workflow ✓\n   - GitHub Flow, PR reviews, CI/CD, semantic versioning\n6. Quality Gates ✓\n   - Tests pass, review approved, benchmarks met, security scanned

## Project Structure

### Documentation (this feature)

```text
specs/[###-feature]/
â”œâ”€â”€ plan.md              # This file (/speckit.plan command output)
â”œâ”€â”€ research.md          # Phase 0 output (/speckit.plan command)
â”œâ”€â”€ data-model.md        # Phase 1 output (/speckit.plan command)
â”œâ”€â”€ quickstart.md        # Phase 1 output (/speckit.plan command)
â”œâ”€â”€ contracts/           # Phase 1 output (/speckit.plan command)
â””â”€â”€ tasks.md             # Phase 2 output (/speckit.tasks command - NOT created by /speckit.plan)
```

### Source Code (repository root)
<!--
  ACTION REQUIRED: Replace the placeholder tree below with the concrete layout
  for this feature. Delete unused options and expand the chosen structure with
  real paths (e.g., apps/admin, packages/something). The delivered plan must
  not include Option labels.
-->

```text
# [REMOVE IF UNUSED] Option 1: Single project (DEFAULT)
src/
â”œâ”€â”€ models/
â”œâ”€â”€ services/
â”œâ”€â”€ cli/
â””â”€â”€ lib/

tests/
â”œâ”€â”€ contract/
â”œâ”€â”€ integration/
â””â”€â”€ unit/

# [REMOVE IF UNUSED] Option 2: Web application (when "frontend" + "backend" detected)
backend/
â”œâ”€â”€ src/
â”‚   â”œâ”€â”€ models/
â”‚   â”œâ”€â”€ services/
â”‚   â””â”€â”€ api/
â””â”€â”€ tests/

frontend/
â”œâ”€â”€ src/
â”‚   â”œâ”€â”€ components/
â”‚   â”œâ”€â”€ pages/
â”‚   â””â”€â”€ services/
â””â”€â”€ tests/

# [REMOVE IF UNUSED] Option 3: Mobile + API (when "iOS/Android" detected)
api/
â””â”€â”€ [same as backend above]

ios/ or android/
â””â”€â”€ [platform-specific structure: feature modules, UI flows, platform tests]
```

**Structure Decision**: [Document the selected structure and reference the real
directories captured above]

## Complexity Tracking

> **Fill ONLY if Constitution Check has violations that must be justified**

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| [e.g., 4th project] | [current need] | [why 3 projects insufficient] |
| [e.g., Repository pattern] | [specific problem] | [why direct DB access insufficient] |

