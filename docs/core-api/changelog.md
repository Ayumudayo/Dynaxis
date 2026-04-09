# 코어(Core) API 변경 이력

주요 `Stable` API 변경은 이 파일에 기록합니다.

## 형식
- 각 릴리스 항목은 `변경됨(Changed)`, `파괴적 변경(Breaking)`, `사용 중단(Deprecated)` 구역을 사용합니다.
- 파괴적 변경 항목에는 `docs/core-api/` 하위 마이그레이션 노트 경로를 포함해야 합니다.

## 미출시(Unreleased)

### 변경됨(Changed)
- `server/core/realtime/simulation_phase.hpp`를 stable public surface로 추가해 fixed-step authoritative tick 내부의 결정론적 phase vocabulary(`SimulationPhase`, `SimulationPhaseContext`, `ISimulationPhaseObserver`)를 gameplay rule 없이 관측할 수 있게 했습니다.
- `server/core/net/transport_router.hpp`를 stable public surface로 추가해 `Connection`/`Listener` 위에서 동작하는 domain-neutral transport-session routing seam(`ITransportSession`, `TransportRouter`)을 제공하고, repo-owned public consumer proof를 packet `Session` 직접 의존 없이 이 seam으로 옮겼습니다.
- `server/core/runtime_metrics.hpp`에 process-wide liveness state, named watchdog snapshot, freeze suspicion, threshold-triggered detailed telemetry window surface를 추가하고, core/runtime exporters가 이 aggregate를 Prometheus text로 노출하도록 확장했습니다.
- `server/core/concurrent/task_scheduler.hpp`에 cancel token/group, `schedule_controlled()`, `schedule_every_controlled()`, `reschedule()`, `update_repeat_policy()`, repeat context/validator/policy surface를 추가해 caller-owned `poll()` 모델은 유지하면서도 runtime-owned periodic work를 덜 open-code 하도록 확장했습니다.
- `server/core/app/engine_runtime.hpp`와 `server/core/app/engine_builder.hpp`에 runtime-owned orchestration module seam(`register_module()`, `start_modules()`, `module_snapshot()`, expanded `Snapshot` watchdog/module counts)을 추가해 bootstrap이 ordered startup/shutdown과 watchdog attachment를 덜 open-code 하도록 정리했습니다.
- `server/core/worlds/{migration,topology,world_drain,world_transfer}.hpp`를 canonical stable public surface로 승격하고, repo-owned consumer/docs/tests를 `server::core::worlds` 기준으로 정렬했습니다.
- `server/core/realtime/{direct_bind,direct_delivery,transport_quality,transport_policy,runtime}.hpp`를 canonical stable public surface로 고정하고, repo-owned consumer/docs/tests를 `server::core::realtime` 기준으로 정렬했습니다.
- `server/core/discovery/{instance_registry,world_lifecycle_policy}.hpp`를 canonical stable public surface로 추가하고, repo-owned gateway/server/tools/tests/docs를 `server::core::discovery` 기준으로 정렬했습니다.
- `server/core/storage_execution/{unit_of_work,connection_pool,db_worker_pool,retry_backoff}.hpp`를 canonical stable public surface로 추가하고, repo-owned server/bootstrap, worker, package-consumer, public-api proof를 `server::core::storage_execution` 기준으로 정렬했습니다.
- `server/core/plugin/{shared_library,plugin_host,plugin_chain_host}.hpp`와 `server/core/scripting/{script_watcher,lua_runtime,lua_sandbox}.hpp`를 stable core extensibility mechanism으로 승격하고, dedicated `CorePublicApiExtensibilitySmoke`와 installed-consumer runtime proof를 추가했습니다.
- `server/core/worlds/aws.hpp`를 stable public surface로 추가해 Kubernetes-first pool binding을 AWS-first load balancer attachment, managed Redis/Postgres naming, world identity, region/zone placement metadata로 해석하는 provider adapter contract를 승격했습니다.
- `server/core/worlds/kubernetes.hpp`를 stable public surface로 추가해 desired topology + adapter lease + runtime assignment + drain orchestration을 Kubernetes-first workload lifecycle vocabulary로 해석하는 contract를 승격했습니다.
- `server/core/app/engine_runtime.hpp`에 `Snapshot` / `snapshot()`을 추가하고, `server/core/app/engine_context.hpp`에 `service_count()`를 추가해 canonical consumer가 lifecycle/service ownership을 instance 단위로 관측할 수 있게 했습니다.
- `server/core/util/service_registry.hpp`의 compatibility bridge가 runtime-owner 단위 clear를 지원하도록 확장되어, 한 runtime의 `clear_global_services()`가 다른 runtime의 shim entries를 지우지 않게 했습니다.
- `server/core/realtime/direct_bind.hpp`를 추가해 direct UDP bind request/response payload와 bind ticket data contract를 stable public engine surface로 승격했습니다.
- `server/core/realtime/transport_policy.hpp`를 추가해 direct UDP/RUDP rollout enablement, canary selection, opcode allowlist parsing policy를 stable public engine surface로 승격했습니다.
- `server/core/realtime/direct_delivery.hpp`를 추가해 direct UDP/RUDP delta delivery route selection policy를 stable public engine surface로 승격했습니다.
- `server/core/realtime/transport_quality.hpp`를 추가해 direct UDP sequenced ingress의 loss/jitter/reorder/duplicate quality signal contract를 stable public engine surface로 승격했습니다.
- `server/core/realtime/runtime.hpp`를 추가해 fixed-step authoritative tick, generic snapshot/delta shaping, coarse interest management, rewind/history query substrate를 stable public engine capability로 승격했습니다.
- `server/core/worlds/migration.hpp`를 추가해 draining source world에서 target world owner로의 migration envelope/status evaluation contract를 stable public engine surface로 승격했습니다.
- `server/core/worlds/world_drain.hpp`를 추가하고 `evaluate_world_drain_orchestration()`까지 확장해 live world drain phase/progress/orchestration evaluation contract를 stable public engine surface로 승격했습니다.
- `server/core/worlds/topology.hpp`를 추가하고 read-only topology actuation planning, revisioned topology actuation request document/status, executor-facing execution progress/status evaluation, observed-topology realization/adoption evaluation, adapter-facing lease/status evaluation, runtime-assignment document/instance lookup helper까지 확장해 revisioned desired topology document, observed topology pool aggregation, desired-vs-observed reconciliation status/actuation contract를 stable public engine surface로 승격했습니다.
- `server/core/worlds/world_transfer.hpp`를 추가해 live world owner handoff의 phase/status evaluation contract를 stable public engine surface로 승격했습니다.
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
- `server/core/fps/{direct_bind,direct_delivery,transport_quality,transport_policy,runtime}.hpp` compatibility wrapper path를 제거했습니다. canonical public surface는 `server/core/realtime/**`만 지원합니다.
- Migration: `docs/core-api/fps-to-realtime-migration.md`
- `server/core/mmorpg/{migration,topology,world_drain,world_transfer}.hpp` compatibility wrapper path를 제거했습니다. canonical public surface는 `server/core/worlds/**`만 지원합니다.
- Migration: `docs/core-api/mmorpg-to-worlds-migration.md`
- `server/core/build_info.hpp`와 `server/core/metrics/build_info.hpp`가 legacy 브랜드 토큰을 직접 노출하지 않도록 정리되면서 기본 build info 메트릭 이름이 `knights_build_info`에서 `runtime_build_info`로 변경되었습니다.
- Migration: `docs/core-api/runtime-build-info-rename.md`
