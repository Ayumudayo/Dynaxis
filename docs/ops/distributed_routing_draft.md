# 게이트웨이 분산 라우팅 초안 (HAProxy 전제)

> 목적: 외부 TCP(L4) 로드밸런서(예: HAProxy)로 **Gateway 수평 확장**을 하고, Gateway 내부에서 **Sticky Session + Instance Registry + Least Connections** 조합으로 backend(server_app)를 안정적으로 선택하기 위한 상세 설계.

## 1. 목표
1. **Gateway 수평 확장** – HAProxy가 여러 `gateway_app` 인스턴스로 연결을 분산
2. **Backend 선택 자동화** – Instance Registry(`active_sessions`) 기반 least-connections 라우팅
3. **Sticky Session 신뢰도** – Redis TTL + 로컬 캐시 + fallback 로직을 명확히 정의
4. **관측 가능성** – Prometheus/Grafana/로그를 통해 병목/장애 지점을 확인할 수 있도록 지표 설계

## 2. 현재 흐름 요약
1. Client → HAProxy (TCP) → Gateway (TCP)
2. Gateway는 Redis Instance Registry를 조회해 backend(server_app)를 선택
3. Gateway ↔ Server 사이에 TCP 브리지(`BackendConnection`)를 구성하고 payload를 중계
4. Redis Pub/Sub/Streams(Write-behind)는 server_app이 담당

## 3. 설계 상세
### 3.1 Backend 선택(Least Connections)
- 입력: Instance Registry의 `InstanceRecord{instance_id, host, port, active_sessions, ...}` 목록
- 선택: `active_sessions`가 가장 작은 backend 1개 선택
- 개선(선택): 최소값 동률이 여러 개면 랜덤/라운드로빈으로 분산해 편중을 줄인다

Pseudo-code:
```cpp
auto instances = registry.list_instances();
auto candidates = filter_valid(instances);
auto selected = min_by(candidates, [](auto& rec){ return rec.active_sessions; });
return {selected.host, selected.port, selected.instance_id};
```

### 3.2 Sticky Session
- Redis 키: `gateway/session/<client_id>`
- 값: backend `instance_id` (plain string)
- TTL: Gateway SessionDirectory TTL (현재 구현은 600초)
- 로컬 캐시: `std::unordered_map` + `steady_clock::time_point expires`
- Fallback:
  - sticky hit + backend alive → 그 backend 사용
  - sticky miss/invalid → 3.1의 least-connections → Redis set/refresh → 캐시 저장

### 3.3 Gateway 인증
| Hook | 설명 |
| --- | --- |
| `auth::IAuthenticator::authenticate` | 최초 접속 시 호출, subject/client_id 반환 |

실패 시 Gateway는 연결 종료 또는 에러 응답을 전송한다.

### 3.4 운영
- Pre-warm: `docs/ops/prewarm.md`
- 롤링 업데이트: HAProxy 설정 반영 → Gateway → Server 순
- 알람/지표: Gateway backend 연결 실패율, Redis latency, server active sessions, write-behind 지표

## 4. 구현 체크리스트
1. [ ] backend 선택이 비정상 값(host/port 누락)을 필터링하는가?
2. [ ] Redis sticky SETNX/TTL 조합에 race 는 없는가?
3. [ ] Instance Registry TTL 만료 시 gateway가 stale backend를 제거하는가?
4. [ ] runbook/알람 문서가 최신 상태인가?

## 5. 개방 이슈
- Sticky session 을 완전히 없애고 least-connections 만으로 충분한가?
- Redis 장애 시 fallback 으로 in-memory cache 만으로 운영 가능한가?
- active_sessions 집계 정확도를 높이기 위한 지표/업데이트 주기가 필요한가?

## 6. 다음 단계
1. 위 체크리스트를 기반으로 Design Spec 확정 → 리뷰
2. `docs/roadmap.md` 에 P1/P2 항목 연결
3. CI에 E2E 테스트(다중 Gateway + HAProxy + 다중 Server) 시나리오 추가
