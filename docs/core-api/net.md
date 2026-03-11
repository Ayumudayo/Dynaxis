# 네트워킹(Networking) API 가이드

## 안정성

| 헤더 | 안정성 |
|---|---|
| `server/core/net/hive.hpp` | `[Stable]` |
| `server/core/net/dispatcher.hpp` | `[Stable]` |
| `server/core/net/listener.hpp` | `[Stable]` |
| `server/core/net/connection.hpp` | `[Stable]` |

## 핵심 계약
- `Hive`는 공유 `io_context`의 run/stop 수명주기를 소유합니다.
- `Dispatcher`는 `msg_id`를 핸들러로 매핑하며 비즈니스 로직을 소유하지 않습니다.
- `Listener`는 accept 루프 수명주기를 소유하고 `connection_factory`를 통해 전송 객체 생성을 주입합니다.
- `Connection`은 FIFO 쓰기 순서와 제한된 send-queue 백프레셔를 갖는 비동기 read/write 루프를 소유합니다.
- 서버 전용 패킷 세션 구현(`acceptor`/`session`)은 내부 범위이며 공개 API 사용 대상이 아닙니다.

## Resilience / Backpressure Story
- `Connection`의 생성자 `send_queue_max_bytes`는 transport 경계에서 사용할 수 있는 현재 stable backpressure knob입니다. 큐 예산을 초과한 `async_send()`는 `no_buffer_space` 오류 경로로 연결을 정리합니다.
- `Listener` 자체는 범용 accept 루프를 유지하고, 수락 이후의 backpressure는 생성된 `Connection` 구현 또는 상위 admission 정책이 담당합니다. 즉 현재 public listener 계약은 "backpressure-aware transport factory를 주입할 수 있다"까지입니다.
- `server/core/net/queue_budget.hpp`의 `exceeds_queue_budget()`는 `Session`과 gateway backend bridge가 함께 재사용하는 공용 helper이지만, 현재는 `Connection`/`Session`/gateway 구현을 뒷받침하는 shared helper로 보는 것이 맞습니다.
- `server/core/net/resilience_controls.hpp`의 `TokenBucket`, `RetryBudget`, `CircuitBreaker`는 gateway가 실제 운영 제어에 사용하는 재사용 가능한 policy 타입입니다. 다만 boundary inventory에 별도 stable header로 승격되기 전까지는 "shared candidate surface"로 취급합니다.

## Current Wired Controls
- Server packet session path: `Session::async_send()`는 `SessionOptions::send_queue_max`와 `queue_budget` helper를 사용해 oversize queued send를 drop하고 세션을 종료합니다.
- Generic transport path: `Connection::async_send()`는 per-connection `send_queue_max_bytes_`를 강제하고 단일 in-flight write만 유지합니다.
- Gateway ingress path: `GatewayApp`는 ingress token bucket, active-session cap, readiness gate, backend circuit-open gate를 조합해 새 연결 admission을 제한합니다.
- Gateway backend bridge path: `BackendConnection`은 bounded send queue, retry budget, exponential retry backoff, circuit breaker metrics를 함께 사용해 backend 장애 전파를 제한합니다.

## 소유권/수명주기 규칙
- 비동기 전송 객체 소유권에는 `std::shared_ptr`를 사용합니다.
- `Connection`의 송신 큐는 in-flight `async_write` 버퍼 수명을 close/queue-clear 이후까지 유지해야 합니다.
- 콜백 핸들러는 논블로킹·예외 안전을 유지합니다.
- 모듈 경계를 넘어 내부 가변 상태에 직접 접근하지 않습니다.

## Verification Pointers
- Bounded queue / transport backpressure: `tests/core/test_core_net.cpp`
- Policy primitives (`TokenBucket`, `RetryBudget`, `CircuitBreaker`): `tests/core/test_gateway_resilience_controls.cpp`
