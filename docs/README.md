# Documentation Index

This is the canonical entrypoint for repository documentation.
Only current-state guides, contracts, and reference docs belong here. Historical branch/tranche notes are intentionally kept in git history instead of the active docs tree.

## Start Here

- `README.md` - repository overview and top-level entry links
- `docs/getting-started.md` - local setup and first-run flow
- `docs/build.md` - build and environment guidance
- `docs/configuration.md` - runtime configuration surface
- `docs/tests.md` - verification entrypoint and local preflight gates
- `docs/rename_boundary.md` - rename policy and allowed legacy record
- `docs/naming-conventions.md` - naming and commenting conventions

## Runtime And Operations

- `docs/core-design.md` - core/runtime layering and ownership
- `docs/protocol.md` - protocol overview
- `docs/protocol/snapshot.md` - room snapshot, recent-history cache, and join-to-fanout flow
- `docs/protocol/rudp.md` - current UDP/RUDP behavior
- `docs/db/architecture.md` - current DB/runtime storage architecture
- `docs/db/redis-strategy.md` - current Redis cache/session/fanout strategy
- `docs/db/migrations.md` - current migration entrypoint
- `docs/db/write-behind.md` - write-behind runtime and operations
- `docs/ops/observability.md` - observability setup and dashboards
- `docs/ops/gateway-and-lb.md` - gateway and load-balancer operations
- `docs/ops/runbook.md` - current runbook-oriented operational notes
- `docs/ops/architecture-diagrams.md` - control-plane and failure-path diagrams
- `docs/ops/engine-readiness-baseline.md` - accepted common-baseline checkpoint ledger
- `docs/ops/engine-readiness-decision.md` - accepted baseline decision and downstream ownership notes
- `docs/ops/quantified-release-evidence.md` - Phase 5 release-evidence inventory, artifact paths, and provisional thresholds
- `docs/ops/worlds-kubernetes-localdev.md` - topology-driven Kubernetes local/dev harness entrypoint and limits
- `docs/ops/session-continuity-contract.md` - current continuity ownership and restore rules
- `docs/ops/mmorpg-world-residency-contract.md` - current world residency/lifecycle/runtime contract
- `docs/ops/mmorpg-desired-topology-contract.md` - current desired-topology, actuation, and orchestration contract
- `docs/ops/realtime-runtime-contract.md` - current realtime transport/runtime substrate contract

## Extensibility

- `docs/extensibility/overview.md` - current extensibility capability overview and entrypoints
- `docs/extensibility/governance.md` - ownership and compatibility rules
- `docs/extensibility/control-plane-api.md` - admin/control-plane API contract
- `docs/extensibility/conflict-policy.md` - rollout/conflict policy
- `docs/extensibility/plugin-quickstart.md` - plugin quickstart
- `docs/extensibility/lua-quickstart.md` - Lua quickstart
- `docs/extensibility/recipes.md` - implementation and rollout recipes

## Core API

- `docs/core-api/overview.md` - public `server_core` package surface map and reading order
- `docs/core-api/quickstart.md` - minimal consumer setup and installed-package flow
- `docs/core-api/compatibility-policy.md` - stable/transitional governance and versioning rules
- `docs/core-api/checklists.md` - PR/release gates and verification commands
- `docs/core-api/adoption-cutover.md` - current stable/transitional/internal cutover status
- `docs/core-api/extensions.md` - extensibility-related public API notes

## Tooling

- `tools/README.md` - tool entry index
- `tools/loadgen/README.md` - load generator scenarios and transport proof catalog
- `tools/admin_app/README.md` - admin control-plane behavior and endpoints
- `tools/migrations/README.md` - migration runner usage
- `tools/wb_worker/README.md` - write-behind worker runtime guidance
