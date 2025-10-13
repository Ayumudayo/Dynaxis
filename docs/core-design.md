# 서버 코어 상세 설계

`core/` 모듈은 서버 공용 런타임을 제공하는 C++20 static library(`server_core`)이며, 네트워크 세션, 메시지 프레이밍, 런타임 메트릭, 공용 스토리지 SPI를 한곳에 모은다. 실제 게임/도메인 로직은 다른 모듈에서 이 라이브러리를 링크해 사용한다.

## 목적 및 범위
- Boost.Asio 기반 TCP 서버 인프라를 공통으로 제공한다.
- 프레이밍/헤더 처리, heartbeat, 기본 백프레셔, 예외 안전한 디스패치 루프를 보장한다.
- 런타임 상태(`runtime_metrics`)와 최소한의 로그/metrics SPI를 노출해 외부 관측 도구로 교체 가능하도록 한다.
- 스토리지/유닛오브워크 인터페이스는 구현을 강제하지 않고, 서버/서비스 레이어에서 주입하도록 정의만 제공한다.
- 장기적으로는 게임 서버 등 다른 도메인에도 그대로 재사용할 수 있는 범용 런타임을 목표로 한다.

> **현재 적용 범위**  
> 1차 적용 대상은 `server/app` 채팅 서버이므로, 네트워크·스토리지·메트릭 설계는 채팅 워크로드 중심으로 우선순위를 잡았다.  
> 다른 유형의 서버에 맞춰 확장할 때는 본 문서의 모듈 구분과 SPI를 기반으로 기능을 보강한다.

## 엔진화 목표와 현재 범위
- **서버 엔진(Server Engine)**: ServiceRegistry, TaskScheduler, DbWorkerPool 등 코어 모듈을 공통 추상화로 유지해 향후 음성/게임/RPC 서비스까지 지원하는 확장성을 확보한다.
- **현재 범위(채팅 중심)**: 최신 스프린트에서는 채팅 흐름에 집중하여 heartbeat, presence, 메시지 영속화와 같은 필수 기능을 우선순위에 둔다. Redis/DB 튜닝도 채팅 시나리오 기준으로 맞춘다.
- **확장 전략**: 모듈 간 의존도를 낮추고 인터페이스 계약을 문서화하여 다양한 실시간 서비스가 동일 코어를 재활용할 수 있게 한다.

## 최근 품질 보강
- 2025-10-13: TaskScheduler, DbWorkerPool GTest 케이스를 추가해 지연·예외 경로 회귀를 방지한다.
- 2025-10-13: Gateway → Load Balancer → Multi-instance 아키텍처 로드맵을 docs/server-architecture.md에 반영했다.
- 2025-10-13: `net/hive.hpp`, `net/connection.hpp`, `net/listener.hpp`�� Hive/Connection ���� �߻�ȭ PoC�� �����ϰ�, `server/state/instance_registry.hpp` ���� Redis/Consul ���� ���� ������Ʈ�� �����ߴ�.
## 디렉터리 구성
- `core/include/server/core/net/` : `Acceptor`, `Session`, `Dispatcher` 등 I/O 계층
- `core/include/server/core/memory/` : 고정 블록 `MemoryPool`, `BufferManager`
- `core/include/server/core/concurrent/` : 간단한 `JobQueue`, `ThreadManager`
- `core/include/server/core/config/` : `.env` 로딩 유틸리티
- `core/include/server/core/metrics/` : 노출된 Counter/Gauge/Histogram SPI
- `core/include/server/core/runtime_metrics.hpp` : 경량 런타임 카운터 스냅샷
- `core/include/server/core/state/` : `SharedState`
- `core/include/server/core/protocol/` : 프레임 구조 및 opcode/flags/error 정의
- `core/include/server/core/storage/` : Repository 및 UnitOfWork SPI
- `core/include/server/wire/codec.hpp` : Protobuf ↔ msg_id 매핑 헬퍼
- `core/src/**` : 각 인터페이스의 기본 구현
- `gateway/` : Gateway ���� ���� ��Ż. Hive/Connection ����� ���� smoke test(`GatewayApp::run_smoke_test`)�� ����Ǿ� 있으며 TLS ��ȣ·세션 등록 로직을 ��� ������ �����Ѵ�.
- `load_balancer/` : Load Balancer ���� ���� ��Ż. InMemory/Redis/Consul ��� ���� ������ `LoadBalancerApp::run_smoke_test()`���� ��Ű��ȭ�ϸ� ��후 샤딩·health probe ��å�� �����Ѵ�.

## 핵심 모듈 요약

### Net 계층 (`core/src/net`)
- **Acceptor**(`acceptor.cpp`): 지정 endpoint에서 listen하고 `async_accept`로 세션을 만든다. `SharedState::max_connections`를 넘어가면 새 커넥션을 즉시 닫고 로그를 남긴다. 성공한 accept는 `runtime_metrics::record_accept()`로 계수한다.
- **Session**(`session.cpp`): 세션 시작 시 `MSG_HELLO`를 송신하고, 헤더→본문 순서로 비동기 read를 반복한다.
  - `BufferManager`로 받은 고정 블록에 헤더/본문을 저장한다.
  - `SessionOptions` 기반으로 `recv_max_payload`, `send_queue_max`, `heartbeat_interval_ms`, `read_timeout_ms`를 적용한다. `write_timeout_ms` 필드는 아직 사용하지 않는다.
  - 송신 큐는 `std::queue`이며 상한을 넘으면 즉시 세션을 종료하고 `runtime_metrics::record_send_queue_drop()`을 남긴다.
  - heartbeat는 단순히 주기적으로 타이머를 재설정하여 클라이언트의 PING에 대비하도록 한다(서버가 직접 PING을 발송하지는 않는다).
- **Dispatcher**(`dispatcher.cpp`): `msg_id -> handler` 매핑을 보관한다. 등록되지 않은 opcode는 `false`를 반환하고, 예외가 발생하면 catch하여 `MSG_ERR` 응답을 보내고 로그/메트릭을 기록한다.

### 메모리 & 버퍼 관리 (`memory/memory_pool.*`)
- `MemoryPool`은 고정 크기 블록 스택을 보호하는 mutex 기반 풀이다. 부족하면 `nullptr`을 반환하여 호출자가 graceful fallback 할 수 있게 한다.
- `BufferManager`는 `unique_ptr` + custom deleter로 풀 반환을 자동화한다. acquire/release 시 `runtime_metrics`에 사용량이 반영된다.

### 런타임 상태 (`runtime_metrics.*`)
- accept/세션/프레임/디스패치/메모리 풀 등 주요 지표를 lock-free `std::atomic`으로 축적한다.
- `snapshot()`은 현재 카운터값과 `opcode_counts` 벡터를 복사한다. 소비자는 폴링 방식으로 노출하면 된다.

### Metrics SPI (`metrics/metrics.*`)
- `metrics::Counter`, `Gauge`, `Histogram` 인터페이스만 정의한다.
- 기본 구현은 no-op이므로 외부에서 어댑터를 등록하지 않으면 호출 비용만 발생한다. 런타임 메트릭은 별도(`runtime_metrics`)로 계수한다.

### 동시성 유틸 (`concurrent/*.cpp`)
- `JobQueue`는 mutex + condition_variable 기반 FIFO 큐이며, pop 시점마다 `runtime_metrics::record_job_queue_depth()`를 갱신한다.
- `ThreadManager`는 `JobQueue`를 소비하는 워커풀 래퍼다. `Stop()`을 호출하면 큐에 종료 신호를 전파한다.

### 설정 및 공유 상태
- `config::load_dotenv()`는 `.env` 파일에서 `key=value`를 읽어 환경 변수에 주입한다. `export` 접두어와 각종 따옴표를 허용한다.
- `SessionOptions`는 세션별 정책을 모아둔 구조체로, 현재 구현에서 실제로 사용하는 필드는 `recv_max_payload`, `send_queue_max`, `heartbeat_interval_ms`, `read_timeout_ms` 네 가지다.
- `SharedState`는 전역 접속 수, 최대 허용치, 세션 ID 시퀀스를 추적한다. 세션 생성/종료 시 atomic 카운터를 증감한다.

### 프로토콜 보조
- `protocol/frame.hpp`는 14바이트 고정 헤더(`FrameHeader`)와 BE 인코딩 유틸, UTF-8 길이 접두 인코더를 제공한다.
- `protocol/opcodes.hpp`, `protocol/protocol_flags.hpp`, `protocol/protocol_errors.hpp`는 tools 스크립트로 생성되며 값 정의만 포함한다.
- `server/wire/codec.hpp`는 Protobuf message ↔ opcode 매핑과 Encode/Decode 헬퍼를 제공한다. 현재는 서버→클라이언트 응답형 메시지 위주로 정의되어 있다.

### 스토리지 SPI (`storage/*.hpp`)
- Repository DTO(`User`, `Room`, `Message`, `Membership`, `Session`)와 관련 추상 인터페이스를 선언한다.
- `IUnitOfWork`는 repository accessor와 `commit/rollback`만 정의하며, 실제 구현은 server/service 모듈에서 제공해야 한다.
- `IConnectionPool`은 `make_unit_of_work()`와 헬스체크만 정의한다.

## 세션 수명주기
1. **생성**: `Acceptor`가 새 소켓을 성공적으로 받아오면 `Session`을 생성한다. `SharedState::connection_count`를 증가시키고 `session_id`를 할당한다.
2. **시작**: `Session::start()`가 `send_hello()`를 호출해 `MSG_HELLO` 패킷을 전송한다.
   - HELLO payload: `proto_major(1)`, `proto_minor(1)`, `capabilities`(현재 `CAP_COMPRESS_SUPP | CAP_SENDER_SID`), 하트비트 간격(1/10 ms), `epoch_high32`.
3. **수신 루프**: 고정 헤더를 읽어 길이/flags/opcode를 파싱한 뒤, `recv_max_payload`와 메모리 풀 블록 크기를 동시에 검사한다.
4. **디스패치**: `Dispatcher::dispatch`로 handler를 호출한다. 실행시간은 `runtime_metrics::record_dispatch_attempt`에 nanoseconds 단위로 기록된다.
5. **종료**: 오류, 타임아웃, 백프레셔 위반, 외부 명령 중 하나가 발생하면 `stop()`이 호출된다. 소켓을 shutdown/close 하고 타이머를 취소한 뒤 카운터를 감소시킨다. `on_close_` 콜백이 등록되어 있으면 마지막에 실행된다.

현재 구현에는 문서상 언급되던 `Draining` 상태 머신이나 별도 write 타임아웃이 존재하지 않는다. 향후 필요 시 도입한다.

## 타임아웃과 heartbeat
- **Read Timeout**: `Session::arm_read_timeout()`이 `read_timeout_ms`만큼 타이머를 설정하고, 만료 시 `read timeout` 로그와 함께 세션을 종료한다.
- **Heartbeat Timer**: `heartbeat_interval_ms`가 0보다 크면 타이머를 반복 설정한다. 현재는 서버가 PING 프레임을 직접 보내지 않으므로, 클라이언트의 heartbeat 정책에 의존한다.
- **Write Timeout**: `SessionOptions::write_timeout_ms` 필드는 정의만 되어 있고 아직 사용되지 않는다.

## 백프레셔 및 큐 정책
- 송신 큐는 프레임 단위 FIFO다. `queued_bytes_`가 `send_queue_max`를 초과하면 더 이상 쌓지 않고 세션을 중단한다(워터마크 기반 드레인 정책은 미구현이다).
- 수신 본문 길이가 `recv_max_payload`를 넘어가면 `MSG_ERR(LENGTH_LIMIT_EXCEEDED)`를 전송하고 세션을 종료한다.

## 관측성
- `runtime_metrics`에서 노출하는 주요 카운터는 아래와 같다.
  - `accept_total`, `session_started_total`, `session_stopped_total`, `session_active`
  - `session_timeout_total`, `heartbeat_timeout_total`, `send_queue_drop_total`
  - `frame_total`, `frame_error_total`, `frame_payload_sum/count/max`
  - `dispatch_total`, `dispatch_unknown_total`, `dispatch_exception_total`
  - `dispatch_latency_sum/count/last/max` (단위: ns)
  - `job_queue_depth`, `job_queue_depth_peak`
  - `memory_pool_capacity`, `memory_pool_in_use`, `memory_pool_in_use_peak`
  - `opcode_counts` : 사용된 opcode와 그 빈도를 벡터로 반환
- 별도의 `metrics::Counter` 등은 현재 no-op이므로 Prometheus 등 외부 백엔드를 붙이려면 adapter를 공급해야 한다.
- `log`는 단일 mutex로 직렬화된 stderr 출력이며, 기본 로그 레벨은 `info`다.

## 에러 처리
- Asio read/write 실패, 메모리 풀 고갈, payload 길이 오류 등은 `runtime_metrics::record_frame_error()`와 함께 세션 종료로 이어진다.
- 핸들러에서 예외가 발생하면 `MSG_ERR(INTERNAL_ERROR)`를 전송하고, 세션은 즉시 종료하지 않고 다음 프레임을 읽기 위해 루프를 계속한다.
- 알 수 없는 opcode 수신 시 `MSG_ERR(UNKNOWN_MSG_ID)`를 응답하고, 이후에도 연결은 유지된다.

## 향후 보강 항목 (TODO)
- 송신 큐 워터마크(high/low) 기반 점진적 배압 로직.
- 서버 주도 heartbeat(PING)과 `write_timeout_ms` 활용.
- 문서에 소개된 Codec 체인 플러그인 SPI는 아직 구현되지 않았다. 현재는 고정 헤더 + payload 구조만 지원한다.
- `ThreadManager`/`JobQueue`는 아직 서버 실행 경로에 직접 연결되지 않았다. 백그라운드 작업 사용 예제가 추가되면 좋은 상태다.
- 테스트 커버리지: `core/tests`가 존재하지 않으므로 향후 GoogleTest 기반 단위 테스트를 작성해야 한다.

현 시점 문서는 실제 코드(`core/src`, `core/include`) 기반으로 업데이트 되었으며, 추가 기능 구현 시 이 문서를 함께 갱신해야 한다.

## TODO (엔진화 레이어)
- [x] TaskScheduler/DbWorkerPool 단위 테스트 커버리지 확보 (2025-10-13).
- [ ] 서비스 io_context 분리를 전제로 한 Hive/Connection 네트워크 추상화 PoC.
- [ ] ECS/액터 기반 모듈식 동시성 모델 조사 및 채택 여부 결정.
- [ ] Lua/WASM 기반 스크립팅·플러그인 샌드박스 정책 초안 수립.
- [ ] 서비스 부트스트랩·종료 파이프라인 정비 및 표준 라이프사이클 훅 정리.
- [ ] 운영 관측성(로그, 메트릭, 트레이싱) 일원화와 SLA 기준 정의.
