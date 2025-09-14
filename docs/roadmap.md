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
3) Write-behind(세션/프레즌스/경량 이벤트) — [ready]
4) 분산 브로드캐스트(Pub/Sub → Streams 확장) — [ready]
5) 스냅샷 정합성·성능 보강 — [todo]
6) 테스트/운영/관측성(Observability) — [todo]

---

## 1) DB 기초·마이그레이션 안정화 — [wip]
- [done] SQL 마이그레이션 구성: `tools/migrations/0001..0004`
- [done] 마이그레이션 러너: `migrations_runner` (dry-run, non-tx 인식)
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

## 3) Write-behind(경량 이벤트) — [ready]
- [done] 워커 스켈레톤: `tools/wb_worker/main.cpp` (환경/루프)
- [todo] Streams 키/옵션 정리: `REDIS_STREAM_KEY=session_events`, `REDIS_STREAM_MAXLEN`
- [todo] Ingest: XADD(서버) → Consumer Group(XREADGROUP) 워커 처리
- [todo] 배치 커밋: `WB_BATCH_MAX_EVENTS/BYTES/DELAY_MS` 반영, at-least-once 멱등 처리
- [todo] DLQ/재시도: `WB_DLOUT_STREAM`, backoff
- [todo] 관측성: 처리량/지연/실패율 메트릭

완료 기준(DoD)
- 이벤트를 Streams로 적재하고 워커가 배치 커밋하여 DB에 반영(샘플 이벤트: presence_heartbeat, typing 등)

리스크/의존성
- Redis 장애/지연 시 처리 지연 가능 → 백오프/알람 필요

---

## 4) 분산 브로드캐스트 — [ready]
- [done] 발행(publish): `USE_REDIS_PUBSUB!=0`일 때 `prefix + fanout:room:{room_name}`로 Protobuf 바이트 publish
- [todo] 구독(subscribe): 구독 스레드/태스크에서 수신 → 로컬 세션에 재브로드캐스트
- [todo] self-echo 방지: envelope에 `gateway_id`, `origin` 추가 및 필터링
- [todo] 실패/재시도: Pub/Sub 단절/재연결 로직, backoff

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
- [todo] 로그/메트릭/트레이싱 지표 합의 및 수집(간단히 stdout → 이후 OpenTelemetry 고려)

---

## 환경변수/설정(요약)
- Presence: `PRESENCE_TTL_SEC`(기본 30), `PRESENCE_CLEAN_ON_START`
- Redis 키/채널 접두사: `REDIS_CHANNEL_PREFIX`
- 분산 브로드캐스트: `USE_REDIS_PUBSUB`
- Write-behind: `WRITE_BEHIND_ENABLED`, `REDIS_STREAM_KEY`, `REDIS_STREAM_MAXLEN`, `WB_*`
- 마이그레이션 러너: `DB_URI`

---

## 참고 문서
- `docs/db/migrations.md` — 러너 사용법/DDL
- `docs/db/redis-strategy.md` — presence/publish/키 설계
- `docs/protocol.md` — 브로드캐스트 포맷 규칙

