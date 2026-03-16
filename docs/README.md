# Documentation Index

This is the canonical entrypoint for repository documentation.
Historical proposal/report docs were pruned from the active docs tree; use the files below as the live source of truth.

## Canonical Docs

- `README.md` - top-level project overview and entry links.
- `docs/getting-started.md` - local setup and basic run flow.
- `docs/rename_boundary.md` - canonical rename policy, affected surfaces, and allowed legacy record.
- `docs/build.md` - build guidance.
- `docs/configuration.md` - runtime config surface.
- `docs/core-design.md` - core/runtime layering and composition ownership notes.
- `docs/tests.md` - verification/test entrypoint.
- `docs/protocol.md` - protocol overview.
- `docs/protocol/rudp.md` - current RUDP/dual-transport behavior.
- `docs/db/write-behind.md` - write-behind runtime and ops behavior.
- `docs/ops/observability.md` - observability setup and dashboards.
- `docs/ops/gateway-and-lb.md` - gateway/load-balancer operations.
- `docs/ops/runbook.md` - current runbook-oriented operational notes.
- `docs/ops/engine-readiness-baseline.md` - tracked checkpoint ledger for phase-by-phase engine readiness proof.
- `docs/ops/engine-readiness-decision.md` - Phase 4 decision record for common blockers and branch readiness.
- `docs/ops/engine-branch-cut-criteria.md` - Phase 5 criteria for FPS/MMORPG branch-cut timing and order.
- `docs/ops/engine-roadmap-fps-charter.md` - prepared branch-start charter for the first FPS-oriented transport/runtime tranche.
- `docs/ops/engine-roadmap-session-continuity-charter.md` - branch-start charter for the capability-first session continuity tranche.
- `docs/ops/engine-roadmap-session-continuity-first-tranche-closure.md` - closure decision for the first session continuity tranche.
- `docs/ops/engine-roadmap-mmorpg-charter.md` - prepared narrow charter for the next MMORPG-oriented branch after the continuity tranche merges.
- `docs/ops/engine-roadmap-mmorpg-first-tranche-closure.md` - closure decision for the first MMORPG admission/residency tranche.
- `docs/ops/engine-roadmap-mmorpg-world-lifecycle-charter.md` - next tranche charter for world lifecycle/control-plane orchestration on the MMORPG branch.
- `docs/ops/session-continuity-contract.md` - current continuity ownership, resume rules, and restart proof targets.
- `docs/ops/mmorpg-world-residency-contract.md` - current world admission, residency owner boundary, and safe fallback rules on the MMORPG branch.
- `docs/ops/mmorpg-desired-topology-contract.md` - future pool-based desired-topology contract for live scaling work on the MMORPG branch.
- `docs/ops/mmorpg-scaling-orchestration-charter.md` - orchestrator-agnostic charter for a future live scaling tranche on the MMORPG branch.
- `docs/core-api/overview.md` - current public core API docs entrypoint.

## Tool-Specific Canonical Docs

- `tools/loadgen/README.md` - unified load generator usage and scenario catalog.
- `tools/admin_app/README.md` - live admin control-plane behavior and endpoints.
- `tools/migrations/README.md` - migration runner usage.
- `tools/wb_worker/README.md` - write-behind worker runtime guidance.
