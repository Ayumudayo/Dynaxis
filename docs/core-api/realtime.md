# 실시간 기능(Realtime Capability) API 가이드

## 안정성

| 헤더 | 안정성 |
|---|---|
| `server/core/realtime/direct_bind.hpp` | `[Stable]` |
| `server/core/realtime/direct_delivery.hpp` | `[Stable]` |
| `server/core/realtime/transport_quality.hpp` | `[Stable]` |
| `server/core/realtime/transport_policy.hpp` | `[Stable]` |
| `server/core/realtime/runtime.hpp` | `[Stable]` |

## 기준 이름

- 기준 include 경로 계열: `server/core/realtime/**`
- 기준 네임스페이스: `server::core::realtime`
- rename 배경: `docs/core-api/fps-to-realtime-migration.md`

이 표면이 중요한 이유는 “실시간 기능”을 앱 로직에서 떼어 내 공용 contract로 고정하기 위해서다. 그렇지 않으면 현재 workload에 맞춘 전송 구현 세부가 그대로 public API가 되어, 다른 consumer가 쓰기 어렵고 향후 refactor도 힘들어진다.

## 범위

- engine-neutral fixed-step runtime substrate
- direct bind payload contract
- rollout/canary/allowlist policy
- direct delivery route policy
- sequenced UDP quality tracking
- snapshot/delta shaping
- rewind/history lookup

포함하지 않는 것:

- gateway 내부 UDP/RUDP 엔진 구현
- session 구현 세부
- 게임별 전투 규칙
- wire/protobuf encoding

즉 이 계층은 “실시간 구현체”가 아니라 “실시간 기능 계약”이다.

## 공개 계약

- `DirectBindRequest`, `DirectBindTicket`, `DirectBindResponse`
  - direct UDP attach에 필요한 payload 계약이다.
- `encode_direct_bind_request_payload()` / `decode_direct_bind_request_payload()`
- `encode_direct_bind_response_payload()` / `decode_direct_bind_response_payload()`
  - generic packet header 위에 올리는 stable payload encoding 규칙이다.
- `parse_direct_opcode_allowlist()`
  - decimal/hex CSV allowlist를 opcode 집합으로 바꾸는 helper다.
- `DirectTransportRolloutPolicy`
  - `enabled`
  - `canary_percent`
  - `opcode_allowlist`
  - deterministic `session_selected()`
  - explicit `opcode_allowed()`
- `evaluate_direct_attach()`
  - bind 시점에서 attach 경로를 어떻게 고를지 명시한다.
- `DirectDeliveryContext`, `DirectDeliveryDecision`, `evaluate_direct_delivery()`
  - 메시지가 direct path를 탈 수 있는지, `tcp fallback`, `udp`, `rudp` 중 무엇을 써야 하는지 판단한다.
- `UdpSequencedMetrics`
  - direct UDP ingress 품질 신호를 추적한다.
- `FixedStepDriver`
  - 고정 틱(fixed-step)에서 catch-up work 상한을 가진다.
- `WorldRuntime`
  - tick-aligned input staging
  - authoritative actor state advancement
  - coarse interest selection
  - snapshot/delta replication shaping
  - bounded rewind/history retention
- `RuntimeConfig`
  - `tick_rate_hz`
  - `snapshot_refresh_ticks`
  - `interest_cell_size_mm`
  - `interest_radius_cells`
  - `max_interest_recipients_per_tick`
  - `max_delta_actors_per_tick`
  - `history_ticks`

## 의미 규약

- `stage_input()`
  - 입력을 즉시 적용하지 않고 다음 authoritative tick에 반영하도록 staging한다.
  - stale input은 상태를 바꾸지 않고 거부한다.
- `tick()`
  - viewer별 `ReplicationUpdate`를 돌려준다.
  - `kSnapshot`은 전체 상태를 다시 맞추는 경로다.
  - `kDelta`는 dirty actor와 removed actor만 담는 경로다.
  - delta budget을 넘기면 snapshot fallback으로 전환한다.
- `rewind_at_or_before()`
  - lag compensation이나 history query에서 특정 tick 이하의 가장 가까운 authoritative sample을 찾는다.

이런 규약을 core가 고정하는 이유는, 앱이 직접 매번 구현하면 “입력 적용 시점”, “snapshot 강제 refresh”, “rewind 의미”가 consumer마다 달라지기 쉽기 때문이다. 그 상태로는 테스트도, 유지보수도, 설치형 소비자 검증도 어려워진다.

## 비목표

- UDP/RUDP handshake 구현
- transport rollout 엔진 구현
- wire/protobuf/game opcode encoding
- 무기/전투/제품 로직
- migration/session continuity 정책

## 공개 검증

- contract proof target: `CorePublicApiRealtimeCapabilitySmoke`
- installed consumer proof: `CoreInstalledPackageConsumer`
- preferred acceptance proof:
  - `CorePublicApiRealtimeCapabilitySmoke`
  - `tests/python/verify_fps_rudp_transport_matrix.py --scenario phase2-acceptance`
- lower-level stack proof:
  - `tests/python/verify_fps_rudp_transport_matrix.py`
  - direct RUDP attach/fallback/restart와 deterministic direct-UDP quality impairment까지 포함한다
