# 공개 API 개요(server_core)

## 상태 범례
- `[Stable]`: 호환성 보장을 제공하는 공개 계약
- `[Transitional]`: 현재는 공개되어 있으나 안정화 진행 중인 계약
- `[Internal]`: 공개/샘플 사용 대상이 아닌 내부 전용 계약

## Package-First Reading Path

- `docs/core-api/overview.md`
  - 현재 공개 surface와 안정성 분류를 먼저 확인합니다.
- `docs/core-api/quickstart.md`
  - 최소 consumer 코드, install prefix, `find_package(server_core)` 흐름을 확인합니다.
- `docs/core-api/compatibility-policy.md`
  - 어떤 변경이 version bump, migration note, proof rerun을 요구하는지 확인합니다.
- `docs/core-api/checklists.md`
  - PR/release gate와 정확한 검증 명령을 확인합니다.
- `docs/tests.md`
  - Windows package proof, Linux parity smoke, stack/runtime entrypoint를 repo-wide 기준으로 확인합니다.

## 모듈 지도

| 모듈 | 안정성 | 주요 헤더 | 목적 |
|---|---|---|---|
| Runtime Host | `[Stable]` | `server/core/app/app_host.hpp`, `server/core/app/engine_builder.hpp`, `server/core/app/engine_context.hpp`, `server/core/app/engine_runtime.hpp`, `server/core/app/termination_signals.hpp` | 프로세스 수명주기, readiness/health, bootstrap composition, 프로세스 종료 신호 계약 |
| Realtime Capability | `[Stable]` | `server/core/realtime/direct_bind.hpp`, `server/core/realtime/direct_delivery.hpp`, `server/core/realtime/transport_quality.hpp`, `server/core/realtime/transport_policy.hpp`, `server/core/realtime/runtime.hpp` | fixed-step runtime, bind payload contract, rollout/canary/allowlist policy, direct delivery route policy, sequenced UDP quality tracking, coarse interest management, rewind/history lookup |
| Discovery / Shared state | `[Stable]+[Internal]` | `server/core/discovery/instance_registry.hpp`, `server/core/discovery/world_lifecycle_policy.hpp` | shared instance record/selector/backend interface, in-memory discovery backend, world drain/replacement policy serialization; sticky session directory and concrete Redis/Consul adapters remain outside the stable surface |
| Storage Execution | `[Stable]+[Internal]` | `server/core/storage_execution/unit_of_work.hpp`, `server/core/storage_execution/connection_pool.hpp`, `server/core/storage_execution/db_worker_pool.hpp`, `server/core/storage_execution/retry_backoff.hpp` | generic transaction 경계, connection-pool health/adapter seam, async DB worker execution, retry/backoff helper를 노출하는 canonical storage execution surface; shared Redis client contract과 chat repository 계층은 internal/app-owned로 남음 |
| World Orchestration | `[Stable]` | `server/core/worlds/migration.hpp`, `server/core/worlds/aws.hpp`, `server/core/worlds/kubernetes.hpp`, `server/core/worlds/topology.hpp`, `server/core/worlds/world_drain.hpp`, `server/core/worlds/world_transfer.hpp` | desired topology document, observed pool aggregation, desired-vs-observed reconciliation summary, read-only topology actuation plan, revisioned topology actuation request/status evaluation, executor-facing execution progress/status evaluation, observed-topology realization/adoption evaluation, adapter-facing lease/status evaluation, runtime-assignment document/instance lookup helper, Kubernetes-first workload binding/orchestration evaluation, AWS-first provider binding for world identity/LB/managed dependency conventions, live world-drain progress/orchestration evaluation, live owner-transfer phase/status evaluation, draining-source to target-world migration envelope/status |
| Networking | `[Stable]+[Internal]` | `server/core/net/hive.hpp`, `server/core/net/dispatcher.hpp`, `server/core/net/listener.hpp`, `server/core/net/connection.hpp` | 이벤트 루프 수명주기와 전송 라우팅 기본 구성요소 |
| Concurrency | `[Stable]+[Internal]` | `server/core/concurrent/task_scheduler.hpp`, `server/core/concurrent/job_queue.hpp`, `server/core/concurrent/thread_manager.hpp` | 스케줄러와 워커 큐 기본 구성요소 |
| Compression | `[Stable]` | `server/core/compression/compressor.hpp` | LZ4 기반 바이트 payload 압축/해제 계약 |
| Extensibility | `[Stable]+[Transitional]` | `server/core/plugin/shared_library.hpp`, `server/core/plugin/plugin_host.hpp`, `server/core/plugin/plugin_chain_host.hpp`, `server/core/scripting/script_watcher.hpp`, `server/core/scripting/lua_runtime.hpp`, `server/core/scripting/lua_sandbox.hpp` | service-neutral plugin/Lua mechanism은 stable core platform capability로 승격되었고, chat hook ABI와 chat Lua bindings는 계속 transitional/app-owned로 남음 |
| Memory | `[Stable]` | `server/core/memory/memory_pool.hpp` | 고정 크기 메모리 풀과 RAII 버퍼 매니저 계약 |
| Metrics/Lifecycle | `[Stable]` | `server/core/metrics/metrics.hpp`, `server/core/metrics/http_server.hpp`, `server/core/metrics/build_info.hpp`, `server/core/runtime_metrics.hpp` | 운영 메트릭과 수명주기 가시성 |
| Protocol | `[Stable]` | `server/core/protocol/packet.hpp`, `server/core/protocol/protocol_flags.hpp`, `server/core/protocol/protocol_errors.hpp`, `server/core/protocol/system_opcodes.hpp` | 와이어 헤더, 플래그, 오류 코드, opcode 상수 |
| Security | `[Stable]` | `server/core/security/cipher.hpp` | 인증된 payload 의미를 포함한 AES-256-GCM 암복호화 계약 |
| Utilities | `[Stable]+[Internal]` | `server/core/util/log.hpp`, `server/core/util/paths.hpp`, `server/core/util/service_registry.hpp` | 바이너리 공통 유틸리티와 프로세스 보조 기능 |

## 표준 include 계약
- 공개 소비자는 `[Stable]`로 분류된 헤더만 사용합니다.
- include 형식은 `#include "server/core/..."` 또는 `<server/core/...>`를 사용합니다.
- 구현 경로(`core/src/**`)나 `Internal` 헤더를 포함하지 않습니다.
- realtime public surface의 canonical path/namespace는 `server/core/realtime/**`와 `server::core::realtime`입니다.
- 기존 `server/core/fps/**` include 경로는 2.x compatibility wrapper로만 유지되며 새 consumer의 기준 표면이 아닙니다.
- discovery public surface의 canonical path/namespace는 `server/core/discovery/**`와 `server::core::discovery`입니다.
- `server/core/state/**`는 underlying/internal ownership 경로로 남으며 새 consumer의 기준 표면이 아닙니다.
- storage execution public surface의 canonical path/namespace는 `server/core/storage_execution/**`와 `server::core::storage_execution`입니다.
- `server/core/storage/*`와 shared Redis client contract은 underlying/internal ownership 경로로 남으며 새 consumer의 기준 표면이 아닙니다.
- world orchestration public surface의 canonical path/namespace는 `server/core/worlds/**`와 `server::core::worlds`입니다.
- 기존 `server/core/mmorpg/**` include 경로는 compatibility wrapper로만 유지되며 새 consumer의 기준 표면이 아닙니다.
- 내부 헤더 목록은 `docs/core-api-boundary.md`에서만 관리합니다.

## Package-First Proof Baseline

- Windows public package proof:
- `core_public_api_smoke`
- `core_public_api_headers_compile`
- `core_public_api_stable_header_scenarios`
- `core_public_api_extensibility_smoke`
- `core_public_api_realtime_capability_smoke`
- `CoreInstalledPackageConsumer` (`server_core_installed_consumer` + `server_core_extensibility_consumer` run)
- `CoreApiBoundaryFixtures`
- `CoreApiStableGovernanceFixtures`
  - `CoreFpsCompatSmoke` (2.x compatibility wrapper proof)
- Linux parity proof:
  - `pwsh scripts/run_linux_installed_consumer.ps1`
- boundary truth는 `docs/core-api-boundary.md`, version/matrix truth는 `core/include/server/core/api/version.hpp`와 `docs/core-api/compatibility-matrix.json`입니다.
- Phase 4 문서 세트는 docs, package validation, CI wording이 이 proof baseline을 같은 방식으로 설명하는 것을 목표로 합니다.

## 현재 사용 지침

- Runtime Host surface는 app bootstrap 공통화를 위한 기본 composition seam입니다.
  - `EngineBuilder`는 초기 lifecycle/dependency/admin-http defaults를 선언합니다.
  - `EngineRuntime`는 `AppHost`와 `EngineContext`를 묶어 app-local bootstrap assembly를 표준화하고, `set_service()` / `set_alias()` / `bridge_service()` / `bridge_alias()`로 instance-local context와 legacy global registry bridge를 같은 규약으로 다룰 수 있습니다.
  - `EngineRuntime::snapshot()`은 lifecycle/readiness/stop/context-service-count/compatibility-bridge-count를 인스턴스 단위로 읽는 canonical embeddability view입니다.
  - `mark_running()` / `mark_stopped()` / `mark_failed()`, `wait_for_stop()`, `run_shutdown()`, `clear_global_services()`는 bootstrap 종료 경로를 덜 open-coded 하도록 돕는 composition helper입니다.
  - `EngineContext`는 instance-scoped typed registry이며 `service_count()`로 local service ownership을 직접 관측할 수 있습니다. 전역 `ServiceRegistry`는 기존 소비자 브리지로만 사용합니다.
  - `clear_global_services()`는 현재 runtime이 올린 compatibility bridge만 정리하며, 다른 runtime의 bridge entries는 지우지 않습니다.
  - app bootstrap에 남는 코드는 listener/route/worker/metric-payload 같은 app-local behavior여야 하며, process lifecycle/admin-http/dependency/shutdown 같은 공통 concern은 이 seam이 소유합니다.
- Concurrency surface는 현재 `TaskScheduler`, `JobQueue`, `ThreadManager`의 기본 계약만 유지한다.
  - `TaskScheduler`는 호출자가 poll 루프와 실행 주기를 직접 소유한다.
  - `JobQueue`와 `ThreadManager`는 명시적 종료 순서를 전제로 한다.
  - 내부 큐 구성요소는 런타임 배선용이며 공개 예제 surface로 취급하지 않는다.
- Extensibility 관련 미래 후보는 별도 candidate 문서가 아니라 `docs/core-api/extensions.md`의 "future candidate surfaces" 아래에서만 추적한다.

## 관련 문서
- 경계 정의: `docs/core-api-boundary.md`
- 호환성 정책: `docs/core-api/compatibility-policy.md`
- 채택/컷오버: `docs/core-api/adoption-cutover.md`
- 빠른 시작: `docs/core-api/quickstart.md`
- 리뷰/릴리스 체크리스트: `docs/core-api/checklists.md`
- 세부 가이드:
  - `docs/core-api/realtime.md`
  - `docs/core-api/discovery.md`
  - `docs/core-api/worlds-migration.md`
  - `docs/core-api/worlds-aws.md`
  - `docs/core-api/worlds-kubernetes.md`
  - `docs/core-api/worlds-drain.md`
  - `docs/core-api/worlds-topology.md`
  - `docs/core-api/worlds-transfer.md`
  - `docs/core-api/net.md`
  - `docs/core-api/metrics-and-lifecycle.md`
  - `docs/core-api/promotion-matrix.md`
  - `docs/core-api/storage.md`
  - `docs/core-api/extensions.md`
