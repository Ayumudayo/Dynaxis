# Server Architecture Overview

Knights 서버 스택은 `server_app`(채팅 백엔드), `gateway_app`(외부 진입점), `load_balancer_app`(세션 라우터) 세 프로세스로 구성된다. 모든 실행 파일은 `core/` 모듈이 제공하는 공통 인프라(네트워크, 스레드, 스케줄러, 스토리지 어댑터)를 공유한다.

## 실행 구성요소
### server_app (`server/src/app/bootstrap.cpp`)
- **네트워크**: Boost.Asio `io_context` + `core::Acceptor`가 TCP 세션을 수락한다. 각 세션은 `core::Session`으로 구성되며 커스텀 헤더/바디 프레이밍을 수행한다.
- **디스패처**: `core::Dispatcher`가 opcode→handler 맵을 유지한다. 예: `MSG_CHAT_SEND` → `server::app::chat::on_chat_send`.
- **작업 인프라**: `core::JobQueue`, `core::ThreadManager`, `core::concurrent::TaskScheduler`, `core::storage::DbWorkerPool`이 백그라운드 I/O를 담당한다.
- **스토리지**: PostgreSQL(`DB_URI`), Redis(`REDIS_URI`) 클라이언트를 DI 컨테이너(`services::set`)에 등록해 도메인 서비스가 가져다쓴다.
- **도메인 서비스**: `server::app::chat::ChatService`가 채팅, 룸, presence 로직을 캡슐화한다.

### gateway_app (`gateway/`)
- TCP 게이트웨이. Boost.Asio `Hive` 기반으로 클라이언트 연결을 수락하고, 최초 메시지에서 간단한 토큰 파싱(익명 허용)을 수행한다.
- 세션마다 gRPC 스트림을 열어 Load Balancer로 프레임(`RouteMessage`)을 전달한다. 백엔드에서 역방향으로 수신한 메시지는 `Connection::async_send()`로 클라이언트에 전송된다.

### load_balancer_app (`load_balancer/`)
- gRPC 서버(`LoadBalancerService::Stream`)가 Gateway 스트림을 수신한다.
- Consistent Hash ring과 Redis 기반 세션 디렉터리(`LB_SESSION_TTL`)로 `client_id`→backend 매핑을 유지하고, 매핑이 없을 때만 라운드로빈으로 폴백한다.
- Redis(`LB_REDIS_URI`/`REDIS_URI`)가 설정되면 `gateway/instances/*` heartbeat와 `gateway/session/<client>` 매핑을 함께 관리한다.

자세한 게이트웨이·로드밸런서 런타임은 [docs/ops/gateway-and-lb.md](ops/gateway-and-lb.md)를 참고한다.

## 부팅 순서 (server_app)
1. `.env` 로딩 (`server/core/config/dotenv.hpp`).
2. `asio::io_context`, `core::JobQueue`, `core::ThreadManager`, `core::BufferManager` 초기화.
3. `core::Dispatcher`에 opcode 핸들러 등록.
4. `core::SessionOptions` (`read_timeout_ms`, `heartbeat_interval_ms` 등) 설정.
5. PostgreSQL/Redis 커넥션 풀 생성 후 DI 컨테이너에 주입.
6. `ChatService` 생성, 라우팅 테이블 등록.
7. `core::Acceptor` listen 후 워커 스레드/타이머(`TaskScheduler`, health check) 기동.
8. OS 시그널(SIGINT/SIGTERM) 수신 시 graceful shutdown 수행.

## 메시지 처리 파이프라인
1. `core::Session::start()` → `MSG_HELLO` 전송 → read loop 진입.
2. 프레임 수신 시 `core::Dispatcher`가 opcode별 핸들러 호출. 미등록 opcode는 `MSG_ERR(UNKNOWN_MSG_ID)` 반환.
3. 주요 핸들러
   - `MSG_LOGIN_REQ` → guest 생성 및 presence 초기화
   - `MSG_JOIN_ROOM` → membership upsert, Redis presence 업데이트
   - `MSG_CHAT_SEND` → 메시지 영속화, Redis Stream(write-behind) 발행 옵션
   - `MSG_LEAVE_ROOM` → membership/presence 정리
4. CPU·I/O 집중 처리는 `core::JobQueue` 또는 `DbWorkerPool`에 위임해 세션 스레드를 가볍게 유지한다.
5. 응답은 `core::Session::async_send()` 큐를 통해 비동기 전송하며, 큐 초과 시 `runtime_metrics::record_send_queue_drop()`로 드롭을 기록한다.

## 백그라운드 작업
- **DbWorkerPool**: Postgres 트랜잭션 작업을 비동기로 처리, 성공/실패/큐 길이를 메트릭으로 노출.
- **TaskScheduler**: health check, presence TTL 갱신, 통계 집계를 주기적으로 실행한다.
- **Health Check**: DB `SELECT 1`, Redis `PING/HELLO`, 외부 서비스 커넥션 점검.
- **CrashHandler**: 비정상 종료 시 minidump 및 로그 버퍼를 `/logs/`에 남긴다.

## 핵심 환경 변수
| 컴포넌트 | 변수 | 기본값/설명 |
| --- | --- | --- |
| server_app | `SERVER_BIND_ADDR`, `SERVER_PORT` | 기본 `0.0.0.0:5000` |
|  | `GATEWAY_ID` | 다중 게이트웨이 self-echo 방지 |
|  | `DB_URI`, `REDIS_URI` | 스토리지 연결 문자열 |
|  | `WRITE_BEHIND_ENABLED`, `USE_REDIS_PUBSUB` | Redis Streams/PubSub 제어 |
| gateway_app | `GATEWAY_LISTEN` | `0.0.0.0:6000` |
|  | `LB_GRPC_ENDPOINT` | Load Balancer gRPC 위치 |
|  | `GATEWAY_ID` | Redis presence/로그 태깅 |
| load_balancer_app | `LB_GRPC_LISTEN` | `127.0.0.1:7001` |
|  | `LB_BACKEND_ENDPOINTS` | 콤마 구분 백엔드 목록 |
|  | `LB_REDIS_URI` | Redis 상태 백엔드 (옵션) |
|  | `LB_INSTANCE_ID` | 상태 레지스트리 키 |
|  | `LB_SESSION_TTL` | 세션→백엔드 매핑 TTL(초) |

## 향후 과제
- **인증**: `auth::NoopAuthenticator` 대신 토큰 검증, 사용자 레지스트리 연동.
- **멀티 인스턴스**: Redis state/metrics 기반 부하 분산 및 세션 재할당.
- **관측성**: gateway/load_balancer에 Prometheus 메트릭과 헬스 엔드포인트 추가.
- **자동 회복**: 백엔드 접속 실패 시 지연 재시도, 백엔드 health probe.
