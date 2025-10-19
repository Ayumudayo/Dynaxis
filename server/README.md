# server_app (Chat Server)

`server/` 모듈은 Knights의 핵심 채팅 서버 실행 파일(`server_app`)을 제공합니다. 이 프로세스는 `server_core` 런타임을 기반으로 TCP 세션을 수용하고, Redis/ PostgreSQL 백엔드, TaskScheduler, DbWorkerPool을 통해 상태를 관리합니다. 장기적으로는 범용 서버 엔진으로 확장될 수 있도록 모듈형 라우터와 인스턴스 레지스트리를 유지합니다.

## 디렉터리 구조
- `include/server/app/` — 부팅, 라우팅, 환경 옵션 인터페이스
- `include/server/chat/` — 라우터 핸들러, 세션 이벤트, 서비스 핵심 로직
- `include/server/state/` — 인스턴스 레지스트리 및 서버 상태 인터페이스
- `include/server/storage/` — PostgreSQL/Redis 클라이언트 어댑터 헤더
- `src/app/` — `bootstrap.cpp`를 포함한 엔트리포인트 및 부트스트랩 로직
- `src/chat/` — `/login`, `/join`, `/leave`, `/chat` 등 명령 핸들러 구현
- `src/state/` — 샤딩/멀티 인스턴스 대비 상태 관리 코드
- `src/storage/` — 데이터베이스/Redis 어댑터 구현
```text
server/
├─ include/
│  └─ server/
│     ├─ app/
│     ├─ chat/
│     ├─ state/
│     └─ storage/
├─ src/
│  ├─ app/
│  ├─ chat/
│  ├─ state/
│  └─ storage/
└─ tests/ (planned)
```


## 주요 환경 변수
| 변수 | 설명 | 기본값 |
| --- | --- | --- |
| `DB_URI` | PostgreSQL 접속 URI | 없음 (필수) |
| `DB_POOL_MIN` / `DB_POOL_MAX` | DB 커넥션 풀 최소/최대 크기 | `1` / `8` |
| `REDIS_URI` | Redis 접속 URI | 없음 (옵션) |
| `REDIS_POOL_MAX` | Redis 커넥션 풀 크기 | `16` |
| `WRITE_BEHIND_ENABLED` | Redis Stream 기반 write-behind 활성화 여부 | `1` |
| `REDIS_STREAM_KEY` 및 `WB_*` | write-behind 스트림 설정 | 자세한 값은 `.env` 참고 |
| `PRESENCE_TTL_SEC` | Presence TTL(초) | `30` |
| `METRICS_PORT` | HTTP `/metrics` 익스포터 포트 | `9090` |

환경 변수는 실행 파일과 동일한 디렉터리 또는 리포지터리 루트의 `.env`에서 자동 로드됩니다. `.env.default`가 존재하면 최초 실행 시 `.env`로 복사됩니다.

### Write-behind 파이프라인
- `WRITE_BEHIND_ENABLED=1`과 `REDIS_URI`가 설정되면 채팅 세션 이벤트가 Redis Streams에 적재됩니다.
- 발행 이벤트: `session_login`, `room_join`, `room_leave`, `session_close` 등은 `emit_write_behind_event`를 통해 기록됩니다.
- 주요 설정 키:
  - `REDIS_STREAM_KEY` — 기본 스트림 이름
  - `WB_GROUP`, `WB_CONSUMER` — `wb_worker`가 사용하는 컨슈머 그룹/ID
  - `WB_BATCH_MAX_EVENTS`, `WB_BATCH_MAX_BYTES`, `WB_BATCH_DELAY_MS` — 스트림 배치 크기 및 지연
  - `WB_DLQ_STREAM`, `WB_GROUP_DLQ`, `WB_DEAD_STREAM` — DLQ 스트림 및 보조 키
- Streams 소비는 `tools/wb_worker` 프로세스가 담당하며, PostgreSQL 등 영속 저장소로 커밋하고 실패 시 DLQ로 이동합니다.
- 운영 가이드는 `docs/db/write-behind.md`, `docs/ops/runbook.md`, `scripts/smoke_wb.ps1`을 참고하세요.

## 빌드 & 실행
```powershell
# 루트에서 CMake configure 후
cmake --build build-msvc --target server_app

# (선택) 디버그 실행
.\build-msvc\server\Debug\server_app.exe
```
Redis/DB 백엔드를 실제로 사용하려면 `.env`에 올바른 URI를 채워두고, 필요 시 `scripts/build.ps1 -Run server` 같은 자동화 스크립트를 활용하세요.

## 런타임 구성 요소
- **라우터**: gRPC Load Balancer에서 전달된 세션을 authoritative하게 처리하며, 룸 스냅샷과 `ROOM_MISMATCH` 검증을 제공합니다.
- **TaskScheduler**: 주기적 메트릭, health check, presence clean-up을 담당합니다.
- **DbWorkerPool**: 백그라운드로 PostgreSQL/Redis 작업을 수행하고 큐 메트릭을 노출합니다.
- **Metrics 서버**: `/metrics` 엔드포인트에서 Prometheus 호환 지표를 제공합니다.
- **Write-behind**: Redis Streams에 적재된 이벤트를 `wb_worker`가 읽어 DB에 반영하며, 서버와 동일한 `.env` 구성을 사용합니다.

## 다음 단계 참고
- 엔진화 로드맵 및 TODO: `docs/server-architecture.md`, `docs/core-design.md`
- Gateway/LoadBalancer와의 gRPC 플로우: `proto/gateway_lb.proto`, `gateway/README.md`, `load_balancer/README.md`
- Write-behind 워커 및 도구: `tools/wb_worker`, `tools/wb_dlq_replayer`, `tools/wb_emit`, `tools/wb_check`
