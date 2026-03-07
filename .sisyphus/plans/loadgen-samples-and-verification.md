# Loadgen Samples And Verification

## Goal

Implement the two next-step loadgen expansions requested by the user, then re-check the full loadgen validation system and commit if everything is clean.

## Scope

1. Add larger duration/session scenario samples that fit the current harness.
2. Add RUDP policy-comparison evidence around the mixed long sample.
3. Update the scenario catalog and quantitative backlog with exact commands, report paths, and summary lines.
4. Re-run the relevant loadgen verification matrix, inspect reports, and ensure the stack is left in the normal attach env.
5. Create a git commit only after verification and Oracle review are complete.

## Concrete Work Items

- Add scenario JSON files:
  - `tools/loadgen/scenarios/mixed_session_soak_long.json`
  - `tools/loadgen/scenarios/mixed_direct_udp_soak_long.json`
  - `tools/loadgen/scenarios/mixed_direct_rudp_soak_long.json`
- Run baseline validation:
  - HAProxy TCP long sample
  - direct same-gateway UDP long sample
  - direct same-gateway RUDP long sample
- Run policy comparison for `mixed_direct_rudp_soak_long.json` using:
  - `docker/stack/.env.rudp-attach.example`
  - `docker/stack/.env.rudp-fallback.example`
  - `docker/stack/.env.rudp-off.example`
- Record results in:
  - `tools/loadgen/README.md`
  - `docs/tests/loadgen-plan.md`
  - `docs/tests/loadgen-next-steps.md`
  - `tasks/quantitative-validation.md`
  - `tasks/todo.md`
  - `docs/tests.md`
- Re-check the loadgen system with a full scenario matrix and report inspection.
- Collect Oracle review.
- Commit if the verification matrix is clean.

## Constraints

- Keep within current harness limits: non-TCP transports stay `login_only` and `join_room=false`.
- Use direct same-gateway TCP+UDP addressing for UDP/RUDP attach validation.
- Do not revert unrelated user changes.
- Do not commit until verification and Oracle review pass.
