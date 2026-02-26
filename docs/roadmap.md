# Knights Roadmap

### 우선순위 실행 계획 (2025-Q4)
1. **운영 안정성·관측성**: docs/configuration.md, docs/ops/gateway-and-lb.md TODO를 정리해 LB/Server registry 설정, 알람, /metrics 항목을 확정한다. (진행 중)
2. **프로토콜·채팅 명세**: docs/protocol.md, docs/chat/recent-history.md, docs/protocol/snapshot.md TODO를 실제 패킷 구조·시퀀스로 채운다. (다음 작업)
3. **데이터 경로/테스트**: docs/db/redis-strategy.md, docs/db/write-behind.md, docs/tests.md 관측 지표·DLQ·통합 테스트 계획을 보강한다. (차순위)
4. **중장기 아키텍처**: docs/core-design.md, docs/server-architecture.md에 남은 Hive 분리, TaskScheduler 테스트, observability 표준화를 추적한다. (대기)

본 문서는 Knights 프로젝트의 기능 로드맵과 우선순위를 기록한다. 각 항목은 완료 상태와 DoD(Definition of Done)를 함께 관리하며, 운영 노트·테스트 계획 등 후속 작업으로 연결된다. 우선순위 표시는 `[done]`, `[in-progress] (wip)`, `[ready]`, `[todo]` 포맷을 사용한다.

## 1. DB 계층 강화 `[in-progress]`
- [done] 기본 마이그레이션 스크립트(`tools/migrations/0001..0004`)와 실행기 정비.
- [done] `libpqxx` 기반 PostgreSQL 연결 계층 구성 및 CMake 의존성 명시.
- [todo] Users/Rooms/Messages/Memberships/Session 테이블에 대한 통합 테스트 추가.
- [todo] Migration 롤백/재적용 및 lock_timeout, statement_timeout 등 운영 파라미터 가이드 작성.
- **DoD**: 주요 CRUD 경로와 마이그레이션 테스트가 자동화되고, 장애 시 롤백 전략이 문서화되어야 한다.

## 2. Presence 안정화 `[in-progress]`
- [done] `presence:room:{room_id}` / `presence:user:{user_id}` 키 설계 및 TTL 관리.
- [done] `PRESENCE_CLEAN_ON_START` 옵션으로 초기 정리 지원.
- [todo] TTL 만료·로그아웃 이벤트가 Memberships와 동기화되도록 write-behind와 연계.
- **DoD**: 로그인, 채팅, 로그아웃까지 Presence가 정확히 반영되고, Redis 장애 시 복구 전략이 마련되어야 한다.

## 3. Write-behind 파이프라인 `[in-progress]`
- [done] Redis Streams 수집기, `wb_worker`, 배치/재시도/DLQ 설정 추가.
- [done] 이벤트 스키마 정의(`session_login`, `room_join`, `room_leave`, `session_close`).
- [todo] 메트릭(`wb_commit_ms`, `wb_fail_total`, `wb_pending`) 추가와 운영 대시보드 정비.
- [todo] DLQ 재처리 자동화 스크립트 개선.
- **DoD**: 정상 경로와 오류 경로에 대한 자동 테스트가 존재하고, 운영자가 DLQ 상태를 쉽게 파악할 수 있어야 한다.

## 4. Redis Pub/Sub 확대 `[ready]`
- [done] `USE_REDIS_PUBSUB` 플래그와 Chat broadcast 발행 경로 정비.
- [todo] 룸 생성/잠금에 대한 분산 락, 캐시 초기화 전략 작성.
- [todo] Pub/Sub 수신 및 self-echo 필터링에 대한 통합 테스트 추가.
- **DoD**: 다중 서버 인스턴스 간 메시지가 손실 없이 전달되고, 장애 시 폴백 전략이 정의되어야 한다.

## 5. Load Balancer 다중 인스턴스 안정화 `[in-progress]`
- [done] Redis 기반 SessionDirectory와 조건부 SET/DEL 구현.
- [done] 실패 카운트/쿨다운 기반 backend health 게이팅.
- [done][P1] Consistent Hash 링 재구성 자동화: LB_DYNAMIC_BACKENDS와 Redis heartbeat 정보를 사용해 hash ring을 주기적으로 재작성 완료 (참조: docs/ops/distributed_routing_draft.md)
- [done][P1] Sticky session 등록/로그 정비: registry 폴백과 로그 정리를 마무리해 장애 복구 절차를 문서화함 (참조: docs/ops/distributed_routing_draft.md)
- [todo][P2] Sticky routing 통합 테스트: 다중 서버, TTL 만료, Redis 장애 시나리오 자동화. → 초안: `docs/ops/distributed_routing_draft.md`
- [todo][P2] Redis Pub/Sub 정합성: backend 추가/제거 이벤트 브로드캐스트, 캐시 초기화 전략 수립.
- [todo][P3] 관측성 강화: 세션 수, 재할당, backend 상태를 메트릭으로 노출.
- [todo][P3] Gateway 인증 연동 설계: 토큰 검증과 Redis 세션 레지스트리 연계. → 초안: `docs/ops/distributed_routing_draft.md`
- **DoD**: 동일 client_id가 안정적으로 sticky routing되고, backend 장애/TTL 만료 시 안전하게 재배치되며, 통합 테스트가 통과해야 한다.

## 6. Observability & Runbook `[todo]`
- [todo] Gateway/Load Balancer 메트릭 엔드포인트 추가(세션 수, 오류율).
- [todo] 공통 로그 포맷 및 로그 레벨 정책 정의.
- [todo] Alert/Runbook 문서(`docs/ops/fallback-and-alerts.md`, `docs/ops/runbook.md`) 업데이트 및 인시던트 체크리스트 작성.
- **DoD**: 주요 장애 시나리오에 대한 대응 절차와 모니터링 지표가 문서화되어야 한다.

## 7. 인증/보안 `[ready]`
- [todo] `auth::IAuthenticator`를 확장해 토큰 검증, 세션 레지스트리 연동, 향후 OAuth2/OpenID Connect 대응 설계.
- [todo] 게이트웨이 입력 검증 강화 및 남용/침입 방지 로깅 추가.
- **DoD**: 최소한의 인증 뼈대가 코드에 존재하고, 실제 구현 계획이 PRD 형태로 준비되어야 한다.

## 8. 엔진화 장기 과제 `[todo]`
- [todo] Hive/Connection을 별도 네트워크 엔진 모듈로 추출.
- [todo] ECS/플러그인/스크립팅(Lua·WASM) 지원 여부 조사 및 PoC 작성.
- [todo] 다중 게임 서비스(샤딩, 멀티 테넌트) 지원 계획 수립.
- **DoD**: 아키텍처 초안과 PoC 결과가 문서화되어 차후 개발 팀이 확장 기반을 활용할 수 있어야 한다.

## 9. 참고 문서
- 서버 구조: `docs/server-architecture.md`
- Gateway & Load Balancer 운영: `docs/ops/gateway-and-lb.md`
- 운영 관리 콘솔 설계: `docs/ops/admin-console.md`
- 운영 관리 API 계약: `docs/ops/admin-api-contract.md`
- 운영 관리 GUI IA: `docs/ops/admin-gui-wireframe.md`
- Core 설계 메모: `docs/core-design.md`
- Redis 전략: `docs/db/redis-strategy.md`
- Write-behind: `docs/db/write-behind.md`

로드맵은 분기마다 점검하며, 완료/지연 사유를 PR 혹은 회고 문서와 연결하여 추적한다.
