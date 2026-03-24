# 공개 API 경계(server_core, 1단계)

## 목적
- 어떤 헤더를 실제 API 계약으로 취급할지 정의합니다.
- `Stable` 공개 API와 `Transitional`, `Internal` 헤더를 분리합니다.
- 불안정한 내부 구현에 대한 우발적 의존을 방지합니다.

## 호환성 레벨
- `Stable`: 호환성 보장이 적용됩니다. 파괴적 변경에는 마이그레이션 노트가 필요합니다.
- `Transitional`: 현재 노출되어 있으나 API 안정화가 진행되는 동안 변경될 수 있습니다.
- `Internal`: 호환성 보장이 없으며 공개 예제에서 사용하면 안 됩니다.

## 표준 include 계약
- 공개 소비자는 이 문서에서 `Stable`로 지정한 헤더만 포함해야 합니다.
- 공개 include 형식은 `#include "server/core/..."`(또는 `<server/core/...>`)이며, 구현 경로 include는 금지합니다.
- `core/include/server/core/protocol/system_opcodes.hpp`는 생성 파일이므로 헤더를 직접 수정하지 않고 JSON과 생성기를 수정합니다.
- `Internal`로 표시된 헤더는 샘플/공개 문서에서 사용 금지입니다.

## 헤더 인벤토리 및 분류

| 헤더 | 레벨 | 비고 |
|---|---|---|
| `server/core/api/version.hpp` | Stable | 공개 API 버전 신호입니다. stable 헤더 변경 시 이 버전을 갱신해야 합니다. |
| `server/core/app/app_host.hpp` | Stable | server/gateway/tools 전반에서 사용하는 런타임 호스트 계약 |
| `server/core/app/engine_builder.hpp` | Stable | bootstrap 초기 lifecycle/dependency/admin-http 기본값을 선언적으로 구성하는 공용 조립 빌더 |
| `server/core/app/engine_context.hpp` | Stable | 인스턴스 범위의 타입 기반 composition context. 앱 bootstrap이 core primitive를 지역적으로 조립하고 `service_count()`로 local ownership을 관측하는 기준 표면 |
| `server/core/app/engine_runtime.hpp` | Stable | `AppHost`와 `EngineContext`를 묶어 수명주기, 의존성, 종료, admin-http 제어를 공통화하고, `snapshot()`과 런타임 소유 compatibility bridge 정리까지 제공하는 런타임 조립 표면 |
| `server/core/app/termination_signals.hpp` | Stable | non-Asio 루프 및 공용 종료 시그널링을 위한 프로세스 전역 종료 폴링 계약 |
| `server/core/build_info.hpp` | Stable | 모든 바이너리가 사용하는 빌드 메타데이터 계약 |
| `server/core/realtime/direct_bind.hpp` | Stable | direct UDP bind 요청과 응답 payload, bind ticket 데이터 계약을 정의하는 기준 realtime ingress 표면 |
| `server/core/realtime/direct_delivery.hpp` | Stable | direct UDP/RUDP delta 전달 경로 선택 정책을 app-local opcode 판별과 분리해 노출하는 기준 realtime 전송 정책 표면 |
| `server/core/realtime/transport_quality.hpp` | Stable | direct UDP sequenced ingress의 loss, jitter, reorder, duplicate 품질 신호 계약을 노출하는 기준 realtime 전송 품질 표면 |
| `server/core/realtime/transport_policy.hpp` | Stable | direct UDP/RUDP rollout 활성화, canary 선택, opcode allowlist 해석을 정의하는 기준 realtime 전송 정책 표면 |
| `server/core/realtime/runtime.hpp` | Stable | 고정 주기 authoritative tick, 공용 snapshot/delta shaping, 거친 관심 범위, rewind/history 조회를 제공하는 기준 realtime 엔진 능력 표면 |
| `server/core/discovery/instance_registry.hpp` | Stable | shared instance record, selector, backend interface와 in-memory discovery backend를 노출하는 기준 discovery 표면 |
| `server/core/discovery/world_lifecycle_policy.hpp` | Stable | world drain과 replacement owner 정책의 serialization, parse 계약을 노출하는 기준 discovery shared-state 표면 |
| `server/core/storage_execution/unit_of_work.hpp` | Stable | 도메인 repository accessor를 포함하지 않는 기준 storage execution 트랜잭션 경계 |
| `server/core/storage_execution/connection_pool.hpp` | Stable | 공용 unit-of-work factory, pool tuning 옵션, health-check adapter 경계를 노출하는 기준 storage execution 표면 |
| `server/core/storage_execution/db_worker_pool.hpp` | Stable | 공용 transaction seam 위에서 비동기 storage task를 실행하는 기준 worker-pool 표면 |
| `server/core/storage_execution/retry_backoff.hpp` | Stable | linear, exponential-full-jitter retry backoff 계산을 공용화하는 기준 storage execution 재시도 helper 표면 |
| `server/core/worlds/migration.hpp` | Stable | draining source world에서 target world owner로 이동하는 migration envelope와 status 평가를 정의하는 world migration 런타임 표면 |
| `server/core/worlds/aws.hpp` | Stable | Kubernetes-first pool binding을 AWS 전용 world identity, load balancer attachment, managed Redis/Postgres naming, region/zone placement metadata로 해석하는 provider adapter 계약 |
| `server/core/worlds/kubernetes.hpp` | Stable | desired topology, adapter lease, runtime assignment, drain orchestration을 Kubernetes workload lifecycle 어휘로 해석하는 Kubernetes 우선 world orchestration 계약 |
| `server/core/worlds/world_drain.hpp` | Stable | 실시간 world drain phase와 progress 평가를 정의하는 world-drain 런타임 표면 |
| `server/core/worlds/topology.hpp` | Stable | desired topology 문서, observed topology pool 집계, desired-vs-observed reconciliation 상태, 읽기 전용 topology actuation 계획, revision이 있는 actuation request/status 평가, executor용 execution progress/status 평가, observed-topology realization/adoption 평가, adapter용 lease/status 평가, runtime-assignment 문서와 instance 조회 helper를 정의하는 world topology 제어면 표면 |
| `server/core/worlds/world_transfer.hpp` | Stable | 실시간 world owner handoff의 phase와 status 평가 계약을 정의하는 world owner-transfer 런타임 표면 |
| `server/core/compression/compressor.hpp` | Stable | 비정상/손상 입력에 대한 명시적 오류 신호를 포함한 LZ4 압축/해제 계약 |
| `server/core/concurrent/job_queue.hpp` | Stable | 명시적 stop 및 backpressure 동작을 포함한 bounded/unbounded FIFO 큐 계약 |
| `server/core/concurrent/locked_queue.hpp` | Internal | 내부 worker 배선에 사용하는 저수준 큐 기본 구성요소 |
| `server/core/concurrent/task_scheduler.hpp` | Stable | 명확한 스케줄링 계약과 모듈 간 사용 지점 제공 |
| `server/core/concurrent/thread_manager.hpp` | Stable | `JobQueue` 소비용 고정 worker-pool 계약(guarded start, 멱등 stop) |
| `server/core/config/options.hpp` | Stable | 네트워킹 경로에서 소비하는 세션 런타임 옵션 |
| `server/core/memory/memory_pool.hpp` | Stable | bounded failure(`Acquire()==nullptr`) 의미를 갖는 고정 블록 할당기 + RAII 버퍼 계약 |
| `server/core/metrics/build_info.hpp` | Stable | 공용 Prometheus build-info helper |
| `server/core/metrics/http_server.hpp` | Stable | 공용 admin/metrics HTTP 표면 |
| `server/core/metrics/metrics.hpp` | Stable | 백엔드가 없을 때 no-op fallback을 제공하는 명명 메트릭 접근 계약 |
| `server/core/net/acceptor.hpp` | Internal | `SessionOptions`/`ConnectionRuntimeState`와 결합된 서버 전용 accept 루프이며 stable transport 계약 대상이 아님 |
| `server/core/net/connection.hpp` | Stable | FIFO send-queue 순서, bounded queue backpressure, 멱등 stop 수명주기를 갖는 확장형 transport 기반 |
| `server/core/net/dispatcher.hpp` | Stable | legacy packet dispatcher 계약은 유지되지만, 새 public consumer의 기본 선택은 `transport_router.hpp` higher-level seam이어야 함 |
| `server/core/net/hive.hpp` | Stable | transport 모듈이 공유하는 `io_context` 수명주기 래퍼 |
| `server/core/net/listener.hpp` | Stable | connection factory 주입과 멱등 stop 의미를 갖는 범용 accept 루프 계약 |
| `server/core/net/transport_router.hpp` | Stable | `ITransportSession`만 요구하는 domain-neutral higher-level transport/session routing seam |
| `server/core/net/rudp/ack_window.hpp` | Internal | gateway canary RUDP 경로에서 사용하는 실험/튜닝 대상 ACK window 구성요소 |
| `server/core/net/rudp/retransmission_queue.hpp` | Internal | 재전송 타이머/ACK 마스크 세부 동작이 안정화 전인 internal 큐 계약 |
| `server/core/net/rudp/rudp_engine.hpp` | Internal | gateway 통합 전개 단계의 실험적 RUDP 엔진 API(기본 OFF) |
| `server/core/net/rudp/rudp_packet.hpp` | Internal | RUDP wire 헤더/프레이밍 실험 스펙 보조 타입 |
| `server/core/net/rudp/rudp_peer_state.hpp` | Internal | RUDP peer 상태 추적용 내부 상태 구조체 |
| `server/core/net/connection_runtime_state.hpp` | Internal | 연결 수 가드레일 및 랜덤 세션 ID seed를 위한 내부 세션 런타임 상태 계약 |
| `server/core/net/session.hpp` | Internal | dispatcher/options/shared runtime state에 결합된 서버 패킷 세션 구현 |
| `server/core/protocol/packet.hpp` | Stable | 와이어 헤더 인코드/디코드 계약 |
| `server/core/protocol/protocol_errors.hpp` | Stable | 프로토콜 응답 공용 오류 코드 상수 |
| `server/core/protocol/protocol_flags.hpp` | Stable | 프로토콜 플래그/기능 비트 공용 상수 |
| `server/core/protocol/system_opcodes.hpp` | Stable | server/client 경로에서 소비하는 생성 opcode 계약 |
| `server/core/plugin/shared_library.hpp` | Stable | 서비스 중립적 확장 메커니즘. 동적 로더 RAII wrapper로 안정된 plugin loading 기반을 제공하는 core 플랫폼 능력 |
| `server/core/plugin/plugin_host.hpp` | Stable | cache-copy, validator, lock/sentinel 의미를 제공하는 단일 플러그인 로드/리로드 host 계약 |
| `server/core/plugin/plugin_chain_host.hpp` | Stable | 다중 플러그인 체인의 스캔, 정렬, 리로드 orchestration 계약 |
| `server/core/runtime_metrics.hpp` | Stable | server/gateway/tools 관측 경로가 사용하는 프로세스 전역 런타임 카운터/스냅샷 계약 |
| `server/core/state/instance_registry.hpp` | Internal | `InstanceRecord`, selector, backend-interface 같은 shared discovery 계약과 가깝지만 discovery adapter와 sticky routing 안정화가 끝나지 않은 internal 경계 |
| `server/core/state/world_lifecycle_policy.hpp` | Internal | 기준 discovery wrapper 아래에 남겨 둔 실제 world lifecycle policy 구현 헤더 |
| `server/core/state/redis_instance_registry.hpp` | Internal | Redis 기반 discovery adapter와 runtime seam의 기준 헤더. ownership은 core로 옮겼지만 아직 Stable 승격 전인 transitional/internal 경계 |
| `server/core/state/redis_backend_factory.hpp` | Internal | Redis 기반 discovery backend factory seam의 기준 헤더. gateway, server, tools가 공유하지만 compatibility와 promotion 전략이 아직 고정되지 않은 internal 경계 |
| `server/core/scripting/script_watcher.hpp` | Stable | 파일 감시와 sentinel 정책을 제공하는 core extensibility watcher 계약 |
| `server/core/scripting/lua_runtime.hpp` | Stable | Lua script load, call, reload, host API 등록, metric 노출을 제공하는 안정된 core runtime 능력 |
| `server/core/scripting/lua_sandbox.hpp` | Stable | instruction, memory 제한과 허용 라이브러리 정책을 정의하는 안정된 core sandbox 계약 |
| `server/core/security/admin_command_auth.hpp` | Internal | admin control-plane 서명 검증/nonce replay 보호를 위한 내부 인증 helper |
| `server/core/security/cipher.hpp` | Stable | 키/IV 크기 검증과 인증 실패 신호를 포함한 AES-256-GCM 암복호화 계약 |
| `server/core/trace/context.hpp` | Internal | 로그/상관관계 추적 컨텍스트의 구현 결합 helper |
| `server/core/storage/connection_pool.hpp` | Internal | 실제 generic transaction/UoW factory와 health-check SPI 헤더. 안정된 소비 표면은 `server/core/storage_execution/connection_pool.hpp` |
| `server/core/storage/db_worker_pool.hpp` | Internal | 실제 async DB execution helper 구현 헤더. 안정된 소비 표면은 `server/core/storage_execution/db_worker_pool.hpp` |
| `server/core/storage/redis/client.hpp` | Internal | gateway, server, tools가 공유하는 Redis client 계약. concrete redis-plus-plus adapter와 factory는 여전히 server 소유 |
| `server/core/storage/unit_of_work.hpp` | Internal | 실제 generic commit/rollback transaction 경계. 안정된 소비 표면은 `server/core/storage_execution/unit_of_work.hpp` |
| `server/core/util/crash_handler.hpp` | Internal | 앱 엔트리포인트용 프로세스 레벨 크래시 훅 |
| `server/core/util/log.hpp` | Stable | 모든 바이너리가 사용하는 공통 로깅 계약 |
| `server/core/util/paths.hpp` | Stable | 도구/서비스에서 사용하는 실행 파일 경로 helper |
| `server/core/util/service_registry.hpp` | Stable | 멀티 바이너리 런타임 조합에 사용하는 타입 기반 서비스 등록/조회 계약 |

## 공개 API 명명 규칙 (1단계)
- 공개 심볼은 `server::core::<module>` 하위에 두고 전역 별칭을 피합니다.
- 공개 API는 가변 public 데이터 필드를 지양하고, 동작을 정의하는 메서드 중심으로 설계합니다.
- 공개 헤더는 `server/` 또는 `gateway/` 구현 헤더에 의존하면 안 됩니다.
- 공개 문서/예제는 `Internal` 헤더를 포함하면 안 됩니다.
- 런타임 조립(runtime composition)은 `EngineBuilder -> EngineRuntime -> EngineContext` 순으로 사용하고, 전역 `ServiceRegistry`는 필요한 경우 compatibility bridge로만 사용합니다.

## 즉시 후속 작업
- `Transitional` 헤더는 릴리스마다 축소하고, 안정화가 끝난 표면은 `Stable`로 승격하거나 `Internal`로 재분류합니다.
- 공개 예제가 `Stable` 헤더만으로 컴파일되도록 CI 가드를 추가합니다.
- `docs/core-api/compatibility-matrix.json`를 `Stable` 헤더 인벤토리와 API 버전에 맞춰 동기화합니다.
