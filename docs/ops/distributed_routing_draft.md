# 게이트웨이·로드밸런서 분산 라우팅 초안

이 문서는 게이트웨이(`gateway_app`)와 로드밸런서(`load_balancer_app`) 사이에서 세션을 분산시키는 구조를 정리한 초안이다. 핵심 목표는 백엔드 증감에 자동 대응하고, sticky session을 Redis에 기록하며, 장애 시 빠르게 복구하는 것이다.

## 1. 목표
1. **Consistent Hash 자동화** – backend 풀 변화에 즉시 반응해 링을 재구성하고 이벤트를 로그/메트릭으로 남긴다.  
2. **Sticky Session 안정화** – Redis에 `gateway/session/<client>` 형태로 세션을 저장, TTL을 주기적으로 갱신한다.  
3. **Gateway 인증 확장성** – NoopAuthenticator에서 JWT/HMAC 등으로 확장 가능한 인터페이스를 유지한다.  
4. **운영 자동화** – 배포/장애 시 재구성 방법, 모니터링 포인트, runbook을 명확히 한다.

## 2. 현재 흐름
- 게이트웨이는 gRPC 스트림을 통해 로드밸런서에 세션을 위임한다.  
- 로드밸런서는 Consistent Hash 또는 sticky 디렉터리를 통해 backend를 선택한 뒤 TCP 프락시를 생성한다.  
- 세션 바인딩은 Redis에 저장되며, heartbeat 또는 데이터 송수신이 있을 때 TTL을 갱신한다.

## 3. 세부 설계

### 3.1 Consistent Hash 재구성
- 소스: 정적 설정(`LB_BACKEND_ENDPOINTS`), AWS Registry(InstanceState), 관리자 API.  
- 절차: 새 스냅샷 → diff → 링 재구성 → Redis/로그에 결과 게시.  
- 실패 시 이전 링을 유지하고 `lb_hash_rebuild_total` 카운터를 증가시킨다.

### 3.2 Sticky Session
- `gateway/session/<client>` 값을 Redis에 저장하고 TTL을 `SessionDirectory::refresh_backend()` 로 갱신한다.  
- backend 장애로 세션이 제거되면 fallback backend를 선택한 뒤 `SESSION_MOVED` 메시지를 게이트웨이가 클라이언트에 전달한다.  
- 로컬 캐시는 만료 시간을 기록하여 Redis TTL과 동기화한다.

### 3.3 Gateway 인증
- 인터페이스: `auth::IAuthenticator::validate_connect(SessionContext&, const AuthPayload&)`.  
- 환경변수 `ALLOW_ANONYMOUS`, `AUTH_PROVIDER`, `AUTH_ENDPOINT` 로 정책을 제어한다.  
- 인증 실패 시 `ROUTE_KIND_CLIENT_CLOSE + AUTH_FAILED` 코드를 클라이언트에 전송한다.

### 3.4 운영/배포
- `.env.gateway`, `.env.lb`, `.env.server` 를 분리 관리하고, IaC(Terraform)로 Redis/RDS/Secrets를 선언한다.  
- Rolling Update 시 링 TTL을 충분히 늘린 뒤 새 인스턴스를 먼저 올리고, 헬스체크 통과 후 구 버전을 내린다.  
- Publish/Subscribe lag, sticky session miss, Redis 연결 실패 등을 Prometheus와 Alertmanager에 연동한다.  
- `LB_BACKEND_IDLE_TIMEOUT` 기본값은 30초이며, idle 종료 시 `metric=lb_backend_idle_close_total` 로그를 남겨 Grafana 패널로 확인한다.

## 4. 로드맵
1. **P1 – Consistent Hash 고도화**  
   - Redis heartbeat watcher(`LB_HASH_RECONFIG_INTERVAL`) 구현  
   - 링 데이터 구조를 RCU/lazy copy로 개선  
   - TTL 만료를 감지해 자동 재구성 트리거
2. **P1 – Sticky Session 보강**  
   - 생성/재바인딩/만료 이벤트를 모두 로깅  
   - `SESSION_MOVED`, `SESSION_REBIND` 프로토콜 명세화
3. **P2 – Gateway 인증 모듈화**  
   - JWT/HMAC 샘플 구현, 설정 문서화  
   - 실패 사유를 UI에 전달할 수 있는 코드 경로 추가
4. **P3 – 토큰 기반 SSO 및 E2E 테스트**  
   - devclient CLI에서 토큰을 주고받는 기능  
   - Helm 차트에 인증/토큰 설정을 노출

## 5. 개방 이슈
- 링 재구성 시 세션 이동 알림을 어떻게 사용자에게 전달할 것인가?  
- Gateway 인증 실패 정보를 로드밸런서가 참고해야 하는가?  
- Redis 장애 시 sticky session 없이 fallback으로 운영 가능한가?

## 6. 다음 단계
1. 이 문서를 기반으로 정식 Design Spec을 작성하고 리뷰 회의를 잡는다.  
2. `docs/roadmap.md` 에 P1/P2 항목을 연결한다.  
3. CI에 게이트웨이-로드밸런서 통합 테스트와 idle close 지표 검증을 추가한다.
