# Dynaxis project knowledge

C++20 reusable TCP server core and MSA chat foundation. Standard runtime is Linux Docker; Windows and Linux build the same C++ ImGui client. Baseline before capability removal: refactor@8d48ccf. Current API: 4.0.0.

## Structure and ownership
- core: TCP/framing/dispatch, bounded queue, scheduler, buffers, lifecycle, metrics; MSA protocol/discovery/storage execution integration.
- gateway: signed login admission, discovery/sticky/load routing, TCP bridge, resume alias/locator, timeout/retry/circuit guards.
- server: chat users/rooms/messages/whispers/history/presence/moderation/session continuity; explicit bootstrap dependencies.
- infra: Redis adapter and signed credential adapter.
- tools: write-behind worker/helpers, migrations and protocol generators, offline credential issuer.
- client_gui: replaceable development UI; NetClient has no ImGui/GLFW dependency. GUI stack: ImGui/GLFW/OpenGL.
- docker/stack: HAProxy, gateway/server instances, Redis/Postgres, worker/migrator; optional observability.
- docs/README.md: canonical entrypoint. tasks/: ignored local plans and evidence.

## Commands
- Build: python scripts/build.py --profile full --config Debug --step all
- Tests: python scripts/build.py --profile full --config Debug --step test
- Runtime: pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build
- Minimum core: python scripts/build.py --profile core --step all; python scripts/build.py --profile core --step test
- Contracts: python tools/check_doxygen_coverage.py; python tools/check_core_api_contracts.py --check-boundary; python tools/gen_opcode_docs.py --check

## Rules
- Conan 2 version: conan/requirements.txt. All C++ dependencies use conanfile.py + conan.lock on Windows/Linux/Docker.
- scripts/build.py is the common native entry; deps/configure/build/test are separate steps. Build trees live under build/<os>/<profile>/<config>.
- OS graphics SDKs/drivers and compiler are platform prerequisites; do not substitute distro C++ libraries or clone dependency sources in Docker.
- Explicit CMake source lists; no file(GLOB). No core dependency on server/gateway implementation.
- Use scripts/deploy_docker.ps1 or run_full_stack_observability.ps1 for Docker runtime; no alternative wrapper.
- Update Doxygen for public/runtime changes. Validate coverage before completion.
- .hpp/#pragma once, snake_case paths/functions, ASCII code symbols; docs/naming-conventions.md.
- Never commit .env or log credentials/personal data. Native gateway/server default signed auth; local compose explicitly development.
- Session restoration uses PG lease and Redis room state. No world ownership/migration policy.
- Game/realtime/UDP/RUDP, world/provider, Lua/plugin and management-console capabilities were removed. Do not reintroduce without a concrete use case and design.
- Keep separate historical worktrees and reference repositories untouched by normal cleanup.

## Workflow Orchestration

### 1. Self-Improvement Loop
- After ANY correction from the user: update `tasks/_shared/lessons.md` with the pattern
- Write rules for yourself that prevent the same mistake
- Ruthlessly iterate on these lessons until mistake rate drops
- Review lessons at session start for relevant project

### 2. Verification Before Done
- Never mark a task complete without proving it works
- Diff behavior between main and your changes when relevant
- Ask yourself: "Would a staff engineer approve this?"
- Run tests, check logs, demonstrate correctness
- If public headers, exported contracts, or core runtime surfaces changed, include Doxygen coverage validation in the proof set instead of treating comments as optional cleanup

### 3. Demand Elegance (Balanced)
- For non-trivial changes: pause and ask "is there a more elegant way?"
- If a fix feels hacky: "Knowing everything I know now, implement the elegant solution"
- Skip this for simple, obvious fixes - don't over-engineer
- Challenge your own work before presenting it

### 4. Autonomous Bug Fixing
- When given a bug report: just fix it. Don't ask for hand-holding
- Point at logs, errors, failing tests - then resolve them
- Zero context switching required from the user
- Go fix failing CI tests without being told how

## Task Management

1. ** Plan First **: Write plan to `tasks/README.md` and the relevant local task note with checkable items
2. ** Verify Plan **: Check in before starting implementation
3. ** Track Progress **: Mark items complete as you go
4. ** Explain Changes **: High-level summary at each step
5. ** Document Results **: Add review section to the relevant local task note
6. ** Capture Lessons **: Update `tasks/_shared/lessons.md` after corrections

### Local Task Workspace Shape
- `tasks/` is local-only and gitignored; do not treat it as repo source of truth.
- Use `tasks/README.md` as the local workspace index.
- Preferred shape: `tasks/<domain>/<topic>/todo.md` with optional sibling `notes.md`.
- Put cross-cutting rules and reusable snippets under `tasks/_shared/`.

## Core Principles

- ** Simplicity First **: Make every change as simple as possible. Impact minimal code.
- ** No Laziness **: Find root causes. No temporary fixes. Senior developer standards.
- ** Minimal Impact **: Changes should only touch what's necessary. Avoid introducing bugs.
