# Knights Roadmap — DB 우선 과제 이후 분산 서버 단계

본 문서는 향후 구현 계획을 진행도와 우선순위에 따라 정리한 로드맵입니다. 현재 상태(진행도 표기)와 구체적 완료 기준(DoD), 위험/의존성을 함께 기록합니다.

## 진행도 표기
- [done]: 완료
- [wip]: 진행 중
- [ready]: 설계/환경 준비 완료, 바로 착수 가능
- [todo]: 계획됨(착수 전)

## 마일스톤 개요
1) DB 기초·마이그레이션 안정화 — [wip]
2) Presence 고도화(유저 TTL + 룸 Set 정합) — [wip]
3) Write-behind(세션/프레즌스/경량 이벤트) — [wip]
4) 분산 브로드캐스트(Pub/Sub → Streams 확장) — [ready]
5) 스냅샷 정합성·성능 보강 — [todo]
6) 테스트/운영/관측성(Observability) — [todo]

---

## 1) DB 기초·마이그레이션 안정화 — [wip]
- [done] SQL 마이그레이션 구성: `tools/migrations/0001..0004`
- [done] 마이그레이션 러너: `migrations_runner` (dry-run, non-tx 인식)
- [done] Postgres 어댑터 단일화: `libpqxx` 필수 의존, 전처리 분기 제거(HAVE_LIBPQXX 삭제)
- [done] CMake 강제화: `server`/`migrations_runner`에서 libpqxx 미존재 시 `FATAL_ERROR`
- [wip] 리포지토리 스켈레톤 → 기능 보강/테스트
  - [todo] Users/Rooms/Messages/Memberships/Session 경계별 단위테스트(기본 happy-path)
  - [todo] 인덱스/성능: 0002 인덱스 점검, lock_timeout/statement_timeout 설정 가이드
  - [todo] 러너 기능 보강: 단일 버전 실행, 실패 정책(중단/계속), 로그 레벨, 타임아웃 파라미터

완료 기준(DoD)
- 러너가 dry-run/실적용 모두 정상 동작하고, 실패/재시도에 대해 일관된 로그 출력
- 기본 CRUD 경로 리포지토리 테스트 통과(GHA/로컬)

리스크/의존성
- libpqxx 제공 타깃(플랫폼 별)이 상이할 수 있음 → CMake 가드 유지

---

## 2) Presence 고도화 — [wip]
- [done] 룸 프레즌스: `prefix + presence:room:{room_id}` SADD/SREM
- [done] 유저 프레즌스: `prefix + presence:user:{user_id}` SETEX TTL(기본 30s)
- [done] 재시작 최소복원: `PRESENCE_CLEAN_ON_START` 시 `presence:room:*` 정리(개발/단일 노드)
- [todo] heartbeat 전용 경량 경로(/ping 등)에서 user TTL 갱신
- [todo] TTL 만료/로그아웃 시 memberships 정합(선택) — write-behind와 연계

완료 기준(DoD)
- 로그인/채팅/하트비트 시 user TTL 갱신 확인, 퇴장/세션종료 시 room SREM 확인
- 단일 노드 재시작 후 비정합 최소화(옵션 정리)

리스크/의존성
- 다중 게이트웨이에서 PRESENCE_CLEAN_ON_START는 금지(문서/가드)

---

## 3) Write-behind(경량 이벤트) — [wip]
### 구현 전 준비
- [done] Redis Streams 클라이언트 구현(XGROUP/XADD/XREADGROUP/XACK) — `server/src/storage/redis/client.cpp`
- [done] 워커 스켈레톤 + .env 인식 — `tools/wb_worker/main.cpp`
- [done] 키/옵션/운영 문서 정리 — `docs/db/write-behind.md`
- [done] 이벤트 필드/트림 정책 최종 확정(`session_login`, `room_join`, `room_leave`, `session_close`) 및 구성 키 기본값 동기화

### 구현
- [done] 서버 Ingest 경로: `WRITE_BEHIND_ENABLED` 활성 시 XADD 생산(handlers_login/handlers_join/handlers_leave/session_events) + MAXLEN 적용
- [todo] 워커 배치 커밋: `WB_BATCH_MAX_EVENTS/BYTES/DELAY_MS` 반영, Postgres 트랜잭션, 멱등 처리, ACK 플로우 확정
- [todo] DLQ/재시도 경로: `WB_DLOUT_STREAM`, 재시도 백오프, 치명 오류 분기 구현

### 구현 후/운영
- [todo] 관측성 지표 및 알람: `wb_batch_size`, `wb_commit_ms`, `wb_fail_total`, `wb_pending`, `wb_dlq_total` 수집·대시보드화
- [todo] 검증: 로컬 Redis 통합 테스트(XREADGROUP 블로킹/배치·ACK), 펜딩/재시도 시나리오, 이벤트 파싱 단위 테스트
- [todo] 운영 가이드: 핸드오프 절차, 장애 폴백 전략, 배포 체크리스트 문서화

완료 기준(DoD)
- 이벤트를 Streams로 적재하고 워커가 배치 커밋하여 DB에 반영(샘플: session_login/room_join/leave)

리스크/의존성
- Redis 장애/지연 시 처리 지연 가능 → 백오프/알람 필요

---

## 4) 분산 브로드캐스트 — [wip]
### 구현 전 준비
- [done] 발행(publish): `USE_REDIS_PUBSUB!=0`일 때 `prefix + fanout:room:{room_name}`로 Protobuf 바이트 publish
- [done] self-echo 방지: Envelope에 `gw=<gateway_id>\n<payload>` 적용, 로컬 `GATEWAY_ID`와 일치하면 드롭
- [todo] 구독 루프 설계: reconnect/backoff 정책, shutdown 처리, 멀티 스레드 안전성 정리

### 구현
- [todo] 구독(subscribe) 태스크: Redis Subscriber 연결, payload 파싱, 세션 fanout 연동
- [todo] 장애 복구 흐름: 재연결(backoff), 누락 감지, 경고 로그/알람 수준 정의
- [todo] 일관성 검증: 멀티 게이트웨이 환경 통합 테스트 스크립트 및 시뮬레이션 작성

### 구현 후/운영
- [todo] 관측성: `publish_total`, `subscribe_total`, `subscribe_lag_ms`, `self_echo_drop_total` 지표 수집 및 경보 설정
- [todo] 운영 가이드: GATEWAY_ID 배포 체크리스트, Pub/Sub 장애 시 폴백 전략 정리

완료 기준(DoD)
- 멀티 인스턴스 간 동일 룸 메시지가 일관되게 팬아웃, self-echo로 인한 중복 수신 없음

리스크/의존성
- Pub/Sub은 내구성 없음 → 필요 시 Streams 전환 고려(보강 계획 포함)

---

## 5) 스냅샷 정합성·성능 보강 — [todo]
- [todo] last_seen 갱신 타이밍/멱등/워터마크 재검토
- [todo] 최근 메시지 캐시 포맷(JSON 이스케이프/크기) → Protobuf/MsgPack 검토
- [todo] 쿼리/인덱스 점검 및 LIMIT/정렬 일관성

완료 기준(DoD)
- 재접속/갱신 시 스냅샷 정합성 확보, UI 멱등 반영 확인

---

## 6) 테스트/운영/관측성 — [todo]
- [todo] 최소 단위/통합 테스트 추가(리포지토리, 러너 dry-run, presence 경로)
- [todo] 빌드 매트릭스(MSVC/GCC/Clang), 샘플 .env 기반 CI 단계
- [todo] 로깅 강화: 구조적 로그(JSON), 레벨/필드 표준화(trace_id, user_id, session_id, room), 회전/보존 정책
- [todo] 모니터링: Prometheus 지표 수집 + Grafana 대시보드(또는 동등 솔루션), 핵심 지표(fanout 지연, 에러율, presence TTL 히트율, DB/Redis 지연)
- [todo] 트레이싱: OpenTelemetry 계측(선택), 분산 트레이스 상관관계 설정
- [todo] 파이프라인 계측: accept→write_frame 단계별 `pipeline_latency_ms{stage}` 히스토그램 구현
- [todo] 분산 브로드캐스트 지표: publish/subscribe 카운트, subscribe lag, duplicate drop 수집
- [todo] 대시보드: 서버/룸 패널 템플릿 구성(처리량, 지연, 오류, 리소스)

---

## 우선순위 제안 (개발자 메모)
1) Pub/Sub 구독 태스크 구현 — 브리지 핵심 기능 미완성 상태 해소, 멀티 게이트웨이 실동작 확보.
2) Pub/Sub 장애 복구/백오프 — Redis 단절 시 메시지 손실 방지를 위한 필수 내구성 확보.
3) 멀티 게이트웨이 통합 테스트 — 구현 완료 즉시 회귀 지표 마련, 이후 변경 안정성 확보.
4) Write-behind 서버 Ingest — Streams 생산이 있어야 워커/커밋 단계 착수 가능, 플로우 기점.
5) 워커 배치 커밋/멱등 처리 — DB 반영이 없으면 write-behind 가치가 없으므로 인접 단계에서 이어가기.
6) DLQ/재시도 경로 — 실패 시 메시지 유실·중단을 막는 안정성 핵심, 커밋 구현 직후 진행.
7) 두 기능군의 관측성 지표 — 운영 감시/튜닝을 위한 최소 계측, 기능 구현 완료 직후 시행.
8) 운영 가이드/런북 — 상위 기능과 계측이 갖춰진 후 배포·장애 대응 절차 문서화.

## 진행 현황(요약)
- DB 마이그레이션 러너 구현 및 작동 확인(dry-run/적용)
- Redis Presence: 룸 SET(SADD/SREM), 유저 TTL(SETEX), 재시작 최소 복원 옵션
- 분산 브로드캐스트(1차): Pub/Sub 발행 옵션(채널 `fanout:room:{room_name}`) + Envelope(`gw=<gateway_id>\n<payload>`) self-echo 필터 적용
- Write-behind 워커: 스켈레톤 추가 및 빌드 통합
- 문서: ROADMAP, HANDOFF, PROTOCOL, REDIS 전략, OBSERVABILITY 보강

---

## 프로세스/DoD(관측성 포함)
- 기능 설계 시 관측 항목(로그/메트릭/트레이스) 함께 정의
- 구현과 동시에 계측 추가(최소 세트: 처리량/지연/오류/동시성)
- PR 체크리스트에 다음 항목 포함
  - [ ] 구조적 로그 추가(필수 필드 포함: trace_id, user_id, session_id, room, server_id)
  - [ ] 메트릭 추가(Counter/Gauge/Histogram) 및 라벨 표준 적용
  - [ ] 대시보드 패널 추가/갱신(PR에 JSON 또는 문서 링크)
  - [ ] 알림 임계치/런북(요약) 갱신
  - [ ] PII/민감정보 로그 금지 확인(마스킹/익명화)

---

## 환경변수/설정(요약)
- Presence: `PRESENCE_TTL_SEC`(기본 30), `PRESENCE_CLEAN_ON_START`
- Redis 키/채널 접두사: `REDIS_CHANNEL_PREFIX`
- 분산 브로드캐스트: `USE_REDIS_PUBSUB`, `GATEWAY_ID`
- Write-behind: `WRITE_BEHIND_ENABLED`, `REDIS_STREAM_KEY`, `REDIS_STREAM_MAXLEN`, `WB_*`
- 마이그레이션 러너: `DB_URI`

---

## 참고 문서
- `docs/db/migrations.md` — 러너 사용법/DDL
- `docs/db/redis-strategy.md` — presence/publish/키 설계
- `docs/protocol.md` — 브로드캐스트 포맷 규칙

## 7) Auth Service — [ready]
- 작업: Register/Login/Refresh/Logout, Argon2id 해시, 로그인 시도/락아웃, 리포지토리/마이그레이션
- DoD: ID/PW 로그인·세션 발급·만료·로그아웃 정상 동작, 기본 레이트 리밋
- 리스크: 해시 파라미터/마이그레이션 비용, 보안 설정 누락

## 8) Gateway 고도화 — [ready]
- 작업: TLS 종료, 헬스체크/드레인, heartbeat 경량 경로로 user TTL 갱신, Pub/Sub 구독 브릿지(envelope/gateway_id)
- DoD: 롤링 배포 중 연결 드레인 무중단, 멀티 인스턴스에서 브로드캐스트 수신·재전파
- 리스크: GATEWAY_ID 오배포(중복 필터 무력화), 재연결 백오프

## 9) 분산 운영 가드 — [todo]
- 작업: envelope(origin,gateway_id,room,ts), self‑echo 필터, backoff, 장애주입
- DoD: 중복 재전파 0, 네트워크 플랩 내성

## 10) DB 파티셔닝/샤딩 PoC — [todo]
- 작업: messages/memberships 파티션(room_id), Citus PoC, 크로스 샤드 최소화
- DoD: 파티션 플랜 검증, 주요 쿼리 성능/정합 통과


