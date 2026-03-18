# 코어(Core) API 변경 이력

주요 `Stable` API 변경은 이 파일에 기록합니다.

## 형식
- 각 릴리스 항목은 `변경됨(Changed)`, `파괴적 변경(Breaking)`, `사용 중단(Deprecated)` 구역을 사용합니다.
- 파괴적 변경 항목에는 `docs/core-api/` 하위 마이그레이션 노트 경로를 포함해야 합니다.

## 미출시(Unreleased)

### 변경됨(Changed)
- `server/core/fps/direct_bind.hpp`를 추가해 direct UDP bind request/response payload와 bind ticket data contract를 stable public engine surface로 승격했습니다.
- `server/core/fps/transport_policy.hpp`를 추가해 direct UDP/RUDP rollout enablement, canary selection, opcode allowlist parsing policy를 stable public engine surface로 승격했습니다.
- `server/core/fps/direct_delivery.hpp`를 추가해 direct UDP/RUDP delta delivery route selection policy를 stable public engine surface로 승격했습니다.
- `server/core/fps/transport_quality.hpp`를 추가해 direct UDP sequenced ingress의 loss/jitter/reorder/duplicate quality signal contract를 stable public engine surface로 승격했습니다.
- `server/core/fps/runtime.hpp`를 추가해 fixed-step authoritative tick, generic snapshot/delta shaping, coarse interest management, rewind/history query substrate를 stable public engine capability로 승격했습니다.
- `server/core/mmorpg/migration.hpp`를 추가해 draining source world에서 target world owner로의 migration envelope/status evaluation contract를 stable public engine surface로 승격했습니다.
- `server/core/mmorpg/world_drain.hpp`를 추가하고 `evaluate_world_drain_orchestration()`까지 확장해 live world drain phase/progress/orchestration evaluation contract를 stable public engine surface로 승격했습니다.
- `server/core/mmorpg/topology.hpp`를 추가하고 read-only topology actuation planning, revisioned topology actuation request document/status, executor-facing execution progress/status evaluation, observed-topology realization/adoption evaluation, adapter-facing lease/status evaluation, runtime-assignment document/instance lookup helper까지 확장해 revisioned desired topology document, observed topology pool aggregation, desired-vs-observed reconciliation status/actuation contract를 stable public engine surface로 승격했습니다.
- `server/core/mmorpg/world_transfer.hpp`를 추가해 live world owner handoff의 phase/status evaluation contract를 stable public engine surface로 승격했습니다.
- `server/core/app/engine_context.hpp`, `server/core/app/engine_runtime.hpp`, `server/core/app/engine_builder.hpp`를 추가해 app bootstrap의 lifecycle/dependency/composition seam을 stable public surface로 승격했습니다.
- `server/core/app/engine_runtime.hpp`에 `set_alias()`, `bridge_alias()`, `mark_running()`, `mark_stopped()`, `mark_failed()`, `clear_global_services()` helper를 추가해 app bootstrap이 alias bridge와 lifecycle teardown을 덜 open-code 하도록 정리했습니다.
- `server/core/app/engine_runtime.hpp`에 `wait_for_stop()`과 `run_shutdown()` helper를 추가하고, `AppHost::start_admin_http()` 경로가 admin-http stop step을 shutdown registry에 자동 연결하도록 정리해 non-Asio/control-plane bootstrap의 normal teardown을 덜 open-code 하도록 만들었습니다.
- `AppHost::start_admin_http()`와 `EngineRuntime::start_admin_http()`가 custom route callback을 받아 `admin_app` 같은 control-plane도 동일한 runtime composition 계층 위에서 admin HTTP를 조립할 수 있게 했습니다.
- `server/core/protocol/system_opcodes.hpp`의 `MSG_PING` 정책이 `transport=both`, `delivery=reliable`로 확장되어 direct UDP/RUDP gameplay-rate proof frame을 허용합니다.
- 헤더별 소비자 사용 커버리지를 높이기 위해 `CorePublicApiStableHeaderScenarios` 테스트 타깃을 추가했습니다.
- `tools/check_core_api_contracts.py`에 stable-governance fixture 회귀 검증을 추가했습니다.
- `server/core/protocol/packet.hpp`에 연결/세그먼트 분류 enum(`ConnectionType`, `SegmentType`)과 `classify_segment_type()` 헬퍼를 추가했습니다.
- `server/core/net/queue_budget.hpp`를 추가해 게이트웨이/세션 송신 큐 예산 초과 판단 로직을 공용화했습니다.
- `server/core/net/connection.hpp`의 송신 큐 수명주기 계약을 명시하고, close 중 큐 정리와 in-flight write가 충돌하지 않도록 버퍼 소유권 규칙을 강화했습니다.

### 파괴적 변경(Breaking)
- `server/core/build_info.hpp`와 `server/core/metrics/build_info.hpp`가 legacy 브랜드 토큰을 직접 노출하지 않도록 정리되면서 기본 build info 메트릭 이름이 `knights_build_info`에서 `runtime_build_info`로 변경되었습니다.
- Migration: `docs/core-api/runtime-build-info-rename.md`

### 사용 중단(Deprecated)
- 없음
