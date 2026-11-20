# Dynaxis Project Knowledge

C++20 reusable TCP server core, MSA chat platform, cross-platform ImGui client, and staged web control plane. Baseline before capability removal: `refactor@8d48ccf`. Current core API: `4.0.0`.

## Structure and Ownership
- `core/`: TCP/framing/dispatch, bounded queue, scheduler, lifecycle, metrics, storage/discovery contracts (must stay domain- and ops-agnostic).
- `gateway/`: signed login admission, sticky/least-connections routing, TCP bridge, resume alias.
- `server/`: chat domain (rooms, whispers, history, presence, moderation, session continuity) via explicit bootstrap dependencies.
- `infra/`: Redis adapter and signed credential verifier.
- `tools/`: write-behind worker/DLQ helpers, migrations, opcode/wire generators, offline token issuer.
- `client_gui/`: cross-platform ImGui/GLFW/OpenGL client (`Messenger` default + `F12` `Workbench`); `ClientApp` / `MessageTimeline` / `ClientPreferences` have zero ImGui dependency.
- `admin_console/`: read-only/staged control-plane server (`admin_app`) + Vanilla ESM web UI; must not pollute `core/` or mount host Docker socket.
- `docker/`: Linux compose stack (`stack/`) and Prometheus/Grafana (`observability/`).

## Canonical Commands
- Full native build & test: `python scripts/build.py --profile full --config Debug --step all` then `--step test`
- Minimum core check: `python scripts/build.py --profile core --step all` then `--step test`
- Client-only release build: `python scripts/build.py --profile client --config Release --step build`
- Docker stack lifecycle: `pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build` (or `run_full_stack_observability.ps1`)
- Contract & doc checks: `python tools/check_doxygen_coverage.py; python tools/check_core_api_contracts.py --check-boundary; python tools/gen_opcode_docs.py --check; python tools/check_markdown_links.py`

## Non-Negotiable Rules
- All C++ dependencies use Conan 2 (`conan/requirements.txt`, `conanfile.py`, `conan.lock`); never clone or substitute distro C++ libraries in Docker.
- Explicit CMake source lists only (no `file(GLOB)`). `core/` must never depend on `server/`, `gateway/`, `client_gui/`, or `admin_console/`.
- Naming & headers: `.hpp` + `#pragma once`, `snake_case` paths/functions, ASCII code symbols (`docs/naming-conventions.md`).
- Security & privacy: never commit `.env`, never log/display/persist `access_token`, `resume_token`, DB/Redis passwords, or chat plaintext in admin views.
- Session continuity uses PG lease + Redis state; do not reintroduce removed game/UDP/world/Lua capabilities without explicit design.
- Keep `.worktrees/` and reference repositories untouched by normal cleanup.

## Detailed Guides (Read Before Editing)
- [Documentation Index](docs/README.md) — canonical architecture, protocol, and ops entrypoints.
- [Workflow, Verification & Comment Ethos](docs/agent-guides/workflow-and-ethos.md) — proof-complete workflow, Korean explanatory doc style, Doxygen rules, and local `tasks/`/`lessons.md` loop.
- [Frontend & UI Design System](docs/agent-guides/frontend-design-system.md) — Slate Dark tokens, 5-state semantic badges, `tabular-nums`, single-scroll shell rules, CJK IME, and XSS/secret boundaries across `client_gui` and `admin_console`.
- Local-only task index & lessons (gitignored): `tasks/README.md` and `tasks/_shared/lessons.md`.
