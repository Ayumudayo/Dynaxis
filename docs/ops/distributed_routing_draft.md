# 게이트웨이·로드밸런서 분산 라우팅 확장 초안 (Draft)

본 초안은 게이트웨이(`gateway_app`)와 로드밸런서(`load_balancer_app`)를 다중 인스턴스 환경에서 안정적으로 확장하기 위한 설계 방향을 문서화한다. 핵심 초점은 Consistent Hash 링 재구성, Redis 기반 sticky session, Gateway 인증 파이프라인, 그리고 운영 자동화이다.

> **진행 현황**  
> Consistent Hash 링 재구성과 Sticky session 보강은 1차 구현 완료(2025-10-26). 나머지 항목은 P2/P3 우선순위로 백로그에 남아 있다.

## 1. 목표
- **Consistent Hash 자동화**: Backend 풀 증감(추가/제거/격리) 시 해시 링을 즉시 재구성하고, 변경 이벤트를 안정적으로 퍼뜨린다.
- **Sticky Session 신뢰도 향상**: Redis 기반 세션 디렉터리를 단일 진실 소스로 삼아 TTL, 재바인딩, 장애 복구 시나리오를 명확히 한다.
- **Gateway 인증 개선**: 토큰/자격 증명 변화를 빠르게 반영할 수 있도록 인증 플러그인을 표준화한다.
- **운영 자동화**: 롤아웃·롤백, 관측 지표, 디버깅 절차를 문서화해 인시던트 대응 속도를 높인다.

## 2. 현재 동작 개요
- Load Balancer는 `LB_BACKEND_ENDPOINTS`를 읽어 `configure_backends()`에서 링을 초기화한다.
- Redis Session Directory(`gateway/session/<client>`)는 게이트웨이가 바라보는 sticky 정보를 담으며, 링 재구성과 직접적인 의존성은 없다.
- 기본 인증기는 `auth::NoopAuthenticator`로 설정되어 있으며 추가 자격 검증은 미사용 상태다.
- `USE_REDIS_PUBSUB=1`일 때 게이트웨이는 Redis Pub/Sub로 채팅 브로드캐스트를 전달, 로드밸런서도 동일 채널을 모니터링한다.

## 3. 세부 설계

### 3.1 Consistent Hash 링 재구성
| 항목 | 내용 |
| --- | --- |
| 트리거 | Redis `gateway/instances/*` heartbeat, Admin API 명령, 운영자가 강제하는 재빌드 |
| 절차 | 1) 새 backend 집합 스냅샷 → 2) 기존 링과 diff → 3) 링 재구성 후 Redis 키에 저장 → 4) TTL 만료 전 알림 브로드캐스트 |
| 동시성 | 해시 링 데이터는 `std::shared_mutex` 또는 RCU 계열 구조로 보호 |
| 실패 처리 | 링 생성 실패 시 이전 스냅샷 유지, WARN 로그 및 알람 |
| 관측 | 링 재구성 이벤트를 INFO 로그 + `lb_hash_rebuild_total` Prometheus 카운터로 집계 |

### 3.2 Sticky Session
| 항목 | 내용 |
| --- | --- |
| Redis 정책 | In-memory 캐시와 Sticky 세션이 일치하지 않을 경우 WARN 로그 및 `lb_session_cache_mode` 메트릭에 사유 기록 |
| TTL 전략 | 클라이언트 heartbeat 또는 Gateway heartbeat 시 `SessionDirectory::refresh_backend()`를 호출하여 TTL 갱신 |
| Backend 격리 | 특정 backend가 장애나 배포로 빠질 경우 Redis에서 해당 backend 세션을 제거하고, 링 재구성과 동기화 |
| 복구 절차 | Gateway는 복구 시 Redis 값을 우선 읽고 없을 경우 fallback backend를 선택, `SESSION_MOVED` 알림을 클라이언트에 발송 |

### 3.3 Gateway 인증 파이프라인
| 요소 | 설명 |
| --- | --- |
| 인터페이스 | `auth::IAuthenticator::validate_connect(SessionContext&, const AuthPayload&)`를 표준 엔트리 포인트로 사용 |
| 기본 정책 | `ALLOW_ANONYMOUS=1`일 때 NoopAuthenticator 유지, 0이면 JWT/HMAC 등의 외부 검증기로 대체 |
| 세션 저장소 | 인증 성공 후 Redis/DB 모두에 `auth/session/<id>` 형태로 캐시 (TTL은 인증 종류에 따라 가변) |
| 실패 처리 | 인증 실패 시 `ROUTE_KIND_CLIENT_CLOSE`와 `AUTH_FAILED` 코드를 함께 전송해 UI가 원인을 표시하도록 한다 |

### 3.4 운영/배포 수칙
- **환경 분리**: `.env.gateway`, `.env.lb`, `.env.server`를 분리 관리하고 Config Drift를 감시한다.
- **Rolling Update**: 링 TTL을 충분히 늘리고 새 인스턴스의 heartbeat를 확인한 뒤 구 버전을 제거한다.
- **알림 체계**: Redis 연결 단절, sticky session miss, 재바인딩 실패 등 주요 실패에 대해 Alert 룰을 정의한다.
- **테스트 매트릭스**: `/join`, `/chat` 반복 호출로 여러 backend를 순회하는 스모크 테스트를 CI에 포함한다.

- **백엔드 유휴 타임아웃**: LB_BACKEND_IDLE_TIMEOUT(기본 30초)으로 gateway- backend 스트림이 멈추지 않도록 하고, 종료 시 metric=lb_backend_idle_close_total 로그를 관측한다.

## 4. 우선순위 로드맵
1. **P1 – Consistent Hash 고도화**
   - Redis heartbeat watcher (`LB_HASH_RECONFIG_INTERVAL`) 구현
   - 링 상태 RCU 캐시 또는 lock-free 스냅샷 추가
   - 링 TTL 만료 감시 및 강제 재구성
2. **P1 – Sticky Session 보강**
   - 생성/재바인딩/만료 이벤트 로깅 강화
   - `SESSION_MOVED` / `SESSION_REBIND` 알림 프로토콜 정식 문서화
3. **P2 – Gateway 인증 모듈화**
   - `IAuthenticator` 확장 샘플 및 NoopAuthenticator 정리
   - `ALLOW_ANONYMOUS`, `AUTH_PROVIDER`, `AUTH_ENDPOINT` 등의 환경 변수 정의
   - 실패 원인별 사용자 메시지 정비
4. **P3 – 토큰 기반 SSO & E2E 테스트**
   - HMAC 기반 토큰 PoC
   - Load Balancer가 토큰 메타데이터를 Redis에 캐싱하도록 확장
   - devclient에서 토큰을 주고받는 CLI 옵션 추가
5. **운영 TODO**
   - `LB_BACKEND_IDLE_TIMEOUT` 같은 환경 변수로 backend idle 감시 임계값 노출
   - idle watcher 동작 여부를 관측할 Prometheus 지표(`lb_backend_idle_close_total`) 추가
   - Gateway/LB gRPC stream health 체크 주기 및 재시도 정책 정의

## 5. 개방된 질문
- 링 재구성으로 인해 세션이 이동할 때 클라이언트 알림은 `SESSION_MOVED`만으로 충분한가, 아니면 별도 grace period가 필요한가?
- Gateway 인증 실패에 대해 Load Balancer가 추가 정보(예: backend 정책)를 참조해야 하는가?
- Redis 장애 시 fallback 전략(예: in-memory) 없이 운영이 가능한가? -> `LB_REDIS_REQUIRED`의 기본값 재검토 필요.

## 6. 다음 단계
1. 본 초안을 기반으로 Design Spec을 확정하고 리뷰 회의를 잡는다.
2. `docs/roadmap.md`에 P1/P2 항목을 연결해 우선순위를 명시한다.
3. CI 파이프라인에 Redis/Gateway/Load Balancer 통합 테스트 시나리오를 추가한다.
