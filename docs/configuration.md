# Configuration Guide

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
| `REDIS_CHANNEL_PREFIX` | Pub/Sub 채널 Prefix (`knights:` 등) |
| `GATEWAY_ID` | Gateway/Server 식별자 (Presence, Pub/Sub self-echo에 사용) |

### 2.2 server_app 설정
| 항목 | 기본값 | 설명 |
| --- | --- | --- |
| `PORT` | `5000` | 서버 리스닝 포트(또는 실행 인자 argv[1]) |
| `SERVER_ADVERTISE_HOST` | `127.0.0.1` | Gateway가 backend를 찾을 때 사용하는 광고 호스트(예: Pod IP, 노드 IP, VIP) |
| `SERVER_ADVERTISE_PORT` | listen 포트 | Gateway가 접속할 포트(포트 매핑 시 외부 포트 지정) |
| `SERVER_INSTANCE_ID` | `server-<timestamp>` | Instance Registry에 등록되는 고유 서버 ID |
| `SERVER_REGISTRY_PREFIX` | `gateway/instances/` | Redis Instance Registry prefix. server_app과 gateway_app이 동일한 prefix를 사용해야 한다. |
| `SERVER_REGISTRY_TTL` | `30` | Instance Registry TTL(초) |
| `SERVER_HEARTBEAT_INTERVAL` | `5` | Instance Registry heartbeat 주기(초) |
| `METRICS_PORT` | `0` | HTTP metrics 포트(관측/대시보드 용도). 0이면 비활성 |
| `WB_*` | - | write-behind batch, delay, DLQ 등 세부 옵션 |

### 2.3 gateway_app 전용
| 변수 | 기본값 | 설명 |
| --- | --- | --- |
| `GATEWAY_LISTEN` | `0.0.0.0:6000` | 게이트웨이 TCP 리스너 |
| `GATEWAY_ID` | `gateway-default` | 인스턴스 식별자(로그/Presence 등) |
| `REDIS_URI` | `tcp://127.0.0.1:6379` | Instance Registry/SessionDirectory Redis |
| `METRICS_PORT` | `6001` | `/metrics` 포트 |

### 2.4 HAProxy(외부 TCP LB)
HAProxy는 본 리포의 실행 파일이 아니며, 설정 파일(`haproxy.cfg`)로 Gateway 인스턴스 목록을 관리한다.
Docker 스택 검증용 설정은 `docker/stack/haproxy/haproxy.cfg` 를 참고한다. (컨테이너 내부 경로: `/usr/local/etc/haproxy/haproxy.cfg`)
운영/구체 예시는 `docs/ops/gateway-and-lb.md` 를 참고한다.

### 2.5 wb_worker 전용
| 변수 | 기본값 | 설명 |
| --- | --- | --- |
| `REDIS_STREAM_KEY` | `session_events` | write-behind Stream 이름 |
| `WB_GROUP`, `WB_CONSUMER` | `wb_group`, `wb_consumer` | Stream 소비자 그룹/ID |
| `WB_BATCH_MAX_EVENTS` | `100` | 배치당 최대 이벤트 수 |
| `WB_BATCH_MAX_BYTES` | `524288` | 배치당 최대 크기(Byte) |
| `WB_BATCH_DELAY_MS` | `500` | 배치 지연(ms) |
| `WB_DLQ_STREAM` | `session_events_dlq` | DLQ 스트림 이름 |
| `WB_RETRY_MAX` | `5` | 재시도 횟수 |
| `WB_RETRY_BACKOFF_MS` | `250` | 재시도 백오프(ms) |

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
