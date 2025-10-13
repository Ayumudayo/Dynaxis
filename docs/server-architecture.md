 서버 애플리케이션 구조

`server/` 모듈은 `core/` 라이브러리에 구축된 공통 인프라를 재사용해 실행되는 실행 파일(`server_app`)로, 현재는 채팅 서비스를 중심으로 Gateway 역할과 도메인 서비스를 함께 담당한다. 네트워크 I/O는 Boost.Asio로 처리하고, 영속 계층은 PostgreSQL과 Redis에 의존한다. 장기적으로는 다른 실시간 서비스가 같은 코어를 활용할 수 있도록 공통 추상화를 유지한다.

## 부팅 및 기본 초기화
- **환경 변수 로딩**: `run_server()`가 `.env`를 우선 로딩하고, 이후 OS 환경 변수를 덮어쓴다. CI/테스트 환경에서는 `.env.example`을 기반으로 재생성한다.
- **부트스트랩 순서**
  1. `asio::io_context` 생성 및 `work_guard` 유지
  2. `core::JobQueue`, `core::ThreadManager` 초기화
  3. `core::BufferManager`(2KB × 1024 슬롯) 준비
  4. `core::Dispatcher`에 opcode → handler 매핑 등록
  5. `core::SessionOptions` 값 설정: `read_timeout_ms=60_000`, `heartbeat_interval_ms=10_000`, `recv_max_payload`·`send_queue_max` 등
  6. `core::SharedState`에 세션 카운터, 채널, gateway id 등 공유 상태 저장
- **외부 리소스 연결**
  - `DB_URI`가 설정되면 `server::storage::postgres::make_connection_pool()`을 통해 PQxx 기반 풀을 만들고 health-check 타이머를 구동한다.
  - `REDIS_URI`가 설정되면 `server::storage::redis::make_redis_client()`로 클라이언트를 구성하고 선택적으로 Pub/Sub 또는 Streams를 사용한다.
- **서비스 구성**: `server::app::chat::ChatService`가 I/O 컨텍스트와 JobQueue, DB/Redis 클라이언트를 주입받아 라우팅 테이블을 완성한다.
- **Acceptor**: `core::Acceptor`가 TCP listen socket을 열고 `ChatService::on_session_close()`를 종료 콜백으로 바인딩한다.
- **스레드 풀**: 기본적으로 CPU 코어 수만큼(최소 1개) 워커 스레드를 띄워 JobQueue를 처리하고, 별도 스레드에서 `io_context.run()`을 호출한다.
- **부가 컴포넌트**
  - Redis Pub/Sub (`USE_REDIS_PUBSUB!=0`): `fanout:room:*` 채널을 구독하여 타 서버의 메시지를 브로드캐스트
  - Metrics HTTP (`METRICS_PORT`): Prometheus 텍스트 포맷으로 런타임 지표 노출
  - OS 시그널 핸들러(SIGINT/SIGTERM): graceful shutdown 수행

## 메시지 처리 파이프라인
1. `core::Session::start()`가 클라이언트에 `MSG_HELLO`를 전송하고, 이후 `do_read_header()` → `do_read_body()` 루프를 진입한다.
2. 패킷이 수신되면 `core::Dispatcher`가 opcode에 맞는 핸들러를 찾아 호출하고, 없으면 `MSG_ERR(UNKNOWN_MSG_ID)`를 반환한다.
3. 주요 opcode 매핑
   | Opcode | 핸들러 | 요약 |
   | ------ | ------ | ---- |
   | `MSG_PING/MSG_PONG` | `on_ping` | heartbeat 유지, Redis presence TTL 갱신 |
   | `MSG_LOGIN_REQ` | `on_login` | 사용자 검증, guest 생성, audit log |
   | `MSG_JOIN_ROOM` | `on_join` | 채팅방 생성/입장, membership upsert |
   | `MSG_CHAT_SEND` | `on_chat_send` | 일반 채팅, `/whisper`, `/rooms` 등 명령 처리, 메시지 영속화 |
   | `MSG_LEAVE_ROOM` | `on_leave` | 퇴장 처리, presence 정리 |
   | `MSG_WHISPER_REQ` | `on_whisper` | 귓속말 전달, 대상 검색 |
   | 기타 | `on_session_close` | 세션 종료/타임아웃 처리 |
4. 핸들러는 CPU 집약 작업을 직접 수행하지 않고 `core::JobQueue`에 비동기 작업을 투입하거나 `DbWorkerPool`에 데이터베이스 작업을 위임한다.
5. 응답은 `core::Session::async_send()`를 통해 전송되며, `send_queue_max`를 초과하면 `runtime_metrics::record_send_queue_drop()`을 통해 드롭을 기록한다.

## 백그라운드 워커와 주기 작업
- **DbWorkerPool**: 비동기 DB 작업을 처리하는 워커 풀로, 각 작업은 `IUnitOfWork`를 통해 트랜잭션 범위를 제어한다. 작업 완료·실패·큐 깊이는 `runtime_metrics`에 수집된다.
- **TaskScheduler**: 지연 작업 및 반복 작업을 처리하는 경량 스케줄러. health-check, presence TTL 갱신, 지표 스냅샷 등에 사용한다.
- **Health Check 루틴**
  - DB: connection pool을 통해 `SELECT 1`을 실행하고 성공 여부를 기록
  - Redis: `PING` 또는 `hello`를 수행하여 RTT를 측정
- **CrashHandler**: 비정상 종료 시 minidump와 로그 버퍼를 `/logs/` 디렉터리에 남기고, 추후 Slack/Webhook 연동을 고려한다.

## 주요 환경 변수
| 이름 | 설명 | 기본값 |
| ---- | ---- | ------ |
| `SERVER_PORT` | TCP 리스닝 포트 | 5000 |
| `SERVER_BIND_ADDR` | 바인딩 주소 | `0.0.0.0` |
| `GATEWAY_ID` | 인스턴스 식별자 | `gw-default` |
| `DB_URI` | PostgreSQL 연결 문자열 | 미설정 시 비활성 |
| `REDIS_URI` | Redis 연결 문자열 | 미설정 시 비활성 |
| `USE_REDIS_PUBSUB` | Pub/Sub 사용 여부 | 0 |
| `WRITE_BEHIND_ENABLED` | Redis Streams 기반 write-behind | false |
| `METRICS_PORT` | Metrics HTTP 포트 | 0 (비활성) |
| `SNAPSHOT_RECENT_LIMIT` | 캐시할 최근 메시지 개수 | 20 |
| `SNAPSHOT_FETCH_FACTOR` | 초기 메시지 fetch 배수 | 3 |

환경 변수는 `.env`와 CI 환경 변수 관리 도구를 통해 통합 관리하며, 운영 환경별 프로파일을 문서화한다.

## 운영 중점 사항 / 기존 TODO
- [ ] Gateway �� Load Balancer �� Multi-instance �ܰ躰 �� ����(���� �����, ���� ����ȭ, ��� ���� �÷ο� ����)�� ����ȭ�Ѵ�.
- [x] Hive/Connection ��Ÿ�� ��Ʈ��ũ �߻�ȭ PoC(`server/core/net/hive.hpp`, `connection.hpp`, `listener.hpp`)�� �߰��ϰ� �⺻ �ۼ��š��������� �帧�� �����ߴ�.
- [ ] Gateway/Session ��층에 Hive/Connection PoC를 통합하고 기존 `core::Session` 전환 계획을 수립한다.
- [ ] ��ũ����/�÷����� Ȯ���� ���� ����ڽ� ��å�� ����� ���̾ƿ��� �����Ѵ�.
- [ ] � ���� ���ø�(k8s, systemd, Windows Service)�� �����ϰ� ǥ�� ���κ����� ������ �����Ѵ�.
- [ ] SLA/��� ���� üũ����Ʈ(�˶�, ���� ��ǥ, �ѹ� �÷ο�)�� ��üȭ�Ѵ�.

## 다단계 인프라 로드맵
- **1단계 Gateway**: TLS termination, rate limit, JWT 검증 등 ingress 공통 로직을 Gateway 계층에서 흡수하고, `service_registry`를 통해 각 인스턴스의 IP·health·세션 수를 공유한다. 초기에는 채팅용 라우터로 시작하되 HTTP/gRPC proxy 형태로 확장 가능하도록 설계한다.
- **2단계 Load Balancer**: Session affinity를 유지하기 위해 Redis/Consul 기반 분산 상태 저장소에 roomId·userId 키를 기록하고, shard-aware hashing으로 라우팅한다. Health probe는 TaskScheduler 기반 주기 검사와 CrashHandler webhook을 통합해 장애 감지를 단순화한다.
- **3단계 Multi-Instance Orchestration**: `DbWorkerPool`과 백그라운드 큐를 기반으로 Redis Streams 혹은 메시지 버스를 통해 인스턴스 간 이벤트를 fan-out 한다. 룸 소유권 이전, on-demand scale-out, warm standby 전략을 문서화한다.
- **Knights 적용 요약**: ServiceRegistry는 인스턴스 디스커버리를 담당하고, TaskScheduler는 health/keep-alive 스케줄링에 사용한다. LockedQueue는 gateway↔core 간 메시지 큐로 재사용해 백프레셔를 관리한다. Sapphire에서 차용한 구조를 채팅 서버 외의 실시간 서비스에도 적용할 수 있도록 가이드한다.

### Gateway 세부 설계
- **수신 파이프라인**: 새 `server/core/net/hive.hpp`, `connection.hpp`, `listener.hpp` 구성요소로 Gateway 전용 Hive를 구성하고, TLS(선택) + TCP handshake → `gateway::Hive`(ASIO strand) → Core IPC(ZeroMQ/Named pipe/gRPC) 순으로 이어진다.
- **세션 디렉터리**: `service_registry`를 통해 할당된 gateway id, connection id를 Core에 전달하고, Redis Hash(`gateway:{id}:sessions`)에 사용자·세션 메타데이터를 캐시한다.
- **보안/검증**: JWT 또는 HMAC 토큰 검증, IP allowlist, rate limit(bucket4j 스타일)을 전처리 단계에서 수행해 Core 로직을 단순화한다.
- **관측성**: Prometheus exporter에서 accept, TLS 오류, 인증 실패, backend 라우팅 시간을 측정하고 Core의 runtime_metrics와 결합한다.
- **장애 복구**: CrashHandler 연동으로 dump 생성, gateway 프로세스 감시(supervisor/systemd). 재시작 시 Redis에 남긴 세션을 스캔해 Core와 재협상한다.

### Load Balancer 세부 설계
- **라우팅 테이블**: `lb::StateStore`가 Redis/Consul watcher로 인스턴스 health와 세션 카운트를 추적하고, roomId 기반 consistent hashing 테이블을 유지한다.
- **전달 경로**: Gateway → Load Balancer → Target Core 간 gRPC/QUIC 스트림을 고려한다. Latency 기반 가중치 조정과 warm-up grace period를 지원한다.
- **세션 Affinity**: room 단위는 hashing, user 단위는 Redis set(`user:{id}:instance`)로 고정. Core 장애 시 Load Balancer가 재할당을 지시하고 Gateway가 세션 재수립을 유도한다.
- **Health Probe**: Core에서 제공하는 `/healthz` HTTP endpoint + TaskScheduler로 주기 체크, Redis heartbeat 키 만료를 watch하여 이중 검증.
- **백프레셔 처리**: Target Core queue depth가 임계치를 넘으면 Load Balancer가 Throttling 응답(HTTP 429 equivalent)을 반환하거나 대체 shard로 분산한다.

### Multi-Instance Orchestration 세부 설계
- **메시지 버스**: Redis Streams(`chat.events`), NATS 또는 Kafka를 후보로 두고, 이벤트 타입(ROOM_CREATED, MESSAGE_PERSISTED, SESSION_TRANSFER 등)을 스키마화한다.
- **소유권 이전**: Room shard 이동 시 Source Core가 snapshot + last message id를 Target Core에 전달하고, `DbWorkerPool` 트랜잭션 내에서 ownership flag를 갱신한다.
- **상태 동기화**: Presence, typing indicator 등 실시간 상태는 Redis Pub/Sub/Keyspace notification으로 교환하고, consistency 보장을 위해 TTL + periodic reconcile 작업을 TaskScheduler로 수행한다.
- **장애 시나리오**: Core 다운 시 Load Balancer가 해당 shard를 블랙리스트 처리 → Gateway가 세션 재협상을 요청 → 새 Core가 Redis/DB에서 상태를 복원.

### 상태 저장소 프로토타입
- Redis 기반 인스턴스 레지스트리: `load_balancer_app`은 `LB_REDIS_URI` (또는 `REDIS_URI`) 환경 변수를 통해 Redis 연결 문자열을 받아 `RedisInstanceStateBackend`를 사용한다. 환경 변수가 설정되지 않으면 InMemory 백엔드로 자동 폴백한다.
- Consul 연동은 선택 사항이며 추후 다중 데이터센터 환경이 필요할 때 추가한다.
- gRPC 연동: `load_balancer_app`은 `LB_GRPC_LISTEN`(기본 `127.0.0.1:7001`)으로 gRPC 서버를 열고, Gateway는 `LB_GRPC_ENDPOINT` 환경 변수로 접속한다. CI/개발 환경에서는 `LB_GRPC_REQUIRED=1`을 설정해 실패 시 테스트가 실패하도록 강제할 수 있다.
- server/state/instance_registry.hpp�� InMemoryStateBackend�� ���� �ڷᱸ���Ե��� �ڵ����� ������ ���÷δ�. 테스트�� state_instance_registry_tests.cpp���� GTest�� ����/touch/삭제 플로우�� 검증한다.
- RedisInstanceStateBackend�� Redis의 TTL(setex)을 활용해 gateway/instances/{id} 키에 JSON 메타데이터를 기록하고, 로컬 캐시로 즉시 조회를 지원한다. make_redis_state_client() 어댑터로 기존 server_storage_redis 클라이언트를 주입한다.
- ConsulInstanceStateBackend�� HTTP PUT/DELETE 콜백을 주입받아 Consul KV와 연동하는 패턴을 시뮬레이션하며, 향후 실제 HTTP 클라이언트로 교체 가능하다.

### 흐름 예시 (Login → Chat)
1. 클라이언트가 Gateway에 TLS 연결을 수립하고 JWT를 제시한다.
2. Gateway가 토큰 검증 후 Load Balancer에 room/user id로 라우팅 질의를 수행한다.
3. Load Balancer가 Target Core를 선택하고 Gateway가 Core IPC 채널을 연다.
4. Core `ChatService`가 로그인 처리 후 Redis presence 업데이트 및 `DbWorkerPool`로 audit 로그를 기록한다.
5. 메시지 송신 시 Core가 DB에 기록 → Redis Streams로 fan-out → 다른 Core/Gateway가 subscribe 하고, 최종적으로 대상 세션에 전달된다.

### 별도 실행 프로그램 구성
- **Gateway 애플리케이션(`gateway_app`)**: `gateway/` 모듈에서 Hive/Connection 스켈레톤을 활용해 TLS 종료·인증·세션 등록을 담당한다. `GatewayApp::run_smoke_test()`는 `server::core::net::Listener`와 `GatewayConnection`을 사용해 로컬 루프백 접속(동적으로 할당된 포트)에서 인증 토큰 핸드셰이크→데이터 수신까지 검증하고, 성공 시 Hive를 graceful stop 한다.
- **Load Balancer 애플리케이션(`load_balancer_app`)**: `load_balancer/` 모듈은 InMemory → Redis/Consul 상태 저장소로 인스턴스 정보를 관리하고, Hive 기반 health probe를 확장할 준비가 되어 있다. Smoke 테스트는 `LoadBalancerApp::run_smoke_test()`에서 인스턴스 레코드를 upsert/touch 한 뒤 Hive를 graceful stop 하며 검증한다.
- **빌드 구성**: 루트 `CMakeLists.txt`에서 `add_subdirectory(gateway)`, `add_subdirectory(load_balancer)`를 추가해 두 실행 파일을 생성한다. 두 타깃 모두 `server_core`를 링크하며, Load Balancer는 상태 저장소 공유를 위해 `server_state` 라이브러리도 링크한다.

### 실행 우선순위
1. **단기**: Gateway 프로토타입 (TCP→Core IPC) + 기본 health probe + session registry 연동.
2. **중기**: Load Balancer 샤딩 알고리즘 구현 및 backpressure 정책 시험.
3. **장기**: Multi-instance 이벤트 버스 도입과 room ownership migration 자동화, 운영 툴 체인 구축.

## TODO (엔진화 준비)
- [ ] Gateway → Load Balancer → Multi-instance 단계별 상세 설계(세션 라우팅, 상태 동기화, 장애 감지 플로우 포함)를 문서화한다.
- [x] Hive/Connection 스타일 네트워크 추상화 PoC(`server/core/net/hive.hpp`, `connection.hpp`, `listener.hpp`)를 추가하고 기본 송수신·백프레셔 흐름을 검증했다.
- [ ] Gateway/Session 계층에 Hive/Connection PoC를 통합하고 기존 `core::Session` 전환 계획을 수립한다.
- [ ] 스크립팅/플러그인 확장을 위한 샌드박스 정책과 저장소 레이아웃을 정의한다.
- [ ] 운영 배포 템플릿(k8s, systemd, Windows Service)을 마련하고 표준 프로비저닝 절차를 정리한다.
- [ ] SLA/장애 대응 체크리스트(알람, 성공 지표, 롤백 플로우)를 구체화한다.
