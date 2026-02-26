# 동시성(Concurrency) API 가이드

## 안정성

| 헤더 | 안정성 |
|---|---|
| `server/core/concurrent/task_scheduler.hpp` | `[Stable]` |
| `server/core/concurrent/job_queue.hpp` | `[Stable]` |
| `server/core/concurrent/thread_manager.hpp` | `[Stable]` |

## 핵심 계약
- `TaskScheduler`는 pull 기반입니다. 호출자가 poll 루프와 실행 주기를 직접 소유합니다.
- `JobQueue`는 스레드 안전 FIFO 의미를 제공하며, `Stop()` 기반의 명시적 종료 신호를 사용합니다.
- `ThreadManager`는 `JobQueue`를 소비하며 보호된 `Start()`와 멱등 `Stop()` 동작을 제공합니다.
- 내부 큐 기본 구성요소는 런타임 배선용으로만 사용하며 공개 예제에는 포함하지 않습니다.

## 사용 규칙
- 예약 작업은 짧게 유지하고 부작용 범위를 명확히 제한합니다.
- 워커 리소스는 명시적 종료 순서를 우선합니다.
- 비즈니스 로직이 큐 내부 구현에 결합되지 않도록 합니다.
