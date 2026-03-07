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

## 소유권/수명주기 규칙
- 비동기 전송 객체 소유권에는 `std::shared_ptr`를 사용합니다.
- `Connection`의 송신 큐는 in-flight `async_write` 버퍼 수명을 close/queue-clear 이후까지 유지해야 합니다.
- 콜백 핸들러는 논블로킹·예외 안전을 유지합니다.
- 모듈 경계를 넘어 내부 가변 상태에 직접 접근하지 않습니다.
