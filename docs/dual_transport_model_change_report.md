# 투트랙 전송(TCP/UDP) + Opcode 정책 메타 모델 변경 리포트

## 1. 문서 목적

본 문서는 다음 모델 변경을 기준으로 영향 범위와 실행 전략을 체계화한다.

- 기존: TCP 기반 `opcode -> handler` 중심 처리
- 변경: `opcode = ID + 상태 제약 + 처리 위치 + 전송/신뢰성 정책`
- 목표: MMORPG 트래픽(TCP)과 FPS 트래픽(UDP)을 단일 코어 정책으로 운용

핵심 원칙은 **투트랙 전송(Two-track transport), 원트랙 게임 로직(one-track game logic)** 이다.

---

## 2. 변경 모델 정의 (현행 As-Is / 목표 To-Be)

## 2.1 현행(As-Is)

- 전송: TCP 중심 (`Session`, `Acceptor`, `Listener`)
- 디스패치: `msg_id -> handler` (`Dispatcher` + `router.cpp`)
- 프로토콜: 길이 기반 프레임 + 헤더 시퀀스 필드
- 정책 위치: 세션/핸들러/운영 규칙에 분산

## 2.2 목표(To-Be)

Opcode 정책을 단일 메타로 승격한다.

```cpp
struct OpcodePolicy {
    std::uint16_t id;
    SessionStatus required_state;
    ProcessingPlace processing_place;
    TransportMask transport;      // TCP | UDP | BOTH
    DeliveryClass delivery;       // ReliableOrdered | Reliable | UnreliableSequenced
    std::uint8_t channel;         // UDP lane/channel
};
```

변경 후 처리 흐름:

1. TCP/UDP ingress에서 공통 `IncomingMessage`로 정규화
2. 정책 테이블(`OpcodePolicy`) 조회
3. 상태/처리 위치/전송 정책 검증
4. 공통 handler 호출

---

## 3. 영향 범위 (전체 맵)

## 3.1 프로토콜/코드생성 계층

대상 파일:

- `core/protocol/system_opcodes.json`
- `server/protocol/game_opcodes.json`
- `tools/gen_opcodes.py`
- `core/include/server/core/protocol/system_opcodes.hpp`
- `server/include/server/protocol/game_opcodes.hpp`
- `docs/protocol/opcodes.md`
- `tools/gen_opcode_docs.py`

변경 포인트:

- opcode 스키마에 정책 필드 추가 (`transport`, `delivery`, `channel`, `required_state`, `processing_place`)
- codegen 산출물에 policy metadata 접근 API 생성
- 문서 생성기(opcode docs)에서 정책 컬럼 노출

리스크:

- generated header 변경으로 consumer 전역 영향
- 스키마 호환성(기존 opcode 정의) 깨짐 가능

완화:

- Phase 1에서 default policy 자동 주입 (no behavior change)
- schema validator 추가 및 CI 체크 강제

## 3.2 코어 네트워크 계층

대상 파일:

- `core/include/server/core/net/session.hpp`
- `core/src/net/session.cpp`
- `core/include/server/core/net/connection.hpp`
- `core/src/net/connection.cpp`
- `core/include/server/core/net/listener.hpp`
- `core/src/net/listener.cpp`
- `core/include/server/core/net/acceptor.hpp`
- `core/src/net/acceptor.cpp`
- `core/include/server/core/net/dispatcher.hpp`
- `core/src/net/dispatcher.cpp`
- `core/include/server/core/protocol/packet.hpp`

변경 포인트:

- transport 추상 인터페이스 도입 (`ITransportSession` 성격)
- TCP `Session`을 인터페이스 구현체로 정렬
- UDP 세션 구현체(신규) 추가
- dispatcher 진입 전 policy gate 적용

리스크:

- 세션 수명주기/strand 모델 회귀
- 읽기/쓰기 타임아웃 및 heartbeat 동작 회귀

완화:

- TCP 경로 불변 유지 후 UDP 경로 별도 추가
- dispatcher gate는 fail-close 방식으로 점진 적용

## 3.3 Gateway 계층

대상 파일:

- `gateway/include/gateway/gateway_app.hpp`
- `gateway/src/gateway_app.cpp`
- `gateway/include/gateway/gateway_connection.hpp`
- `gateway/src/gateway_connection.cpp`

변경 포인트:

- TCP ingress와 UDP ingress 동시 운용 구조
- UDP 세션 바인딩(인증/토큰 기반) 경로 추가
- backend 브릿지에서 transport-aware forwarding 규칙 추가

리스크:

- gateway 백엔드 연결 보호장치(connect timeout/send queue)와 충돌
- 세션 디렉터리 sticky 규칙과 UDP 바인딩 불일치

완화:

- 초기에는 UDP를 특정 opcode 그룹으로만 제한
- TCP 인증 성공 세션에만 UDP 바인딩 허용

## 3.4 서버 앱/핸들러 계층

대상 파일:

- `server/src/app/router.cpp`
- `server/src/app/bootstrap.cpp`
- `server/include/server/chat/chat_service.hpp`
- `server/src/chat/chat_service_core.cpp`
- `server/src/chat/handlers_login.cpp`
- `server/src/chat/handlers_join.cpp`
- `server/src/chat/handlers_chat.cpp`

변경 포인트:

- 라우터 등록 시 policy-aware 등록 또는 정책 조회 경유
- handler는 transport 분기 로직 대신 도메인 로직 유지
- UDP 허용 opcode는 idempotency/sequence guard 필요

리스크:

- 핸들러 내부에 숨어 있는 ordered/reliable 가정 붕괴
- 중복 패킷/순서 역전 시 상태 변형

완화:

- opcode별 delivery contract 명시
- 민감한 상태 변경 opcode는 당분간 TCP-only 유지

## 3.5 인증/보안 계층

대상 문서 및 경로:

- `docs/identity.md`
- `docs/session-context.md`
- `docs/ops/gateway-and-lb.md`

변경 포인트:

- TCP 인증 후 UDP 바인딩 토큰 발급/검증
- replay 방지(sequence window), rate limit, session hijack 방어

리스크:

- UDP 세션 탈취/재전송 공격
- 토큰 만료/재발급 경계 오류

완화:

- 짧은 TTL + 서명 검증 + 세션 키 바인딩
- 실패 횟수 기반 차단 + 운영 알림 연결

## 3.6 관측성/운영 계층

대상 파일/문서:

- `server/src/app/metrics_server.cpp`
- `gateway/src/gateway_app.cpp`
- `docs/ops/observability.md`
- `docker/observability/prometheus/prometheus.yml`
- `docker/observability/grafana/dashboards/`

변경 포인트:

- UDP 품질 메트릭 추가: RTT/loss/jitter/reorder/dup/retransmit
- transport별 traffic/error 지표 분리
- tick 기반 서비스라면 tick duration/late tick 지표 추가

리스크:

- 메트릭 폭증(cardinality)
- 대시보드 부재로 장애 인지 지연

완화:

- label 최소화, 세션 단위는 샘플링/집계
- 알림 규칙과 runbook 동시 갱신

## 3.7 빌드/테스트/CI 계층

대상 파일:

- `CMakeLists.txt`
- `core/CMakeLists.txt`
- `server/CMakeLists.txt`
- `gateway/CMakeLists.txt`
- `tests/CMakeLists.txt`
- `.github/workflows/ci.yml`

변경 포인트:

- UDP 옵션(빌드 플래그, 의존성) 추가
- policy validation/unit/integration test 추가
- TCP-only 회귀 테스트를 release gate로 유지

리스크:

- 플랫폼별 의존성 차이로 CI 불안정
- 통합 테스트 증가로 파이프라인 지연

완화:

- feature flag 기반 단계적 활성화
- fast lane(unit/schema) + slow lane(e2e/soak) 분리

---

## 4. 변경 전략 (단계별)

## 단계 0(Phase 0): 정책 모델 고정 (문서/스키마)

- 정책 필드 및 기본값 정의
- opcode 분류표(TCP-only / UDP-candidate / dual) 확정
- 운영/보안 요구사항 확정

산출물:

- 스키마 초안, 분류표, 리스크 레지스터

## 단계 1(Phase 1): 코드생성 확장 (동작 무변경)

- JSON + generator 확장
- generated metadata API 추가
- 기존 처리 경로는 기존 동작 유지

산출물:

- policy-aware generated headers
- schema validator + docs 업데이트

## 단계 2(Phase 2): 코어 정책 게이트 도입 (TCP only)

- dispatcher 전에 `required_state`/`processing_place` gate 연결
- TCP 트래픽만 대상으로 검증

산출물:

- policy gate + 테스트

## 단계 3(Phase 3): UDP ingress 도입 (제한 롤아웃)

- UDP 세션 도입
- TCP 인증 연동 바인딩
- `UnreliableSequenced` 대상 opcode 일부 전환

산출물:

- dual ingress + UDP 품질 메트릭

## 단계 4(Phase 4): 투트랙 운영 안정화

- opcode별 전송 정책 점진 확대
- 운영 대시보드/알람/런북 정착

산출물:

- production rollout 기준 충족

---

## 5. 권장 Opcode 분류 기준

- TCP-only (초기 고정)
  - 로그인/인증, 계정/권한 변경, 결제성 이벤트, 강정합 상태 변경
- UDP 우선 후보
  - 이동/에임/시야/주기적 상태 동기화
- Dual 후보
  - 전투 이벤트(정확성 요구 높음), 특정 입력 이벤트(서버 authoritative)

원칙:

- 상태를 바꾸는 명령은 기본적으로 Reliable 계열
- 화면 갱신용 정보는 UnreliableSequenced 우선

---

## 6. 주요 위험과 대응

1. **정책 분산 재발**
   - 위험: transport 분기 로직이 handler로 새어 나감
   - 대응: 정책은 opcode metadata 단일 소스에서만 관리

2. **핸들러 순서 가정 붕괴**
   - 위험: UDP 전환 시 기존 ordered 가정 위배
   - 대응: opcode별 sequence/idempotency guard 도입

3. **보안 경계 취약**
   - 위험: replay/session hijack
   - 대응: 서명 토큰 + short TTL + sequence window

4. **운영 복잡도 급증**
   - 위험: 이슈 원인 추적 난이도 상승
   - 대응: transport/delivery 레이블 기반 메트릭 분리

5. **대규모 일괄 변경 실패**
   - 위험: 회귀 범위 과대
   - 대응: phase gate + canary rollout + 즉시 rollback

---

## 7. 검증 전략

## 7.1 기능 검증

- schema validation 테스트
- policy gate 단위 테스트
- TCP 회귀 테스트(기존 전체 경로)
- UDP 경로 통합 테스트(손실/중복/역순 시뮬레이션)

## 7.2 품질 검증

- soak 테스트 (장시간 손실/지터 조건)
- 성능 비교 (CPU, p95/p99 latency, drop/retransmit)

## 7.3 운영 검증

- Prometheus/Grafana 지표 확인
- 알람/런북 훈련

---

## 8. 롤백 전략

- feature flag로 UDP ingress 즉시 차단 가능해야 함
- opcode policy는 TCP fallback 기본값 유지
- 배포 단위별 rollback 문서화 (gateway/server/core)

---

## 9. 완료 기준 (Definition of Done)

1. 모든 opcode가 정책 메타를 가진다(기본값 포함).
2. TCP 기존 경로 회귀 없음(기존 테스트/스모크 통과).
3. UDP 대상 opcode는 손실/중복/역순 테스트 통과.
4. 운영 지표/알람/런북이 준비되어 실제 장애 대응 가능.
5. 단계별 rollout/rollback 절차가 문서화되어 있다.

---

## 10. 실행용 TODO 문서 연결

본 리포트를 기반으로 한 실행 체크리스트는 다음 문서를 따른다.

- `tasks/todo.md`
