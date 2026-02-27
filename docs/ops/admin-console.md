# 운영 관리 GUI 설계 (Admin Console)

## 1. 배경

관련 문서:

- API 계약: `docs/ops/admin-api-contract.md`

현재 Knights는 운영에 필요한 핵심 관측면을 이미 갖고 있다.

- 서비스별 admin HTTP: `/metrics`, `/healthz`, `/readyz` (`server_app`은 `/logs` 추가)
- Redis 기반 분산 상태:
  - 인스턴스 레지스트리: `gateway/instances/*`
  - 세션 디렉터리(sticky): `gateway/session/{client_id}`
- Redis Pub/Sub fanout:
  - `${REDIS_CHANNEL_PREFIX}fanout:room:{room}`
  - `${REDIS_CHANNEL_PREFIX}fanout:refresh:{room}`
  - `${REDIS_CHANNEL_PREFIX}fanout:whisper`

문제는 운영자가 이 정보를 한 곳에서 보지 못하고, 장애 대응 시 Redis/Prometheus/로그를 직접 넘나들어야 한다는 점이다.

## 2. 목표 / 비목표

### 2.1 목표

1. 운영자 관점의 단일 뷰 제공
   - 클러스터 상태(ready/deps/session load)
   - 세션 -> backend 라우팅 상태
   - write-behind 처리 상태
2. **조회 + 제한적 write**(disconnect/announce/settings) 제공
3. 기존 데이터 경로(HAProxy -> gateway -> server)와 분리된 제어면(control plane) 확보

### 2.2 비목표

- 서버/게이트웨이/워커의 실행 경로를 대규모 리팩터링
- 브라우저가 Redis/Postgres/Prometheus에 직접 접속
- drain/undrain, 권한 정책 수정, 영구 설정 저장

## 3. 설계 결정

### 3.1 채택안

**Web Admin Console + 별도 `admin_app`(API/집계 서비스)**

- 브라우저는 `admin_app`만 호출
- `admin_app`이 Prometheus/Redis/Postgres/서비스 admin endpoint를 조회
- 인증/권한/감사를 `admin_app` 경계에서 일원화

### 3.2 대안 비교

- Native GUI(예: ImGui 기반)는 배포/버전 관리/권한 통제가 분산되기 쉽다.
- 단일 운영자 워크스테이션 도구가 필요한 특수 상황이 아니면, 웹 콘솔이 운영 표준화에 유리하다.

## 4. 현재 코드베이스에서 사용 가능한 데이터 소스

### 4.1 서비스 엔드포인트

공통 제공 (`core/src/metrics/http_server.cpp`):

- `/metrics` (`/` alias)
- `/healthz` (`/health` alias)
- `/readyz` (`/ready` alias)
- `/logs` (callback이 있을 때만; 없으면 404)

`server_app`은 `server/src/app/metrics_server.cpp`에서 `/logs`를 연결한다.

### 4.2 readiness/의존성 의미

`AppHost` (`core/src/app/app_host.cpp`) 기준:

- readiness 본문 예시: `not ready: starting, deps=redis,db`
- dependency metrics:
  - `knights_dependency_ready{name="redis",required="true"} 1|0`
  - `knights_dependencies_ok 1|0`

### 4.3 Redis 상태 모델

1) 인스턴스 레지스트리 (`server/src/state/instance_registry.cpp`)

- key: `{SERVER_REGISTRY_PREFIX}{instance_id}` (기본 `gateway/instances/`)
- TTL: `SERVER_REGISTRY_TTL` (기본 30s)
- 값(JSON):
  - `instance_id`, `host`, `port`, `role`, `capacity`, `active_sessions`, `ready`, `last_heartbeat_ms`

2) 세션 디렉터리 (`gateway/src/session_directory.cpp`, `gateway/src/gateway_app.cpp`)

- key: `gateway/session/{client_id}`
- 값: `instance_id` (plain string)
- TTL: 600s (현재 gateway에서 고정)
- sticky 갱신은 backend TCP connect 성공 후에만 반영

3) fanout 채널 (`server/src/app/bootstrap.cpp`, `server/src/chat/chat_service_core.cpp`)

- pattern subscribe: `${REDIS_CHANNEL_PREFIX}fanout:*`
- whisper route channel: `${REDIS_CHANNEL_PREFIX}fanout:whisper`
- envelope: `gw=<id>\n<protobuf payload>`

### 4.4 기본 포트 (docker/stack)

- HAProxy traffic: `6000`
- HAProxy stats/metrics: `8404`
- gateway metrics: `36001`, `36002`
- server metrics: `39091`, `39092`
- wb_worker metrics: `39093`

참조: `docker/stack/docker-compose.yml`, `docker/stack/haproxy/haproxy.cfg`

## 5. 제안 아키텍처

```text
Operator Browser
  -> (internal ingress / VPN / allowlist)
  -> admin_app (authn/authz/audit + aggregation)
      -> Prometheus HTTP API
      -> Redis (read-only credentials)
      -> Postgres (read-only credentials)
      -> service admin endpoints (/metrics,/healthz,/readyz,/logs)
```

핵심 원칙:

1. data plane과 control plane 분리
2. 브라우저 직접 데이터스토어 접근 금지
3. `admin_app` 장애가 채팅 경로에 영향 주지 않도록 분리 배포

## 6. MVP 범위 (Read + Write-lite)

### 6.1 화면/기능

1. 클러스터 개요
   - 서비스별 up/ready/dependency 상태
   - instance별 active sessions
2. 라우팅 및 세션 조회
   - client_id 입력 -> sticky backend 조회
   - backend별 레지스트리 레코드 확인
3. Write-behind 상태
   - `wb_pending`, flush/ack/reclaim 추세
4. Fanout 개요
   - room/whisper fanout 관련 지표/로그 링크

### 6.2 신규 API (제안)

아래는 **신규로 추가할 admin_app API**이다.

- `GET /api/v1/overview`
  - 서비스 요약(up/ready/deps/session load)
- `GET /api/v1/instances`
  - instance registry 목록 + ready/active_sessions
- `GET /api/v1/sessions/{client_id}`
  - sticky mapping + 대상 instance 상태
- `GET /api/v1/users`
  - 현재 접속 사용자 목록 + backend 매핑
- `POST /api/v1/users/disconnect`
  - 선택 사용자 강제 종료 명령 publish
- `POST /api/v1/announcements`
  - 서버 전체 공지 명령 publish
- `PATCH /api/v1/settings`
  - 런타임 설정 변경 명령 publish
- `GET /api/v1/worker/write-behind`
  - worker backlog/flush/reclaim 요약
- `GET /api/v1/metrics/links`
  - Grafana/Prometheus deep-link 템플릿

### 6.3 API 응답 예시 (제안)

```json
{
  "instance_id": "server-1",
  "ready": true,
  "active_sessions": 42,
  "last_heartbeat_ms": 1760535000123,
  "source": {
    "registry": "gateway/instances/server-1",
    "metrics_url": "http://server-1:9090/metrics"
  }
}
```

## 7. 보안 모델

### 7.1 인증/인가

- 초기: 내부망 + VPN/allowlist + reverse proxy auth
- 확장: OIDC(SSO) 연동
- RBAC 최소 3단계
  - `viewer` (MVP 기본)
  - `operator` (announcement)
  - `admin` (정책/권한 관리)

### 7.2 감사(Audit)

모든 admin API 호출 로그 기록:

- `actor`, `role`, `request_id`, `source_ip`, `action`, `resource`, `result`, `latency_ms`, `timestamp`

MVP는 write 액션까지 포함하므로 감사 로그를 필수로 남긴다.

### 7.3 자격 증명 분리

- Redis/Postgres는 read-only 계정 사용
- 브라우저 토큰과 데이터스토어 자격 증명 분리
- 비밀값은 `.env` 커밋 금지, 운영 secret store 사용

## 8. 운영 안전장치

1. 서버측 write 액션은 role-gated + channel allowlist 적용
2. 쿼리 제한: pagination, timeout, max cardinality
3. Redis key 조회는 범위 제한(prefix 고정) + 과도한 SCAN 금지
4. 장애 시 admin_app를 즉시 차단 가능한 kill-switch 준비

## 9. 단계별 실행 계획

### 단계 0 - 명세 및 스켈레톤

- 본 문서 확정
- `admin_app` 스켈레톤(health/ready/metrics) 생성
- docker stack에 admin profile 초안 추가

### 단계 1 - 읽기 전용 기준선

- `overview`, `instances`, `sessions/{client_id}`, `worker/write-behind` API 구현
- 최소 웹 UI(테이블 + 검색 + 링크)
- 감사 로그/권한(viewer) 적용

### 단계 2 - 운영 편의 강화

- fanout/whisper 전용 운영 지표 보강
- 필터/검색/시계열 비교 UX 개선
- 알람 임계치 뷰 제공

### 단계 2.5 - Write-lite 제어면

- `users`, `users/disconnect`, `announcements`, `settings` API 구현
- role-gated UX(viewer/operator/admin) 적용
- Redis Pub/Sub 기반 명령 채널 연동

### 단계 3 - 고급 쓰기 액션 (선택)

- drain/undrain 같은 라우팅 제어
- 이중 확인 + idempotency key + 감사 강화
- feature flag 기본 off

## 10. 구현 전 체크리스트

- [ ] 브라우저에서 Redis/Postgres/Prometheus 직접 접근 경로가 없는가?
- [ ] admin_app 권한 모델이 viewer 기본 최소권한인가?
- [ ] write endpoint가 role-gated + 감사 추적 가능한가?
- [ ] 운영 포트/네트워크가 내부 전용으로 제한되는가?
- [ ] 감사 로그가 운영 추적에 충분한 필드를 포함하는가?

## 11. 오픈 이슈

1. `client_id` 규칙 표준화
   - 현재 sticky key는 `gateway/session/{client_id}`를 사용하므로, 운영 조회 키 정책(유저명/UUID/subject)을 명확히 해야 한다.
2. fanout 가시성
   - whisper/local/remote를 구분하는 메트릭을 server에서 추가할지 결정 필요.
3. admin_app 구현 위치
   - `tools/` 하위 신규 바이너리 vs 별도 디렉터리(빌드/배포 정책 포함) 확정 필요.
