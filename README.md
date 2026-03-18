# Dynaxis

Dynaxis is a C++20 server-core repository with a concrete distributed chat stack built on top of it.
The default runtime shape is `HAProxy -> gateway_app -> server_app`, with Redis and Postgres backing state, fanout, and write-behind persistence.

## What Lives Here

- `core/`: reusable runtime/core library (`server_core`)
- `gateway/`: edge session routing, stickiness, direct UDP/RUDP transport handling
- `server/`: chat/runtime logic, continuity, world residency, FPS substrate
- `tools/`: admin, loadgen, migration, and write-behind tools
- `docker/stack/`: the standard local/full-stack runtime entrypoint

## Architecture Summary

- TCP entry traffic is distributed by an external L4 load balancer such as HAProxy.
- `gateway_app` terminates client sessions, authenticates first-hop traffic, and routes to backends using Redis-backed stickiness and least-load selection.
- `server_app` owns chat/runtime state, continuity state, world residency, and the neutral FPS fixed-step substrate.
- Write-behind persistence flows through Redis Streams into `wb_worker`, which flushes durable events to Postgres.

For control-plane and failure-path diagrams, use [docs/ops/architecture-diagrams.md](docs/ops/architecture-diagrams.md).

## Start Here

- Setup and first run: [docs/getting-started.md](docs/getting-started.md)
- Build guidance: [docs/build.md](docs/build.md)
- Runtime configuration: [docs/configuration.md](docs/configuration.md)
- Tests and verification: [docs/tests.md](docs/tests.md)
- Protocol and state flow: [docs/protocol.md](docs/protocol.md), [docs/protocol/snapshot.md](docs/protocol/snapshot.md)
- Canonical documentation index: [docs/README.md](docs/README.md)

## Current Runtime Contracts

- Engine readiness baseline: [docs/ops/engine-readiness-baseline.md](docs/ops/engine-readiness-baseline.md)
- Session continuity: [docs/ops/session-continuity-contract.md](docs/ops/session-continuity-contract.md)
- World residency/lifecycle: [docs/ops/mmorpg-world-residency-contract.md](docs/ops/mmorpg-world-residency-contract.md)
- FPS transport/runtime substrate: [docs/ops/fps-runtime-contract.md](docs/ops/fps-runtime-contract.md)
- Observability and operations: [docs/ops/observability.md](docs/ops/observability.md), [docs/ops/runbook.md](docs/ops/runbook.md)

## Subprojects

| Project | Path | Notes |
| --- | --- | --- |
| Core | [core/README.md](core/README.md) | public runtime/core library surface |
| Server | [server/README.md](server/README.md) | app/runtime behavior and service notes |
| Gateway | [gateway/README.md](gateway/README.md) | edge routing and transport notes |
| Client GUI | [client_gui/README.md](client_gui/README.md) | Windows ImGui development client |
| Tools | [tools/README.md](tools/README.md) | tool entry index and per-tool docs |

## Quick Commands

```powershell
pwsh scripts/build.ps1 -Config Debug
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build
ctest --preset windows-test
python tools/gen_opcode_docs.py --check
```

Use `pwsh scripts/deploy_docker.ps1 -Action down` to tear the stack down again.
