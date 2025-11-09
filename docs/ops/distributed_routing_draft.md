# 게이트웨이·로드밸런서 분산 라우팅 초안 (Detailed)

> 목적: Consistent Hash + Sticky Session + Instance Registry 조합으로 게이트웨이와 로드밸런서를 안정적으로 확장하기 위한 상세 설계.

## 1. 목표
1. **Consistent Hash 자동화** – backend 풀 증감에 1초 내 반응, 링 재구성 이벤트 로그·지표 노출
2. **Sticky Session 신뢰도** – Redis TTL, 로컬 캐시, fallback 로직을 명확히 정의
3. **Gateway 인증 확장** – NoopAuthenticator 외부로 JWT/HMAC, 사설 OAuth 등을 붙이기 쉬운 구조
4. **관측 가능성** – Prometheus/Grafana/로그를 통해 문제 지점을 즉시 확인할 수 있도록 메트릭 설계

## 2. 현재 흐름 요약
1. Client → Gateway (TCP) → Load Balancer (gRPC Stream)
2. LB는 Redis SessionDirectory + Consistent Hash 링으로 backend 결정
3. backend 연결 후 Gateway와 TCP payload 를 프락시
4. heartbeat/metrics/log 로 상태 확인

## 3. 설계 상세
### 3.1 Consistent Hash
| 단계 | 설명 |
| --- | --- |
| 링 Seed | `static_backends` + Registry snapshot + health OK 인 노드만 사용 |
| 링 갱신 트리거 | ① Helm deploy ② Instance Registry TTL 만료 ③ 운영자 API 호출 |
| 데이터 구조 | `std::map<uint32_t, BackendRef>` (virtual node 128개) + RCU 카피 |
| 이벤트 | `lb_hash_rebuild_total` Counter, `core::log::info("hash_rebuild new=<n> removed=<m>")`

Pseudo-code:
```cpp
auto snapshot = registry.fetch();
auto healthy = filter(snapshot, [](auto& rec){ return rec.status == "ready"; });
auto new_ring = build_ring(healthy, static_backends_);
std::atomic_store(&hash_ring_, new_ring);
```

### 3.2 Sticky Session
- Redis 키: `gateway/session/<client_id>`
- 값: `{"backend":"server-1","expires":"..."}` (JSON) 또는 plain string
- TTL: `LB_SESSION_TTL` seconds (기본 45)
- 로컬 캐시: `std::unordered_map` + `steady_clock::time_point expires`
- Fallback: Redis miss → Consistent Hash → Redis SETNX → 캐시 저장

### 3.3 Gateway 인증
| Hook | 설명 |
| --- | --- |
| `validate_connect(SessionContext&, AuthPayload&)` | 최초 접속 시 호출, subject 반환 |
| `on_token_refresh` (후속) | 장시간 연결 시 토큰 갱신을 트리거 |

토큰 헤더 예시:
```
client-id: user42
authorization: Bearer <jwt>
```
실패 시 Gateway는 `ROUTE_KIND_CLIENT_CLOSE` + `AUTH_FAILED` 를 전송하고 Load Balancer 세션을 닫는다.

### 3.4 운영
- Pre-warm: `docs/ops/prewarm.md`
- 알람: `sum(increase(lb_backend_idle_close_total[5m]))`, `chat_subscribe_last_lag_ms`
- 롤링 업데이트: LB → Gateway → Server 순, 링 TTL 을 2배 이상으로 늘린 뒤 진행

## 4. 구현 체크리스트
1. [ ] 링 재구성 코드가 O(n log n) 이하인지 확인
2. [ ] Redis SETNX / TTL 조합에 race 는 없는가?
3. [ ] gRPC Stream 재연결 시 exponential backoff 적용
4. [ ] Prometheus 로그 파서가 `metric=lb_backend_idle_close_total` 을 수집하는가
5. [ ] runbook/알람 문서가 최신 상태인가

## 5. 개방 이슈
- Sticky session 을 완전히 없애고 Consistent Hash 만으로 충분한가?
- Gateway 인증 실패 정보를 Load Balancer가 별도 통계로 수집해야 하는가?
- Redis 장애 시 fallback 으로 in-memory cache 만으로 운영 가능한가?

## 6. 다음 단계
1. 위 체크리스트를 기반으로 Design Spec 확정 → 리뷰 미팅
2. `docs/roadmap.md` 에 P1/P2 항목 연결
3. CI 에 Gateway/LB 통합 테스트, idle close 지표 검증 추가
