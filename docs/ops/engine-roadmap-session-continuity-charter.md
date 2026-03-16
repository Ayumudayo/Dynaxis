# Engine Roadmap Session Continuity Charter

This document is the branch-start charter for `engine-roadmap-session-continuity`.

## Branch Cut

- branch: `engine-roadmap-session-continuity`
- cut from: `engine-readiness-baseline`
- cut commit: `bbdf3a5`
- baseline decision: `docs/ops/engine-readiness-decision.md`
- branch-cut criteria: `docs/ops/engine-branch-cut-criteria.md`

## Current Status

- first tranche status: complete
- closure decision: `docs/ops/engine-roadmap-session-continuity-first-tranche-closure.md`
- next recommended genre branch after merge: `engine-roadmap-mmorpg`

## Why This Branch Exists

- The accepted common baseline is stronger in TCP control flow, lifecycle/readiness, Redis/Postgres dependency handling, write-behind, and admin/control-plane behavior than in gameplay-grade realtime transport.
- The immediate next step is therefore session/persistence continuity rather than FPS transport expansion.
- Because the runtime is still fundamentally a chat/control stack, this branch stays capability-first instead of pretending the current slice is already a full MMORPG branch.

## First-Tranche Theme

- session continuity before genre framing

This tranche exists to move the stack from bounded restart recovery to resumable player continuity.
It does not attempt full MMORPG world orchestration in one step.

## First-Tranche Scope

### 1. Resumable session identity contract

- define a reconnect/resume token or lease model that can survive bounded gateway/server restarts
- distinguish transport session identity from logical player session identity
- make resume semantics explicit enough for both gateway and server to enforce consistently

### 2. Continuity state model

- define the minimum continuity state that must survive reconnect:
  - effective user identity
  - current room or equivalent lightweight location
  - recent continuity timestamp / lease expiry
- define where that state lives and which component owns each write path

### 3. Restart-aware reconnect flow

- specify the reconnect handshake for a client returning after gateway or server disruption
- define how the stack decides between:
  - resume existing logical session
  - reject stale resume
  - fall back to fresh login

### 4. Minimal shard/world locator boundary

- define only the control-plane metadata needed to route a resumable session toward the correct shard/world boundary
- keep this limited to selectors and continuity ownership; do not start full shard migration or world balancing yet

### 5. Verification harness for continuity

- add a branch-local proof target that demonstrates reconnect continuity after bounded restart events
- retain evidence bundles comparable to the readiness baseline artifacts

## Explicit Non-Goals

- no FPS gameplay replication, lag compensation, or gameplay-frequency UDP/RUDP expansion
- no full zone migration or live world transfer choreography
- no broad persistence rewrite beyond continuity-critical session state
- no reopening already-closed common baseline blockers as primary work
- no attempt to guarantee seamless continuity for arbitrary gameplay state outside the defined continuity contract

## Verification Bar

This branch's first tranche is only done when all items below are satisfied.

### 1. Continuity contract is explicit

- the resume/reconnect contract is written down
- ownership of continuity state is unambiguous across gateway/server/storage surfaces

### 2. Restart continuity proof exists

- gateway restart rehearsal:
  - a resumable client reconnect path succeeds
  - logical identity continuity is preserved
- server restart rehearsal:
  - a resumable client reconnect path succeeds
  - the continuity contract decides resume vs fresh login deterministically

### 3. Persistence evidence exists

- continuity-critical state survives the intended bounded restart window
- failure and recovery signals remain observable in metrics/logs

### 4. Evidence artifacts are retained

- loadgen or targeted reconnect reports
- pre/during/post metrics snapshots
- relevant service log tails
- short result summaries under the retained run roots

## First Implementation Slice

Start with the narrowest slice that can prove value:

1. define logical session continuity state
2. add resume token / lease validation path
3. wire reconnect decisioning through gateway/server
4. prove continuity through gateway and server restart rehearsals

Do not start shard/world expansion before this slice is green.
