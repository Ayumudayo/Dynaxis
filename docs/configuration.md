# 구성 가이드

Knights 프로젝트는 실행 환경에서 주입되는 **환경 변수**를 사용해 구성 요소를 설정한다. `.env` 파일은 예시/편의 용도로만 제공되며, 실제 로딩은 Docker Compose의 `env_file`, Kubernetes ConfigMap/Secret, 시스템 서비스 설정 등을 통해 수행하는 것을 전제로 한다.

## 1. 구성 로딩 순서
1. 커맨드라인 인자(예: 포트)가 있으면 우선 적용한다.
2. 환경 변수를 로드해 기본값을 오버라이드한다.
3. 각 애플리케이션은 필요 시 추가 검증(예: 필수 키 누락)을 수행하고 기본값을 적용한다.

멀티 인스턴스 환경에서는 공통 `.env`를 공유하기보다 인스턴스별로 필요한 값만 export한 뒤 실행하는 방식을 권장한다.

## 2. 핵심 환경 변수
### 2.1 서버 공통
| 변수 | 설명 |
| --- | --- |
| `DB_URI` | PostgreSQL 연결 문자열 (server_app, wb_worker) |
| `DB_POOL_MIN`, `DB_POOL_MAX` | DB 커넥션 풀 크기 |
| `REDIS_URI` | Redis 연결 문자열 (공통) |
| `REDIS_POOL_MAX` | Redis 커넥션 풀 최대치 |
| `WRITE_BEHIND_ENABLED` | Redis Streams write-behind 활성화 |
| `USE_REDIS_PUBSUB` | Redis Pub/Sub 브로드캐스트 활성화 |
| `REDIS_CHANNEL_PREFIX` | Pub/Sub 채널 접두어(`knights:` 등) |
| `GATEWAY_ID` | Gateway/Server 식별자 (Presence, Pub/Sub self-echo에 사용) |
| `ADMIN_COMMAND_SIGNING_SECRET` | admin command fanout payload HMAC-SHA256 서명 키 (admin_app/server_app 공통) |
| `KNIGHTS_TRACING_ENABLED` | 경량 트레이싱/상관키 전파 활성화 (`1`/`0`) |
| `KNIGHTS_TRACING_SAMPLE_PERCENT` | 트레이싱 샘플링 비율(0~100) |
| `LOG_ASYNC_QUEUE_CAPACITY` | 비동기 로거 큐 상한(기본 8192) |
| `LOG_ASYNC_QUEUE_OVERFLOW` | 비동기 로거 overflow 정책(`drop_newest|drop_oldest|block`) |
| `LOG_LEVEL` | 로그 출력 레벨(`trace|debug|info|warn|error`) |
| `LOG_BUFFER_CAPACITY` | 최근 로그 ring buffer 보관 개수(기본 256) |
| `LOG_FORMAT` | 로그 포맷(`text|json`) |

### 2.2 `server_app` 설정
| 항목 | 기본값 | 설명 |
| --- | --- | --- |
| `PORT` | `5000` | 서버 리스닝 포트(또는 실행 인자 argv[1]) |
| `SERVER_ADVERTISE_HOST` | `127.0.0.1` | Gateway가 backend를 찾을 때 사용하는 광고 호스트(예: Pod IP, 노드 IP, VIP) |
| `SERVER_ADVERTISE_PORT` | listen 포트 | Gateway가 접속할 포트(포트 매핑 시 외부 포트 지정) |
| `SERVER_INSTANCE_ID` | `server-<timestamp>` | Instance Registry에 등록되는 고유 서버 ID |
| `SERVER_ROLE` | `server` | 서버 역할 메타데이터(제어면 selector의 `roles` 필터 기준) |
| `SERVER_REGION` | `global` | 서버 지역 메타데이터(제어면 selector의 `regions` 필터 기준) |
| `SERVER_SHARD` | `default` | 서버 샤드 메타데이터(제어면 selector의 `shards` 필터 기준) |
| `SERVER_TAGS` | (unset) | 서버 태그 목록(CSV, 예: `canary,vip`, selector의 `tags` 필터 기준) |
| `SERVER_REGISTRY_PREFIX` | `gateway/instances/` | Redis Instance Registry prefix. server_app과 gateway_app이 동일한 prefix를 사용해야 한다. |
| `SERVER_REGISTRY_TTL` | `30` | Instance Registry TTL(초) |
| `SERVER_HEARTBEAT_INTERVAL` | `5` | Instance Registry heartbeat 주기(초) |
| `METRICS_PORT` | `0` | HTTP 관리 포트(`/metrics`, `/healthz`, `/readyz`; `server_app`는 `/logs`도 제공). 0이면 비활성 |
| `METRICS_HTTP_MAX_CONNECTIONS` | `64` | metrics/admin HTTP 동시 연결 상한(초과 시 503) |
| `METRICS_HTTP_HEADER_TIMEOUT_MS` | `5000` | metrics/admin HTTP header read timeout(ms, 초과 시 연결 종료 + timeout 카운터 증가) |
| `METRICS_HTTP_BODY_TIMEOUT_MS` | `5000` | metrics/admin HTTP body read timeout(ms, 초과 시 연결 종료 + timeout 카운터 증가) |
| `METRICS_HTTP_MAX_BODY_BYTES` | `65536` | metrics/admin HTTP 요청 body 최대 크기(바이트, 초과 시 413) |
| `METRICS_HTTP_AUTH_TOKEN` | (unset) | 설정 시 `/metrics` 계열 요청에 bearer 또는 `X-Metrics-Token` 인증 강제 |
| `METRICS_HTTP_ALLOWLIST` | (unset) | 콤마 구분 IP allowlist(미일치 요청 403) |
| `CHAT_HOOK_PLUGINS_DIR` | (unset) | (실험) 채팅 훅 플러그인 디렉터리. 존재하는 `.so/.dll`을 파일명 순으로 로드 |
| `CHAT_HOOK_PLUGIN_PATHS` | (unset) | (실험) 플러그인 경로 목록(순서 고정, 구분자 `;` 또는 `,`) |
| `CHAT_HOOK_PLUGIN_PATH` | (unset) | (실험, 레거시) 단일 플러그인 경로 |
| `CHAT_HOOK_ENABLED` | `0` | 채팅 훅 플러그인 런타임 활성화 (`1`/`0`). `0`이면 경로가 설정돼 있어도 훅 체인을 초기화하지 않음 |
| `CHAT_HOOK_CACHE_DIR` | (unset) | cache-copy 로딩용 캐시 디렉터리 |
| `CHAT_HOOK_RELOAD_INTERVAL_MS` | `500` | 핫 리로드 폴링 주기(ms) |
| `CHAT_HOOK_WARN_BUDGET_US` | `0` | 플러그인 hook 호출 경고 예산(마이크로초). 0이면 비활성 |
| `LUA_ENABLED` | `0` | Lua 스크립팅 경로 런타임 활성화 (`1`/`0`). 기본 배포 바이너리는 `BUILD_LUA_SCRIPTING=ON`을 전제로 하며, `OFF` 커스텀 빌드에서는 `1`이어도 비활성 경고만 출력 |
| `LUA_SCRIPTS_DIR` | (unset) | Lua 스크립트 디렉터리 (예: `/app/scripts`) |
| `LUA_FALLBACK_SCRIPTS_DIR` | (unset) | `LUA_SCRIPTS_DIR`가 비어 있거나 읽기 실패일 때 사용할 fallback 스크립트 디렉터리 (예: `/app/scripts_builtin`) |
| `LUA_LOCK_PATH` | (unset) | Lua 스크립트 리로드 lock/sentinel 파일 경로 (존재 시 watcher poll/reload 스킵) |
| `LUA_RELOAD_INTERVAL_MS` | `1000` | Lua 스크립트 리로드 폴링 주기(ms) |
| `LUA_INSTRUCTION_LIMIT` | `100000` | Lua 호출 1회당 instruction 제한 |
| `LUA_MEMORY_LIMIT_BYTES` | `1048576` | Lua 런타임 메모리 상한(바이트) |
| `LUA_AUTO_DISABLE_THRESHOLD` | `3` | 연속 오류 N회 도달 시 자동 비활성화 임계치 |
| `LUA_HOOK_WARN_BUDGET_US` | `0` | Lua cold hook 호출 경고 예산(마이크로초). 0이면 비활성 |
| `CHAT_ADMIN_USERS` | (unset) | 관리자 사용자 목록(구분자 `,` 또는 `;`) |
| `CHAT_SPAM_THRESHOLD` | `6` | 스팸 판정 메시지 개수 임계치 |
| `CHAT_SPAM_WINDOW_SEC` | `5` | 스팸 카운트 윈도우(초) |
| `CHAT_SPAM_MUTE_SEC` | `30` | 1차 스팸 위반 시 뮤트 지속 시간(초) |
| `CHAT_SPAM_BAN_SEC` | `600` | 누적 위반 시 밴 지속 시간(초) |
| `CHAT_SPAM_BAN_VIOLATIONS` | `3` | 밴으로 승격되는 누적 위반 횟수 |
| `ADMIN_COMMAND_TTL_MS` | `60000` | admin command payload 허용 TTL(ms). `issued_at + ttl` 초과 시 거부 |
| `ADMIN_COMMAND_FUTURE_SKEW_MS` | `5000` | admin command payload 미래 시각 허용치(ms). 초과 시 거부 |
| `SERVER_DRAIN_TIMEOUT_MS` | `15000` | shutdown drain 대기 상한(ms). timeout 초과 시 남은 연결은 강제 종료 경로로 정리 |
| `SERVER_DRAIN_POLL_MS` | `100` | shutdown drain 진행률(남은 연결 수) 폴링 주기(ms) |
| `WB_*` | - | write-behind 배치/지연/DLQ 등 세부 옵션 |
| `KNIGHTS_TRACING_ENABLED` | `0` | tracing context 생성/로그 상관키 주입 활성화 |
| `KNIGHTS_TRACING_SAMPLE_PERCENT` | `100` | tracing 샘플링 비율(0~100) |

Lua cold-hook 작성 기준:

- 권장 형식은 function-style hook + `ctx`다.
- 예: `function on_login(ctx) ... return { decision = "pass" } end`
- `server/scripts/`는 runtime image builtin fallback(`/app/scripts_builtin`) 소스이고, `docker/stack/scripts/`는 compose mount(`/app/scripts`) 샘플 경로다.
- return-table(`return { hook = "...", decision = "..." }`)와 directive(`-- hook=... decision=...`)는 fallback/testing aid로 유지한다.
- limit 시뮬레이션은 `-- hook=on_admin_command limit=instruction|memory` 형식을 사용한다.
- decision 우선순위는 `block/deny > handled > modify > pass/allow`이다.

### 2.3 `gateway_app` 전용
| 변수 | 기본값 | 설명 |
| --- | --- | --- |
| `GATEWAY_LISTEN` | `0.0.0.0:6000` | 게이트웨이 TCP 리스너 |
| `GATEWAY_ID` | `gateway-default` | 인스턴스 식별자(로그/Presence 등) |
| `REDIS_URI` | `tcp://127.0.0.1:6379` | Instance Registry/SessionDirectory용 Redis |
| `METRICS_PORT` | `6001` | HTTP 관리 포트(`/metrics`, `/healthz`, `/readyz`) |
| `GATEWAY_BACKEND_CONNECT_TIMEOUT_MS` | `5000` | backend(server_app) TCP 연결 타임아웃(ms) |
| `GATEWAY_BACKEND_SEND_QUEUE_MAX_BYTES` | `262144` | backend 전송 대기 큐 상한 바이트(초과 시 세션 종료) |
| `GATEWAY_BACKEND_CIRCUIT_BREAKER_ENABLED` | `1` | backend 연결 연속 실패 시 circuit breaker 활성화 여부(1/0) |
| `GATEWAY_BACKEND_CIRCUIT_FAIL_THRESHOLD` | `5` | circuit open으로 전환되는 연속 실패 임계치 |
| `GATEWAY_BACKEND_CIRCUIT_OPEN_MS` | `10000` | circuit open 유지 시간(ms) |
| `GATEWAY_BACKEND_CONNECT_RETRY_BUDGET_PER_MIN` | `120` | gateway 전체 backend connect 재시도 예산(분당) |
| `GATEWAY_BACKEND_CONNECT_RETRY_BACKOFF_MS` | `200` | backend connect 재시도 백오프 시작값(ms) |
| `GATEWAY_BACKEND_CONNECT_RETRY_BACKOFF_MAX_MS` | `2000` | backend connect 재시도 백오프 상한(ms) |
| `GATEWAY_INGRESS_TOKENS_PER_SEC` | `200` | ingress 토큰 버킷 초당 토큰 수 |
| `GATEWAY_INGRESS_BURST_TOKENS` | `400` | ingress 토큰 버킷 burst 용량 |
| `GATEWAY_INGRESS_MAX_ACTIVE_SESSIONS` | `50000` | gateway의 동시 backend 세션 상한 |
| `ALLOW_ANONYMOUS` | `1` | `0`이면 토큰 없는/anonymous 로그인 거부 |

#### 2.3.1 UDP ingress (현재)

| 변수 | 기본값 | 설명 |
| --- | --- | --- |
| `GATEWAY_UDP_LISTEN` | (unset) | 설정 시 gateway UDP ingress listener 활성화 (`host:port`) |
| `GATEWAY_UDP_BIND_SECRET` | (unset) | UDP bind ticket 검증 비밀키 |
| `GATEWAY_UDP_BIND_TTL_MS` | `5000` | bind ticket TTL(ms) |
| `GATEWAY_UDP_BIND_FAIL_WINDOW_MS` | `10000` | bind 실패 누적 윈도우(ms) |
| `GATEWAY_UDP_BIND_FAIL_LIMIT` | `5` | 윈도우 내 실패 허용 횟수 |
| `GATEWAY_UDP_BIND_BLOCK_MS` | `60000` | 실패 한도 초과 endpoint block 시간(ms) |
| `GATEWAY_UDP_BIND_RETRY_BACKOFF_MS` | `200` | 세션별 bind 실패 재시도 backoff 시작값(ms) |
| `GATEWAY_UDP_BIND_RETRY_BACKOFF_MAX_MS` | `2000` | 세션별 bind 실패 재시도 backoff 상한(ms) |
| `GATEWAY_UDP_BIND_RETRY_MAX_ATTEMPTS` | `6` | 세션별 bind 재시도 backoff 지수 증가 상한 attempt |
| `GATEWAY_UDP_OPCODE_ALLOWLIST` | (unset) | UDP ingress 허용 opcode CSV. 미설정/빈 값이면 최소 고정 allowlist(`MSG_UDP_BIND_REQ`)만 허용 |

#### 2.3.2 Core RUDP (기본 OFF)

아래 키는 gateway RUDP 어댑터 및 core RUDP 엔진 튜닝값이다. 런타임 기본값은 OFF이며,
`GATEWAY_RUDP_ENABLE=1` + `GATEWAY_RUDP_CANARY_PERCENT>0` + `GATEWAY_RUDP_OPCODE_ALLOWLIST`가 함께 설정되어야 실제 세션 경로에 적용된다.

| 변수/옵션 | 기본값 | 설명 |
| --- | --- | --- |
| `GATEWAY_RUDP_ENABLE` | `0` | gateway의 RUDP handshake/data path 활성화 게이트 |
| `GATEWAY_RUDP_CANARY_PERCENT` | `0` | 신규 세션 중 RUDP canary 비율(0~100) |
| `GATEWAY_RUDP_OPCODE_ALLOWLIST` | (empty) | RUDP 허용 opcode 목록(콤마 구분, 기본 비활성) |
| `GATEWAY_RUDP_HANDSHAKE_TIMEOUT_MS` | `1500` | RUDP HELLO/ACK 완료 타임아웃(ms) |
| `GATEWAY_RUDP_IDLE_TIMEOUT_MS` | `10000` | 유휴 상태 timeout(ms) |
| `GATEWAY_RUDP_ACK_DELAY_MS` | `10` | delayed ACK 기본 지연(ms) |
| `GATEWAY_RUDP_MAX_INFLIGHT_PACKETS` | `256` | 세션별 최대 in-flight 패킷 수 |
| `GATEWAY_RUDP_MAX_INFLIGHT_BYTES` | `262144` | 세션별 최대 in-flight 바이트 |
| `GATEWAY_RUDP_MTU_PAYLOAD_BYTES` | `1200` | datagram payload 상한 |
| `GATEWAY_RUDP_RTO_MIN_MS` | `50` | 재전송 RTO 하한(ms) |
| `GATEWAY_RUDP_RTO_MAX_MS` | `2000` | 재전송 RTO 상한(ms) |

### 2.4 HAProxy(외부 TCP LB)
HAProxy는 본 리포의 실행 파일이 아니며, 설정 파일(`haproxy.cfg`)로 Gateway 인스턴스 목록을 관리한다.
Docker 스택 검증용 설정은 `docker/stack/haproxy/haproxy.cfg` 를 참고한다. (컨테이너 내부 경로: `/usr/local/etc/haproxy/haproxy.cfg`)
운영/구체 예시는 `docs/ops/gateway-and-lb.md` 를 참고한다.

운영(Prod) 권장 TLS baseline:

| 항목 | 권장값 | 설명 |
| --- | --- | --- |
| edge 최소 TLS 버전 | TLS 1.3 | 기본 listener는 `ssl-min-ver TLSv1.3`를 강제한다. |
| 레거시 예외 listener | 분리 포트(TLS 1.2 only) | 예외 트래픽은 별도 listener로 분리하고 SNI allowlist로 제한한다. |
| 내부 링크(gateway/backend) | mTLS + `verify required` | LB->gateway/backend hop에서 클라이언트 인증서를 검증해 plaintext를 금지한다. |
| 인증서 자동 갱신 | 30/14/7일 윈도우 | 30일(일정 확정), 14일(리허설), 7일(즉시 교체) 절차를 고정한다. |

검증용 TLS 템플릿은 `docker/stack/haproxy/haproxy.tls13.cfg`를 참고한다.

### 2.5 wb_worker 전용
| 변수 | 기본값 | 설명 |
| --- | --- | --- |
| `REDIS_STREAM_KEY` | `session_events` | write-behind Stream 이름 |
| `WB_GROUP`, `WB_CONSUMER` | `wb_group`, `wb_consumer` | 스트림 소비자 그룹/ID |
| `WB_BATCH_MAX_EVENTS` | `100` | 배치당 최대 이벤트 수 |
| `WB_BATCH_MAX_BYTES` | `524288` | 배치당 최대 크기(Byte) |
| `WB_BATCH_DELAY_MS` | `500` | 배치 지연(ms) |
| `WB_DLQ_STREAM` | `session_events_dlq` | DLQ 스트림 이름 |
| `WB_DLQ_ON_ERROR` | `1` | 처리 실패 이벤트를 DLQ로 이동 |
| `WB_ACK_ON_ERROR` | `1` | 처리 실패 시에도 ACK(=재시도 대신 정리) |
| `WB_DB_RECONNECT_BASE_MS` | `500` | DB 재연결 지수 백오프 시작값(ms) |
| `WB_DB_RECONNECT_MAX_MS` | `30000` | DB 재연결 지수 백오프 상한(ms) |
| `WB_RETRY_MAX` | `5` | 재시도 횟수 |
| `WB_RETRY_BACKOFF_MS` | `250` | 재시도 백오프(ms) |
| `KNIGHTS_TRACING_ENABLED` | `0` | stream->DB 경로 tracing context 활성화 |
| `KNIGHTS_TRACING_SAMPLE_PERCENT` | `100` | tracing 샘플링 비율(0~100) |

### 2.6 운영 관점 영향도(왜 이 값이 중요한가)
아래 항목은 단순한 숫자 튜닝이 아니라, 장애 시 시스템 거동을 바꾸는 핵심 스위치다.

- `GATEWAY_BACKEND_CONNECT_TIMEOUT_MS`
  - 너무 크면 죽은 backend를 오래 기다리며 세션이 묶이고, 너무 작으면 일시 지연도 실패로 처리된다.
  - 기본값(5000ms)은 "짧은 네트워크 흔들림은 흡수"하면서도 "장애 backend에 오래 매달리지 않기" 위한 절충값이다.

- `GATEWAY_BACKEND_SEND_QUEUE_MAX_BYTES`
  - 느린 backend/네트워크에서 큐가 무한히 커지는 메모리 폭주를 막는 상한이다.
  - 상한 초과 시 세션을 끊는 이유는, 일부 세션 때문에 전체 프로세스가 OOM으로 죽는 상황을 피하기 위해서다.

- `GATEWAY_BACKEND_CIRCUIT_*`
  - backend 연결 실패가 연속으로 발생하면 circuit을 open해 신규 connect 시도를 잠시 차단한다.
  - downstream 장애 구간에서 불필요한 connect 폭주를 줄여 gateway 자원 소모를 제한한다.

- `GATEWAY_BACKEND_CONNECT_RETRY_BUDGET_PER_MIN`
  - backend 연결 재시도 예산을 분당으로 제한해 장애 시 재시도 폭주(thundering herd)를 방지한다.

- `GATEWAY_INGRESS_TOKENS_PER_SEC`, `GATEWAY_INGRESS_BURST_TOKENS`, `GATEWAY_INGRESS_MAX_ACTIVE_SESSIONS`
  - ingress 토큰 버킷 + 동시 세션 상한으로 과부하 시 신규 연결을 선제적으로 감쇠(load shedding)한다.
  - 정책 위반 연결은 즉시 종료되어 기존 활성 세션과 핵심 경로를 보호한다.

- Room 비밀번호 해시 포맷
  - 신규/갱신 비밀번호는 `sha256:<hex>` 포맷으로 저장한다.
  - 레거시 해시는 사용자가 비밀번호로 정상 입장할 때 자동으로 신규 포맷으로 마이그레이션된다.

- 채팅룸 운영/제재 명령(슬래시 커맨드)
  - 룸 소유자/관리자: `/invite <user> [room]`, `/kick <user> [room]`
  - 관리자 전용: `/mute <user> [seconds]`, `/ban <user> [seconds]`
  - 스팸 감지는 서버가 자동 처리하며, 임계치를 넘기면 자동 뮤트 후 누적 위반 시 기간제 밴으로 승격된다.

- `WB_DB_RECONNECT_BASE_MS`, `WB_DB_RECONNECT_MAX_MS`
  - DB 장애 시 모든 워커가 동시에 재접속을 시도하는 집단 폭주(thundering herd)를 줄이기 위한 백오프 경계값이다.
  - 운영에서는 DB 복구 시간대(수초~수십초)에 맞춰 max를 조정하고, 복구 지연이 길면 max를 키운다.

- `WB_RETRY_MAX`, `WB_RETRY_BACKOFF_MS`
  - flush 트랜잭션 실패 시 즉시 재시도 횟수/간격(예산)을 제한해 저장소 장애 구간에서 무한 재시도를 막는다.
  - 재시도 예산을 초과하면 해당 배치는 PEL reclaim 경로로 넘겨 후속 루프에서 재회수되므로, 워커가 한 배치에 장시간 고착되지 않는다.

- `WB_DLQ_ON_ERROR`, `WB_ACK_ON_ERROR`
  - `WB_DLQ_ON_ERROR=0` + `WB_ACK_ON_ERROR=1`이면 실패 이벤트가 유실될 수 있다.
  - 운영 기본 권장값은 DLQ 보존(`WB_DLQ_ON_ERROR=1`)이며, ACK 정책은 데이터 보존 우선순위에 맞춰 결정한다.

## 3. 멀티 인스턴스 구성 팁
1. **Gateway 다중화**  
   - 각 인스턴스에 고유한 `GATEWAY_ID`를 부여한다.  
   - HAProxy는 여러 Gateway 인스턴스로 TCP 연결을 분산한다.
   - 동일 Redis를 사용한다면 `REDIS_CHANNEL_PREFIX`를 일치시켜야 self-echo 필터가 동작한다.

3. **Server 설정**  
   - 모든 서버 인스턴스에서 DB_URI, REDIS_URI, REDIS_CHANNEL_PREFIX를 일관되게 맞춰 세션/브로드캐스트 경로가 섞이지 않도록 한다.
   - SERVER_ADVERTISE_HOST/PORT는 Gateway가 접속하는 주소이며 SERVER_REGISTRY_PREFIX도 Gateway 설정과 일치해야 heartbeat가 정상적으로 갱신된다.
   - USE_REDIS_PUBSUB=1인 경우 브로드캐스트가 Redis에 의존하므로 채널 prefix와 self-echo 정책을 점검한다.
    - METRICS_PORT는 관측 도구만 접근하도록 방화벽·보안그룹을 설정한다.
    - 외부 노출 환경이면 `METRICS_HTTP_AUTH_TOKEN` 또는 `METRICS_HTTP_ALLOWLIST`를 함께 설정해 제어면 접근을 제한한다.

4. **환경 파일 분리**  
    - 개발 단계에서는 루트 `.env` 하나를 공유해도 되지만, 운영에서는 구성 요소별 `.env.gateway`, `.env.server` 등을 만들어 필요한 값만 포함하도록 분리한다.  
   - 운영에서는 `.env.server`, `.env.gateway`처럼 역할 단위로 분리하고, 실행 환경에서 환경 변수를 주입한다.

## 4. 검증 체크리스트
- `USE_REDIS_PUBSUB` 값이 서버 인스턴스마다 동일한가?
- Instance Registry prefix(`SERVER_REGISTRY_PREFIX`)가 Gateway가 사용하는 prefix와 일치하는가?
- Gateway/Server에서 Redis 연결이 실패할 경우 로그에 WARN이 남는지 확인.
- `.env`에 민감한 정보(DB 비밀번호 등)가 포함될 경우 운영 환경에서는 별도 비밀 관리(예: Azure Key Vault, AWS Secrets Manager)를 사용한다.

## 5. 참고 문서
- 서버 구조 개요: `docs/server-architecture.md`
- Gateway & Load Balancer 운영: `docs/ops/gateway-and-lb.md`
- 로드맵 및 우선순위: `docs/roadmap.md`
- Write-behind 상세: `docs/db/write-behind.md`
