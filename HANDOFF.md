# HANDOFF: 현재 컨텍스트 요약 및 다음 단계 안내

본 문서는 현재까지 구현된 기능과 설정, 실행·검증·트러블슈팅 절차를 한눈에 복원할 수 있도록 정리합니다. 이 문서를 기준으로 다른 개발자가 바로 이어서 작업/운영을 재개할 수 있습니다.

## 현재 상태(요약)
- Write-behind 엔드투엔드 동작
  - 서버가 로그인/룸 입장/퇴장/세션 종료 시 Redis Streams로 이벤트 생산(XADD)
  - 워커가 컨슈머 그룹으로 소비 후 Postgres에 배치 커밋, 성공 시 ACK
  - 세션 식별자는 세션별 UUID v4를 생성·캐시해 `session_id`로 사용(정합성/조인 용이)
  - 비-UUID 값은 워커에서 NULL 정규화해 DB 캐스팅 오류 방지
- 관측/운영
  - 서버: 부팅 시 write-behind 설정 로그 출력, `/metrics` 노출(옵션)
  - 워커: `wb_flush`, `wb_pending` 등 최소 메트릭 로그
- 실행 스크립트
  - 원클릭: `scripts/run_all.ps1`(Windows), `scripts/run_all.sh`(WSL/Linux)
  - 수동: `server_app.exe`/`wb_worker.exe` 개별 실행 가능

## 변경 사항(최근 스프린트)
- 세션 UUID 도입 및 이벤트 반영
  - ChatService 내부에 세션별 UUID v4 생성/캐시(`session_uuid`) 추가
  - 모든 write-behind 이벤트의 `session_id`에 세션 UUID 적용
  - 세션 종료 시 UUID 정리
- 로그인 이벤트의 user_id 누락 수정
  - 로그인 처리 시 확보한 `user_uuid`를 이벤트에 반영
- 워커의 안정성 보강
  - `user_id/session_id/room_id`가 UUID 형식이 아니면 빈 문자열→DB에서 NULL 저장
- 빌드/실행 편의성 개선
  - `run_all.ps1/sh` 추가, `build.ps1` 인자 전달/실패 처리 보강
  - `wb_check`가 `.env` 자동 로드하도록 수정
- 문서/설정 갱신
  - `.env`에 write-behind/DLQ/metrics/presence/pubsub 키 추가
  - Getting Started/Deployment에 원클릭 스크립트/마이그레이션 러너/트러블슈팅 반영
- 귓속말/잠금 방 기능
  - 서버에 `/whisper` 전용 opcode 추가, 게스트/미대상 검증 및 `[whisper]` 로그 출력
  - 클라이언트는 `/whisper`·`/w` 명령과 잠금 방 표식(`🔒`)을 지원하며, 비밀번호 누락 시 즉시 안내
  - `/join <room> <password>`로 잠금 방 생성/입장, `/who`는 비회원에게 거부됨

## 소스 포인터(핵심 파일)
- 서버 이벤트 생산/세션 UUID
  - `server/include/server/chat/chat_service.hpp`
  - `server/src/chat/chat_service_core.cpp`
  - `server/src/chat/handlers_login.cpp`
  - `server/src/chat/handlers_join.cpp`
  - `server/src/chat/handlers_leave.cpp`
  - `server/src/chat/session_events.cpp`
- Redis Streams 클라이언트
  - `server/include/server/storage/redis/client.hpp`
  - `server/src/storage/redis/client.cpp`
- 워커/도구
  - `tools/wb_worker/main.cpp` — Streams 소비→DB 커밋
  - `tools/wb_emit/main.cpp` — 테스트용 XADD 발행(더미 UUID)
  - `tools/wb_check/main.cpp` — DB 반영 확인(.env 로드)
- 스크립트/설정
  - `scripts/run_all.ps1`, `scripts/run_all.sh`
  - `scripts/build.ps1` (BuildDir/UseVcpkg/Fail-fast)
  - 루트 `.env` 및 각 바이너리 폴더에 복사된 `.env`

## 실행 절차(권장)
- 원클릭(Windows): `scripts/run_all.ps1 -Config Debug -WithClient -Smoke`
  - 서버(:5000) + 워커 기동 → 선택 스모크(wb_emit→wb_check)
- 원클릭(WSL/Linux): `bash scripts/run_all.sh Debug build-linux 5000` (환경변수 `SMOKE=1`)
- 수동
  - 서버: `build-msvc/server/Debug/server_app.exe 5000`
  - 워커: `build-msvc/Debug/wb_worker.exe`
  - 클라: `build-msvc/devclient/Debug/dev_chat_cli.exe`
  - 확인(SQL): `select id,event_id,type,ts,user_id,session_id,room_id from session_events order by id desc limit 20;`

## 환경 변수(핵심)
- Write-behind 게이트/Streams
  - `WRITE_BEHIND_ENABLED=1`
  - `REDIS_STREAM_KEY=session_events`, `REDIS_STREAM_MAXLEN=10000`, `REDIS_STREAM_APPROX(기본 ~)`
- 워커 배치/그룹/DLQ
  - `WB_BATCH_MAX_EVENTS`, `WB_BATCH_MAX_BYTES`, `WB_BATCH_DELAY_MS`
  - `WB_GROUP`, `WB_CONSUMER`
  - `WB_DLQ_STREAM`, `WB_ACK_ON_ERROR`, `WB_DLQ_ON_ERROR`, `WB_GROUP_DLQ`, `WB_DEAD_STREAM`, `WB_RETRY_MAX`, `WB_RETRY_BACKOFF_MS`
- 기타
  - `PRESENCE_TTL_SEC`, `PRESENCE_CLEAN_ON_START`, `GATEWAY_ID`, `REDIS_CHANNEL_PREFIX`, `USE_REDIS_PUBSUB`, `METRICS_PORT`

## 검증 체크리스트
- 서버 로그: `Write-behind enabled: stream=...` 가 보여야 함(비활성 시 `.env` 확인)
- 워커 로그: `metric=wb_flush ... wb_ok_total>0` 증가, `wb_pending` 지표 출력
- DB 반영: 최근 레코드에 실제 `user_id/room_id` UUID, `session_id`는 세션 UUID
- 스모크(wb_emit)는 더미 UUID를 사용 — 서버 경로 검증은 실제 클라이언트 동작으로 확인할 것

## 트러블슈팅
- PowerShell 문자열 보간 오류 → `run_all.ps1` 고침(배열→해시테이블 splatting)
- CMake `-B` 인자 미전달 → `BuildDir` 로그 및 인자 전달 보강
- LNK2019(load_dotenv) → `wb_check`가 `server_core`에 링크되도록 CMake 보강
- CMake 캐시 경로 불일치(WSL/Windows 혼용) → `scripts/build.ps1 -Clean` 또는 빌드 디렉터리 정리 후 재구성
- Redis Cloud 연결 이슈 → `.env`의 `REDIS_URI`(인증 포함) 및 방화벽/SSL 스킴 확인

## 운영 메모
- 현재 배치 커밋은 `WB_BATCH_DELAY_MS`(기본 500ms)와 임계치에 의해 거의 실시간에 가깝게 반영됨
  - 더 “주기형”으로 만들려면 지연/임계값 상향(또는 추가 파라미터 도입)으로 조정 가능
- 장애 시 즉시 `WRITE_BEHIND_ENABLED=0`으로 폴백(동기 경로), 재가동 시 멱등 반영으로 복구

## 다음 단계
- (옵션) 주기형 배치 파라미터 노출(`WB_BLOCK_MS`, `WB_MIN_BATCH_EVENTS`) 및 관측 지표 확장
- Compose/K8s 배포 자산 보강(Dockerfile 멀티스테이지/Helm 차트)
- 대시보드/알람(
  - 워커: `wb_flush`, `wb_fail_total`, `wb_pending`, DLQ 길이
  - 서버: `/metrics` 지표 및 subscribe lag)

## 핵심 결정(요약)
- SoR(System of Record): PostgreSQL 16, 드라이버는 `libpqxx`(C++17). ORM 미도입, Prepared Statement 사용.
- 캐시/실시간 가속: Redis 사용(세션/프레즌스/최근 메시지/팬아웃). 장애 시 Postgres 폴백 가능.
- 보안: 비밀번호 해시 Argon2id. 세션 토큰은 해시만 저장(`token_hash`).
- 식별자: UUID 기반(`user_id`, `room_id`, `session_id`). 이름은 라벨로만 사용(룸 이름 중복 허용).
- 스냅샷: 방 입장 시 최근 N(기본 20) 메시지 스냅샷 전송 후 입력 활성화. 단일 메시지 포맷(MSG_STATE_SNAPSHOT) 채택.
- Write-behind: 세션 이벤트를 Redis Streams에 버퍼링 후 배치 커밋(at-least-once + 멱등)으로 RDB 부하 완화(현재 활성화 가능).

## 문서 지형도(중요 파일)
- 아키텍처/결정
  - `docs/db/decision-record.md` — DB/보안 선택 ADR(한글)
  - `docs/db/architecture.md` — 계층/SPI/어댑터, Redis 연동, Write-behind, 폴백/복구
  - `docs/identity.md` — UUID 식별 전략, 이름은 라벨
- 데이터/마이그레이션
  - `docs/db/schema.md` — 논리 모델 및 DDL 초안(rooms.name 중복 허용, sessions.client_ip 등)
  - `docs/db/migrations.md` — 전략/러너 가이드/주요 스크립트 구성
  - `docs/db/migrations/0001_init.sql.md` — 코어 테이블 생성
  - `docs/db/migrations/0002_indexes.sql.md` — 인덱스/pg_trgm, CONCURRENTLY 주의
  - `docs/db/migrations/0003_identity.sql.md` — 이름 고유 제약 제거/컬럼 추가
  - `docs/db/migrations/0004_session_events.sql.md` — write-behind 이벤트 테이블
- Redis 전략/운영
  - `docs/db/redis-strategy.md` — 캐시/세션/프레즌스/팬아웃 설계
  - `docs/db/cache-keys.md` — 키 네임스페이스/TTL/메모리 예산
  - `docs/ops/fallback-and-alerts.md` — 장애 폴백/알람/런북
  - `docs/ops/prewarm.md` — 캐시 프리워밍 절차
- 프로토콜/흐름/클라이언트 UX
  - `docs/protocol/snapshot.md` — 스냅샷 최종안과 바이너리 포맷
  - `docs/chat/recent-history.md` — 입장 시 최근 20건 로딩/워터마크/중복 처리/UI 게이팅
  - `docs/chat/sequence-join.md` — Join → Snapshot → Fanout 시퀀스(mermaid 포함)
- 체크리스트
  - `docs/db/checklist.md` — 구현 전 확인용(설정 키/문서 상태/Go-NoGo)

## 설정 키(현 시점 기준)
- Postgres: `DB_URI`, `DB_POOL_MIN`, `DB_POOL_MAX`, `DB_CONN_TIMEOUT_MS`, `DB_QUERY_TIMEOUT_MS`, `DB_HEALTHCHECK_INTERVAL_SEC`, `DB_PREPARE_STATEMENTS`
- Redis: `REDIS_URI`, `REDIS_POOL_MAX`, `REDIS_CHANNEL_PREFIX`, `REDIS_USE_STREAMS`, `REDIS_STREAM_MAXLEN`
- 캐시/스냅샷: `CACHE_TTL_SESSION`, `CACHE_TTL_RECENT_MSGS`, `CACHE_TTL_MEMBERS`, `RECENT_HISTORY_LIMIT`(20), `ROOM_RECENT_MAXLEN`(~200)
- Write-behind: `WRITE_BEHIND_ENABLED`, `WB_BATCH_MAX_EVENTS`, `WB_BATCH_MAX_BYTES`, `WB_BATCH_MAX_DELAY_MS`, `WB_WORKER_CONCURRENCY`, `WB_DLOUT_STREAM`
- 폴백 플래그: `REDIS_REQUIRED`, `USE_REDIS_CACHE`, `USE_REDIS_PUBSUB`
- 프리워밍: `PREWARM_ENABLED`, `PREWARM_TOP_ROOMS`, `PREWARM_CONCURRENCY`

## 프로토콜 스냅샷(요약)
- `MSG_STATE_SNAPSHOT` payload(순서 고정):
  1) `room_id`(UUID 16B)
  2) `wm`(u64 워터마크)
  3) `count`(u16)
  4) `messages[]` × count: `id(u64)`, `user_id(UUID)`, `created_at_ms(i64)`, `content(lp_utf8)`
- 정렬/중복: 오름차순 전송, `id <= wm` 드랍. 스냅샷 완료 전 UI 비활성화.

## 데이터 모델 핵심 포인트
- rooms: `name` 중복 허용, `is_active/closed_at`로 라이프사이클 표현, 이름 검색 인덱스는 0002에서 별도 생성
- sessions: `client_ip inet`, `user_agent text` 추가(보안/운영 분석용)
- messages: `id bigserial`, 룸당 페이징 인덱스(room_id, id)

## Redis 캐시/팬아웃
- 키: `room:{room_id}:recent`(LIST/ZSET of id), `msg:{id}`(JSON), `presence:*`, `session:{token_hash}` 등 — 반드시 UUID 중심 설계
- 팬아웃: 초기 Pub/Sub, 필요 시 Streams(내구/재처리)
- 장애 시 폴백: 캐시는 Postgres로, 팬아웃은 인프로세스 축소(플래그 제어)

## Write-behind(세션 이벤트)
- Redis Streams `session_events`에 적재 → 워커가 배치 커밋(at-least-once)
- 멱등: `event_id` 고유 + 고유 제약으로 중복 삽입 방지
- 모니터링: 배치 지연/크기/성공률, 펜딩 길이, DLQ

## 남은 작업(구현 승인 후)
1) core SPI 추가: `server::core::storage`에 `IConnectionPool`, `IUnitOfWork`, `I*Repository`
2) Postgres 어댑터: `server/storage/postgres` 구현(풀/유닛오브워크/리포지토리)
3) 서버 주입: `server/app/bootstrap`에서 풀 생성/DI, 라우터/서비스에 주입
4) Redis 클라이언트/풀: 캐시/세션/프레즌스/팬아웃 적용, 설정 키 연결
5) 스냅샷 핸들러: 조인 시 Redis→Postgres 폴백 로직, 워터마크/정렬/멱등 처리
6) Write-behind 워커(옵션): Streams 컨슈머/배치 커밋/멱등 삽입
7) 마이그레이션 러너: `tools/migrations/` 적용(0002는 non-transactional 예외)
8) 프리워밍 잡(옵션): 복구 시 상위 룸 캐시 재가열
9) 문서/CI: 설정 키 검증, ‘이름 기반 식별 금지’ 린트, ‘knights’ 문자열 금지 규칙 등

## 유의사항/제약
- “구현 보류” 원칙: 사용자가 명시적으로 “구현 시작”을 승인하기 전에는 코드 변경 금지(문서만 업데이트)
- 문서 언어: 한국어로 통일(기술 용어는 원문 병기 가능)
- 식별자: 이름은 라벨. 프로토콜/키/조인/로그 레퍼런스는 모두 UUID 사용
- 인덱스 생성: 운영 환경에선 `CREATE INDEX CONCURRENTLY` 사용, 러너 예외 처리 필요

## 재개 절차(빠른 스타트)
1) `docs/db/checklist.md`에서 Go/No-Go 항목 체크 후 “구현 단계 승인”
2) 마이그레이션 러너 요구사항 문서 보강(필요 시) → 0001~0004 순서 적용
3) core SPI → PG 어댑터 → 주입 → Redis 캐시 → 스냅샷 핸들러 →(옵션) write-behind 순으로 커밋 단위 구분해 진행
4) 사용자가 빌드/실행 후 `info.txt`로 빌드 로그/스모크 결과 공유(문서 상 가정)

## 참고 네임스페이스/타깃(코드 스타일)
- C++ 네임스페이스: `server::core::{net,concurrent,memory,config,state,protocol,storage}`, `server::app::chat`, `server::storage::postgres`
- CMake 타깃: `server_core`, `server_app`, `server_storage_pg`, `dev_chat_cli`, `wire_proto`

이 문서만으로도 다음 담당자가 현재 설계 상태를 복원하고, 승인 후 구현 순서를 이어갈 수 있도록 작성했습니다. 추가 설명이 필요하면 `docs/` 내 세부 문서를 우선 참조하세요.
## 이번 세션 변경 사항(요약)
- 마이그레이션 러너 추가: `tools/migrations/runner.cpp`
  - 실행: `DB_URI` 환경변수 또는 `--db-uri` 인자 사용, `--dry-run` 지원
  - 적용 순서: `tools/migrations/*.sql`의 `000N_*.sql` 오름차순, `schema_migrations`로 상태 추적
  - `CREATE INDEX CONCURRENTLY` 포함 시 non-transactional로 실행
- Redis 프레즌스 최소 구현 및 재시작 복원
  - 입장 시 SADD: `prefix + presence:room:{room_id}`에 `user_id` 추가
  - 퇴장/세션종료 시 SREM: 동일 SET에서 제거
  - 재시작 시 선택적 정리: `PRESENCE_CLEAN_ON_START != 0`이면 `SCAN`+`DEL`로 `prefix + presence:room:*` 제거
  - 접두사: `REDIS_CHANNEL_PREFIX`가 설정되면 모든 프레즌스 키/패턴에 접두사 적용
  - 변경 파일: `server/src/chat/handlers_join.cpp`, `server/src/chat/handlers_leave.cpp`, `server/src/chat/session_events.cpp`, `server/src/app/bootstrap.cpp`, `server/src/storage/redis/client.cpp`, `server/include/server/storage/redis/client.hpp`
- Sender 표기 일관화 및 브로드캐스트 정리
  - 시스템 메시지 sender를 "(system)"으로 통일
  - 퇴장/세션종료 공지: Protobuf `ChatBroadcast` 사용으로 통일
  - 변경 파일: `server/src/chat/handlers_leave.cpp`, `server/src/chat/session_events.cpp`, `server/src/chat/chat_service_core.cpp`
- Presence/분산 브로드캐스트/워커(1~3 단계)
  - Presence(user) TTL: 로그인/채팅 시 `SETEX prefix + presence:user:{user_id}`로 TTL 갱신(`PRESENCE_TTL_SEC`, 기본 30초)
  - 분산 브로드캐스트: `USE_REDIS_PUBSUB!=0`일 때 `prefix + fanout:room:{room_name}`로 Protobuf 바이트 publish(수신 self-echo 필터는 후속 보강 예정)
  - Write-behind 워커 스켈레톤: `tools/wb_worker/main.cpp` 추가(CMake 타겟 `wb_worker`). Redis 헬스체크 후 루프 대기(Streams 연결은 후속 작업)
  - 변경 파일: `server/src/chat/handlers_login.cpp`, `server/src/chat/handlers_chat.cpp`, `server/include/server/storage/redis/client.hpp`, `server/src/storage/redis/client.cpp`, `tools/wb_worker/main.cpp`, `CMakeLists.txt`

## 진행 현황 업데이트
- 빌드 상태: server_app, storage_pg/redis, migrations_runner, wb_worker 모두 빌드 성공
- 문서 갱신: ROADMAP(마일스톤/DoD/진행 요약), PROTOCOL(인증/브로드캐스트 규칙), REDIS 전략(채널 명세), OBSERVABILITY(지표/파이프라인)
- 향후 작업: Pub/Sub 구독 + echo 필터(envelope/gateway_id), Streams 기반 write-behind 구현, 스냅샷 정합성 보강
 - 분산 브로드캐스트: USE_REDIS_PUBSUB!=0일 때 `fanout:room:*` 패턴 구독 시작. Envelope `gw={gateway_id}\n` + Protobuf 바이트. 로컬 gw와 동일하면 드롭 후, 룸을 채널명에서 파싱하여 재브로드캐스트.
 - Postgres 어댑터: libpqxx 필수 의존으로 강제(CMake), 코드에서 HAVE_LIBPQXX 분기 제거하고 pqxx 단일 경로로 정리
- Postgres 빌드 오류 수정(현재 info.txt 기준)
  - `PgMembershipRepository` 누락으로 인한 컴파일 오류(C2079, C2439) 해결: 구현 추가 확인
  - 클래스/함수 재정의(C2011, C2084) 정리: 중복 정의 제거 상태 확인
  - 파일: `server/src/storage/postgres/connection_pool.cpp`

## 다음 단계 로드맵
- 상세 계획과 진행도, DoD, 리스크/의존성은 `docs/roadmap.md`를 참조.
- DB 우선 과제(마일스톤 1~3)가 완료되면, 분산 브로드캐스트 수신/echo 필터 → Streams 확장 순으로 본격적인 분산 서버 구현 단계로 진입.
 - 관측성 계획은 `docs/ops/observability.md`를 참조(구조적 로깅, Prometheus/Grafana, OpenTelemetry).

## 운영/설정 키(추가/변경)
- Redis 정리 플래그: `PRESENCE_CLEAN_ON_START` — 0이 아니면 부팅 시 프레즌스 키 일괄 정리
- 프레즌스 키 접두사: `REDIS_CHANNEL_PREFIX` — 예: `chat:dev:`

## 주의사항
- 다중 게이트웨이 환경에서는 부팅 시 프레즌스 정리 기능을 사용하지 말 것. 개발 또는 단일 인스턴스에서만 사용 권장.
작업 TODO: 모든 docs/*.md 문장의 관련 구현 경로를 (파일:라인) 형식으로 명시할 것.
