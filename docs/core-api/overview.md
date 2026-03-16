# 공개 API 개요(server_core)

## 상태 범례
- `[Stable]`: 호환성 보장을 제공하는 공개 계약
- `[Transitional]`: 현재는 공개되어 있으나 안정화 진행 중인 계약
- `[Internal]`: 공개/샘플 사용 대상이 아닌 내부 전용 계약

## 모듈 지도

| 모듈 | 안정성 | 주요 헤더 | 목적 |
|---|---|---|---|
| Runtime Host | `[Stable]` | `server/core/app/app_host.hpp`, `server/core/app/termination_signals.hpp` | 프로세스 수명주기, readiness/health, 프로세스 종료 신호 계약 |
| Networking | `[Stable]+[Internal]` | `server/core/net/hive.hpp`, `server/core/net/dispatcher.hpp`, `server/core/net/listener.hpp`, `server/core/net/connection.hpp` | 이벤트 루프 수명주기와 전송 라우팅 기본 구성요소 |
| Concurrency | `[Stable]+[Internal]` | `server/core/concurrent/task_scheduler.hpp`, `server/core/concurrent/job_queue.hpp`, `server/core/concurrent/thread_manager.hpp` | 스케줄러와 워커 큐 기본 구성요소 |
| Compression | `[Stable]` | `server/core/compression/compressor.hpp` | LZ4 기반 바이트 payload 압축/해제 계약 |
| Extensibility | `[Transitional]` | `server/core/plugin/shared_library.hpp`, `server/core/plugin/plugin_host.hpp`, `server/core/plugin/plugin_chain_host.hpp`, `server/core/scripting/script_watcher.hpp`, `server/core/scripting/lua_runtime.hpp`, `server/core/scripting/lua_sandbox.hpp` | 서비스 재사용을 위한 plugin/Lua 메커니즘 계층. 현재는 platform capability이지만 안정화 진행 중 |
| Memory | `[Stable]` | `server/core/memory/memory_pool.hpp` | 고정 크기 메모리 풀과 RAII 버퍼 매니저 계약 |
| Discovery / Shared state | `[Internal]` | (Stable 공개 헤더 없음) | shared instance-discovery contract와 Redis registry factory seam의 canonical ownership은 core에 있으나, surface는 아직 `Internal`이며 Consul adapter와 sticky session implementation은 app-owned로 유지 |
| Storage SPI | `[Internal]` | (Stable 공개 헤더 없음) | generic transaction 경계, shared Redis client contract, 비동기 DB 실행 seam. 채팅 repository 계층과 concrete Redis factory는 `server/storage/*`에 남음 |
| Metrics/Lifecycle | `[Stable]` | `server/core/metrics/metrics.hpp`, `server/core/metrics/http_server.hpp`, `server/core/metrics/build_info.hpp`, `server/core/runtime_metrics.hpp` | 운영 메트릭과 수명주기 가시성 |
| Protocol | `[Stable]` | `server/core/protocol/packet.hpp`, `server/core/protocol/protocol_flags.hpp`, `server/core/protocol/protocol_errors.hpp`, `server/core/protocol/system_opcodes.hpp` | 와이어 헤더, 플래그, 오류 코드, opcode 상수 |
| Security | `[Stable]` | `server/core/security/cipher.hpp` | 인증된 payload 의미를 포함한 AES-256-GCM 암복호화 계약 |
| Utilities | `[Stable]+[Internal]` | `server/core/util/log.hpp`, `server/core/util/paths.hpp`, `server/core/util/service_registry.hpp` | 바이너리 공통 유틸리티와 프로세스 보조 기능 |

## 표준 include 계약
- 공개 소비자는 `[Stable]`로 분류된 헤더만 사용합니다.
- include 형식은 `#include "server/core/..."` 또는 `<server/core/...>`를 사용합니다.
- 구현 경로(`core/src/**`)나 `Internal` 헤더를 포함하지 않습니다.
- 내부 헤더 목록은 `docs/core-api-boundary.md`에서만 관리합니다.

## 현재 사용 지침

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
  - `docs/core-api/net.md`
  - `docs/core-api/metrics-and-lifecycle.md`
  - `docs/core-api/storage.md`
  - `docs/core-api/extensions.md`
