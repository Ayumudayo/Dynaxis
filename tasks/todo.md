# TODO - TCP/UDP 투트랙 전송 모델 변경 실행 계획

기준 문서: `docs/dual_transport_model_change_report.md`

## 0) 범위 확인

- [x] 목표 확정: "Two-track transport, one-track game logic"
- [x] 비목표 확정: 대규모 핸들러 리라이트/도메인 로직 재설계는 본 작업 범위에서 제외
- [x] 단계별 롤아웃/롤백 정책을 운영팀과 합의

완료 기준:

- [x] 범위/비범위가 문서와 팀 합의에 명시됨

---

## 1) Phase 0 - 정책 모델/스키마 고정

## 1.1 정책 스키마 정의

- [x] `SessionStatus`, `ProcessingPlace`, `TransportMask`, `DeliveryClass` 열거값 정의
- [x] 기본값 정책(legacy opcode 호환용) 정의
- [x] 정책 필드 유효성 규칙(조합 제한) 정의

## 1.2 opcode 분류표 작성

- [x] 시스템 opcode 분류(TCP-only/UDP-candidate/dual)
- [x] 게임 opcode 분류(TCP-only/UDP-candidate/dual)
- [x] 고위험 opcode(인증/권한/정합성) TCP-only 고정 목록 작성

## 1.3 문서 동기화

- [x] `docs/protocol/opcodes.md`에 정책 컬럼 초안 반영
- [x] 운영 문서(`docs/ops/observability.md`, `docs/ops/runbook.md`)에 변경 예정 지표/운영 포인트 추가

완료 기준:

- [x] 스키마와 분류표가 리뷰 승인됨

---

## 2) Phase 1 - 코드생성 확장 (동작 무변경)

## 2.1 프로토콜 소스 확장

- [x] `core/protocol/system_opcodes.json`에 정책 필드 추가
- [x] `server/protocol/game_opcodes.json`에 정책 필드 추가

## 2.2 생성기 확장

- [x] `tools/gen_opcodes.py`가 정책 필드를 읽어 코드 생성하도록 확장
- [x] 기본값 자동 주입 로직 추가(legacy 정의 호환)
- [x] 정책 스키마 검증 에러 메시지 정비

## 2.3 산출물/문서 생성

- [x] `core/include/server/core/protocol/system_opcodes.hpp` 재생성
- [x] `server/include/server/protocol/game_opcodes.hpp` 재생성
- [x] `tools/gen_opcode_docs.py` 반영 및 `docs/protocol/opcodes.md` 갱신

## 2.4 검증

- [x] opcode 생성 체크 명령 통과 (`python tools/gen_opcode_docs.py --check`)
- [x] 기존 빌드/테스트 회귀 없음 확인

완료 기준:

- [x] 런타임 동작 변화 없이 policy metadata만 추가됨

---

## 3) Phase 2 - 코어 정책 게이트 도입 (TCP only)

## 3.1 디스패치 게이트

- [x] `core/src/net/session.cpp` 또는 `core/src/net/dispatcher.cpp`에 정책 조회 경로 추가
- [x] `required_state` 검증 실패 시 일관된 에러 처리 적용
- [x] `processing_place` 적용 지점 정의 및 초기 적용(TCP 경로)

## 3.2 라우팅 계층 반영

- [x] `server/src/app/router.cpp` 등록 경로와 정책 메타 정합성 확인
- [x] 정책-핸들러 불일치 검증(시작 시점 assert/log) 추가

## 3.3 검증

- [x] 단위 테스트: 상태 제약 허용/거부 케이스
- [x] 통합 테스트: 로그인 전 금지 opcode 차단
- [x] 기존 chat 경로 회귀 없음 확인

완료 기준:

- [x] TCP-only 환경에서 정책 게이트가 안정적으로 동작

---

## 4) Phase 3 - UDP ingress 도입 (제한 롤아웃)

진행 메모 (2026-02-21):

- [x] gateway UDP listener 스켈레톤(`GATEWAY_UDP_LISTEN`) 추가 및 수신 카운터/에러 카운터 메트릭 노출
- [x] dispatcher transport-aware 진입점 추가(`TransportKind`) 및 policy.transport 게이트(TCP/UDP) 적용
- [x] `MSG_UDP_BIND_REQ/RES` + HMAC ticket 기반 TCP-auth 세션 <-> UDP endpoint 바인딩 구현
- [x] 바인딩된 UDP payload를 정책 검증 후 backend 경로로 전달하고, `UnreliableSequenced` 최소 replay/순서 가드 적용

## 4.1 전송 추상화

- [x] 코어 전송 인터페이스(`ITransportSession` 성격) 도입
- [x] 기존 TCP `Session`을 인터페이스 구현체로 연결
- 별도 `UdpSession` 구현체 항목은 현재 gateway `TransportSessionPtr` + bind 모델로 대체되어, 후속 설계 필요 시 재정의한다.

## 4.2 Gateway 연동

- [x] `gateway/src/gateway_app.cpp`에 UDP listener 경로 추가
- [x] TCP 인증 세션과 UDP 세션 바인딩 절차 구현
- [x] 바인딩 실패/만료/재시도 정책 구현(실패/만료/재시도 backoff 포함)

## 4.3 정책 기반 전송 분기

- [x] `TransportMask` + `DeliveryClass` 기반 송수신 분기 구현(UDP ingress -> policy 검사 -> backend 전달)
- [x] `UnreliableSequenced`용 seq window/replay guard 구현(최소 단조 증가 가드)
- [x] 초기 대상 opcode를 최소 세트로 제한

## 4.4 검증

- [x] 손실/중복/역순 시뮬레이션 테스트 통과
- [x] TCP fallback 정상 동작 확인
- 잔여 검증(혼합 트래픽 soak)은 `15.3`에서 단일 추적한다.

완료 기준:

- [x] 단위/회귀 범위에서 UDP 대상 opcode 처리와 TCP 회귀 무결성을 확인한다.
- 확장 검증(soak/운영)은 `15.3` 잔여 DoD에서 추적한다.

---

## 5) Phase 4 - 관측성/보안/운영 정착

## 5.1 메트릭/대시보드

- [x] UDP 품질 지표 추가(RTT/loss/jitter/reorder/dup/retransmit)
- [x] transport/delivery 레이블 기반 대시보드 갱신
- [x] 임계치 기반 알람 규칙 반영

## 5.2 보안/남용 방지

- [x] UDP 바인딩 토큰 서명/TTL 검증
- [x] rate limit 및 반복 실패 차단
- [x] replay/session hijack 방어 테스트

## 5.3 운영 문서

- [x] `docs/ops/observability.md` 갱신
- [x] `docs/ops/runbook.md`에 장애 대응 절차 추가
- [x] 배포/롤백 체크리스트(`docs/ops/deployment.md`) 갱신

완료 기준:

- [x] 운영팀이 투트랙 장애 대응을 문서만으로 수행 가능

---

## 6) CI/빌드/테스트 체계

- [x] CMake 옵션(UDP on/off, feature flag) 정비
- [x] `.github/workflows/ci.yml`에 schema/policy/test 단계 추가
- [x] 빠른 테스트와 느린 테스트를 분리해 파이프라인 최적화

완료 기준:

- [x] main 브랜치에서 지속적으로 안정적인 CI 결과 확보

---

## 7) 롤아웃/롤백 실행 체크리스트

## 7.1 롤아웃

- [x] canary 환경에서 UDP 대상 opcode 제한 오픈
- [x] 핵심 메트릭 안정화 확인 후 점진 확장
- [x] 이슈 발생 시 즉시 feature flag rollback

## 7.2 롤백

- [x] UDP ingress 즉시 차단 절차 검증
- [x] TCP-only 모드 복귀 시 데이터/세션 정합성 확인
- [x] 사후 분석 및 재시도 조건 문서화

완료 기준:

- [x] 10분 내 안전 롤백이 가능함을 리허설로 증명

---

## 8) 초기 DoD(달성)

- 주의: mixed soak/운영 검증/RUDP 확장 잔여는 `15.3`에서 후속 추적한다.

- [x] 모든 opcode에 정책 메타가 존재한다(기본값 포함).
- [x] TCP 기존 회귀 테스트가 모두 통과한다.
- [x] UDP 대상 opcode의 손실/중복/역순 테스트가 통과한다.
- [x] 메트릭/알람/런북이 운영 수준으로 준비된다.
- [x] 단계별 rollout/rollback이 실제 리허설로 검증된다.

---

## 9) Doxygen 전면 개선 TODO

진행 메모 (audit snapshot):

- non-generated 헤더 중 Doxygen 태그 전무: 6개
- 선언 인접 Doxygen 누락(휴리스틱): 55건
- 임계 런타임 `.cpp` 중 `@brief` 누락: 1개 (`server/src/app/core_internal_adapter.cpp`)

## 9.1 후속 거버넌스 결정 (별도 추적)

- 후속 결정: `docs/naming-conventions.md` 2.1 기준으로 "필수 대상"(public API 헤더 + 임계 런타임 `.cpp`) 확정
- 후속 결정: 생성 파일 경계 확정(직접 수정 금지):
  - `core/include/server/core/protocol/system_opcodes.hpp`
  - `server/include/server/protocol/game_opcodes.hpp`
  - `core/include/server/wire/codec.hpp`
- 후속 결정: private/internal 선언(예: `gateway_app.hpp` 내부 `SessionState`)의 필수 범위 제외 정책 명시

## 9.2 1차 블로커 해소 (파일 단위 태그 전무)

- [x] `core/include/server/core/api/version.hpp` Doxygen 추가
- [x] `core/include/server/core/protocol/opcode_policy.hpp` Doxygen 추가
- [x] `server/include/server/app/core_internal_adapter.hpp` Doxygen 추가
- [x] `server/include/server/config/runtime_settings.hpp` Doxygen 추가
- [x] `gateway/include/gateway/udp_bind_abuse_guard.hpp` Doxygen 추가
- [x] `gateway/include/gateway/udp_sequenced_metrics.hpp` Doxygen 추가
- [x] `server/src/app/core_internal_adapter.cpp` 파일/모듈 `@brief` 추가

## 9.3 2차 고우선 보강 (누락 건수 상위)

- [x] `server/include/server/chat/chat_service.hpp` 공개 타입/핵심 public 함수 문서화
- [x] `server/include/server/state/instance_registry.hpp` 인터페이스 계약(`@param/@return/@note`) 보강
- [x] `core/include/server/core/storage/repositories.hpp` 모델/필드 의미 문서화
- [x] `server/include/server/storage/redis/client.hpp` 옵션/계약 문서화
- [x] `core/include/server/core/concurrent/task_scheduler.hpp` 스케줄/시간 단위/스레드 계약 문서화

## 9.4 3차 잔여 누락 정리 (모듈별)

- [x] core 모듈 잔여 누락 정리 (`core/include/server/core/*`)
- [x] server 모듈 잔여 누락 정리 (`server/include/server/*`)
- [x] gateway 모듈 잔여 누락 정리 (`gateway/include/gateway/*`)
- [x] client_gui 모듈 잔여 누락 정리 (`client_gui/include/client/*`)

## 9.5 생성기 경로 개선 (generated 헤더 대응)

- [x] `tools/gen_opcodes.py`가 생성 헤더에 최소 Doxygen 헤더 블록을 출력하도록 확장
- [x] `tools/gen_wire_codec.py`가 생성 헤더에 최소 Doxygen 헤더 블록을 출력하도록 확장
- [x] 생성 산출물 재생성 후 `--check` 경로와 충돌 없는지 검증

## 9.6 자동 검증/회귀 방지

- [x] Doxygen 커버리지 점검 스크립트(`tools/check_doxygen_coverage.py`) 추가
- [x] CI에 문서화 검증 단계 추가(생성 파일 예외 규칙 포함)
- [x] 실패 리포트에 "파일/심볼/누락 태그"를 출력하도록 정비

완료 기준:

- [x] non-generated public API 헤더의 class/struct에 `@brief` 100%
- [x] public 함수(인자/반환 존재)의 `@param/@return` 100%
- [x] 임계 런타임 `.cpp`의 `@brief` 100%
- [x] generated 헤더는 generator 경로로 관리되며 커버리지 정책이 CI에 고정됨

---

## 10) 임시 문서 정리 작업 (2026-02-27)

목표:

- 루트 임시 문서를 제거하고, 참조 누락 없이 정리한다.

체크리스트:

- [x] 새 작업 브랜치 생성: `chore/temp-md-cleanup`
- [x] 임시 문서 참조 검색(문서/스크립트/테스트/워크플로우) 수행
- [x] 임시 문서 파일 제거
- [x] 제거 후 참조 재검색으로 깨진 링크/의존성 없음 확인
- [x] 작업 결과 요약(브랜치/변경 파일/검증 결과) 기록

리뷰:

- [x] 제거 후 참조 재검색으로 깨진 링크/의존성 없음 확인
- [x] 작업 결과 요약(브랜치/변경 파일/검증 결과) 기록
- `chore/temp-md-cleanup` 브랜치에서 루트 임시 문서 삭제를 완료했고, 코드/문서 참조 이슈는 발견되지 않았다.

---

## 11) Core RUDP 사전 구축 계획 (기본 OFF, 미래 활성화 대비)

목표:

- 현재 TCP 기본 경로/기존 UDP bind 경로를 유지하면서, `core`에 RUDP 엔진을 "미사용 상태"로 사전 탑재한다.
- 활성화는 빌드/런타임 플래그로만 허용하고, 실패 시 세션 단위 TCP fallback을 강제한다.

제약:

- 규칙: 기본값 OFF 유지 (`KNIGHTS_ENABLE_CORE_RUDP=OFF`, `GATEWAY_RUDP_ENABLE=0`)
- 규칙: 기존 `PacketHeader`(14 bytes) 앱 프레임 규약 유지 (`core/include/server/core/protocol/packet.hpp`)
- 규칙: 기존 UDP bind control plane 유지 (`MSG_UDP_BIND_REQ/RES`)
- 규칙: 계층 의존성 유지 (`core`는 `gateway/server` 구현에 의존하지 않음)
- 추적 원칙: 11.x의 미완 상세 항목은 `15.3` 잔여 DoD에 집계하며, 최종 상태 판단은 `15.3`를 단일 소스로 사용한다.

### 11.1 Phase 0 - 설계/계약 고정 (문서 우선)

- [x] `docs/protocol/rudp.md` 신규 작성 (패킷 외곽 헤더, handshake, ack, retransmit, fallback 규칙)
- [x] `docs/configuration.md`에 RUDP 플래그/튜닝 키 초안 추가
- [x] `docs/ops/observability.md`, `docs/ops/runbook.md`에 RUDP 관측/장애 대응 절차 추가
- [x] 기존 UDP bind 이후에만 RUDP handshake를 시작한다는 계약 명시

완료 기준:

- [x] 상태도(Idle/Hello/Established/Draining) + 타임아웃/RTO 정책 + 롤백 절차가 문서로 고정됨

### 11.2 Phase 1 - Core RUDP 엔진 스캐폴드 추가 (동작 무변경)

- [x] `core/include/server/core/net/rudp/` 헤더 스캐폴드 추가
- [x] `core/src/net/rudp/` 소스 스캐폴드 추가
- [x] 핵심 타입 초안 추가: `RudpPacket`, `RudpPeerState`, `AckWindow`, `RetransmissionQueue`, `RudpEngine`
- [ ] datagram I/O 추상 인터페이스 추가 (core 재사용 가능 구조)
- [x] `core/CMakeLists.txt`에 신규 파일 명시 등록
- [x] 루트 `CMakeLists.txt`에 `KNIGHTS_ENABLE_CORE_RUDP` 옵션 추가(기본 OFF)

완료 기준:

- [ ] 플래그 OFF일 때 기존 바이너리 동작/메트릭/테스트 결과가 변하지 않음

### 11.3 Phase 2 - 신뢰전송 로직 구현 (ACK/retransmit/window)

- [x] ACK 모델 구현: `ack_largest + ack_mask(64)`
- [x] RTO 기반 재전송(`srtt/rttvar/rto`, clamp) 구현
- [x] in-flight 상한(패킷/바이트) + delayed ACK 구현
- [x] 기본 MTU payload 1200B 적용 (파편화는 후속 단계)
- [x] RUDP runtime 메트릭 추가:
  - [x] `core_runtime_rudp_handshake_total{result}`
  - [x] `core_runtime_rudp_retransmit_total`
  - [x] `core_runtime_rudp_inflight_packets`
  - [x] `core_runtime_rudp_rtt_ms_*`
  - [x] `core_runtime_rudp_fallback_total{reason}`
- [x] 관련 파일 반영:
  - [x] `core/include/server/core/runtime_metrics.hpp`
  - [x] `core/src/runtime_metrics.cpp`
  - [x] `core/src/metrics/metrics.cpp`

완료 기준:

- [ ] 손실/중복/역순 시뮬레이션에서 reliable 채널 정확성(중복 없음/순서 보장) 통과

### 11.4 Phase 3 - Gateway 어댑터 연동 (기본 OFF 유지)

- [x] `gateway/src/gateway_app.cpp`에서 기존 UDP bind/control과 RUDP data path를 구분 처리
- [x] RUDP magic/version 패킷만 core `RudpEngine`에 전달
- [x] handshake 실패/품질 저하 시 세션 단위 TCP fallback 구현
- [x] 런타임 플래그/게이트 추가:
  - [x] `GATEWAY_RUDP_ENABLE=0`
  - [x] `GATEWAY_RUDP_CANARY_PERCENT=0`
  - [x] `GATEWAY_RUDP_OPCODE_ALLOWLIST=`(기본 빈값)

완료 기준:

- [ ] 플래그 OFF에서 기존 `gateway_udp_*` 동작/지표가 불변
- [ ] 플래그 ON canary 환경에서 fallback 경로가 검증됨

### 11.5 Phase 4 - 테스트/CI 확장

- [x] 신규 core 테스트 추가:
  - [x] `tests/core/test_rudp_ack_window.cpp`
  - [x] `tests/core/test_rudp_retransmit.cpp`
  - [x] `tests/core/test_rudp_handshake.cpp`
  - [x] `tests/core/test_rudp_flow_control.cpp`
  - [x] `tests/core/test_rudp_fallback.cpp`
- [x] 기존 회귀 테스트 보강:
  - [x] `tests/core/test_core_net.cpp` transport policy 회귀
  - [x] `tests/core/test_udp_sequenced_metrics.cpp` 연계 회귀
  - [x] `tests/core/test_udp_bind_abuse_guard.cpp` 연계 회귀
- [x] CI 분리:
  - [x] 기본 파이프라인: RUDP OFF 회귀
  - [x] 별도 파이프라인: impairment(loss/reorder/jitter) 시나리오

완료 기준:

- [x] OFF 회귀 0건 + ON 시나리오 안정성 확보

### 11.6 Phase 5 - 운영 준비 (아직 미사용 유지)

- [x] 경보 룰 추가 (retransmit ratio/handshake fail/fallback rate)
- [x] `docs/ops/fallback-and-alerts.md`에 RUDP 알람/임계치 반영
- [x] `docs/ops/runbook.md`에 RUDP 장애 triage 절차 반영
- [x] 롤백 리허설 절차 고정:
  - [x] `GATEWAY_RUDP_CANARY_PERCENT=0`
  - [x] `GATEWAY_RUDP_ENABLE=0`
  - [x] TCP KPI 정상 복귀 확인

완료 기준:

- [x] 10분 내 안전 롤백 리허설을 재현 가능하게 문서/스크립트로 고정

## 11.7 검증 매트릭스

- [x] 정적 검증: `lsp_diagnostics` 에러 0 (변경 파일 전부)
- [x] 빌드 검증: `core_general_tests`, `server_general_tests`, `gateway_app` 빌드 성공
- [x] 테스트 검증(단위/회귀): core RUDP 단위 + 기존 TCP 회귀 테스트 통과
- 잔여 검증(장시간 soak + 운영 canary/kill-switch)은 `15.3`에서 단일 추적한다.

## 11.8 리스크/완화

- 후속 리스크 항목(NAT rebinding/MTU/메모리/보안)은 `15.3` 마지막 항목에서 단일 추적한다.

---

## 12) CI 의존성 캐시 최적화 계획 (vcpkg 중심)

목표:

- vcpkg baseline이 유지되는 동안 의존성 재빌드/재다운로드를 최소화해 CI wall-clock을 추가 단축한다.
- 코드 컴파일 산출물 재사용이 아닌, **의존성 빌드 재사용 품질**(hit rate, restore latency, miss 원인)을 운영 가능 수준으로 만든다.

### 12.1 Phase A - 즉시 적용(저리스크)

- [x] 캐시 키 입력 최소화 설계안 작성 (`vcpkg.json`, toolchain 영향 파일만 포함)
- [x] `ci.yml`에 cache hit/miss 및 restore/저장 시간 요약 출력 추가
- [x] 최근 10회 PR run 기준 baseline 수집(평균/중앙값/분산)
- [x] 캐시 miss 분류표 작성(키 변경, runner 변동, 압축/복원 실패, 다운로드 실패)

완료 기준:

- [x] "왜 miss가 났는지"를 run 단위로 설명 가능한 상태
- [x] baseline 보고서(전/후 비교 템플릿) 확정

### 12.2 Phase B - 중기 개선(효과 큼)

- [x] vcpkg 캐시 prewarm 워크플로우(수동/스케줄) 초안 작성
- [ ] prewarm이 PR CI hit rate에 미치는 영향 측정(최소 1주)
- [x] `sccache` PoC 브랜치 구성(Windows 우선, 실패 시 즉시 롤백)
- [ ] `sccache` 적용 전/후 compile 단계 시간 비교

완료 기준:

- [ ] hit rate 개선 또는 근거 기반 폐기 결정
- [ ] `sccache` 채택/비채택 결론 문서화

### 12.3 Phase C - 장기 전략(도구 전환 판단)

- [ ] Conan2 + binary remote 비교 실험 설계(동일 의존성 셋)
- [ ] vcpkg 대비 성능/운영 복잡도/안정성 점수표 작성
- [ ] 전환 조건(Go/No-Go) 명문화

완료 기준:

- [ ] 도구 전환 여부를 합의 가능한 근거로 결정

### 12.4 예상 효과

- [x] Phase A 완료 시: 재현 가능한 관측체계 확보 + 추가 5~10% 단축 여지 확인
- [ ] Phase B 완료 시: 반복 PR에서 추가 10~25% 단축 가능성 검증
- [ ] Phase C 완료 시: 장기 비용(시간/운영) 최적화 의사결정 완료

---

## 13) 기반 레이어 개선 계획 (Kawari 비교 반영)

목표:

- `core/gateway/LB`에 한정해 전송 경계 타입화 -> LB 프로파일 표준화 -> resilience 공용화 순으로 적용한다.
- 기존 chat 도메인 로직은 변경하지 않는다.

### 13.1 P0 - 전송 경계 타입화 (우선순위 1)

- [x] `core/include/server/core/protocol/packet.hpp`에 연결/세그먼트 경계 타입(enum/분류 helper) 도입
- [x] `gateway/src/gateway_connection.cpp` handshake 경로에서 경계 타입을 사용하도록 반영
- [x] 회귀 테스트/빌드 검증 (`test_core_net`, `gateway_app`)

완료 기준:

- [x] 기존 동작 불변 + 경계 타입이 코드상 명시됨

### 13.2 P1 - LB 프로파일 표준화 (우선순위 2)

- [x] `docker/stack/docker-compose.yml`에서 HAProxy dev/prod 프로파일 선택 경로 명시
- [x] `docker/stack/haproxy/haproxy.cfg`(dev) / `docker/stack/haproxy/haproxy.tls13.cfg`(prod) 운영 계약 동기화
- [x] `docs/ops/gateway-and-lb.md` 업데이트

완료 기준:

- [x] 환경별 프로파일 전환으로 안전하게 롤백 가능

### 13.3 P2 - resilience 공용화 (우선순위 3)

- [x] `gateway/include/gateway/resilience_controls.hpp`의 핵심 primitive를 `core` 공용 유틸로 승격
- [x] gateway는 wrapper/alias로 하위 호환 유지
- [x] send queue 보호 경로(`core/src/net/session.cpp`, `gateway/src/gateway_app.cpp`) 정합성 통일

완료 기준:

- [x] 기존 gateway 회복탄력성 동작과 메트릭이 유지됨

### 13.4 검증 리뷰 (2026-03-02)

- [x] Local CI(`act`) 재검증: `core-api-consumer-linux` 재실행 2회
  - 결과: 둘 다 실패
  - 공통 원인: `python3: can't open file '/app/tools/check_core_api_contracts.py': [Errno 2] No such file or directory`
  - 시도한 우회:
    - `act pull_request -b -W .github/workflows/ci.yml -j core-api-consumer-linux`
    - `act pull_request -b --env GITHUB_WORKSPACE=/run/desktop/mnt/host/e/Repos/MyRepos/Knights -W .github/workflows/ci.yml -j core-api-consumer-linux`
- [x] Docker stack 정리 완료
  - 실행: `pwsh -NoProfile -ExecutionPolicy Bypass -File scripts/deploy_docker.ps1 -Action down`
  - 검증: `docker compose --project-name knights-stack --project-directory docker/stack -f docker/stack/docker-compose.yml ps` 출력 헤더만 존재(서비스 없음)

---

## 14) 런타임 확장성 TODO 후속 작업 (2026-03-05)

기준 문서: `tasks/runtime-extensibility-todo.md`

- [x] 미체크 항목을 코드 기준으로 재분류(실제 미구현 vs 체크 누락)
- [x] Phase 4 핵심 미체크 항목 우선 구현
- [x] hook 호출 시간 측정 추가
- [x] 시간 예산 초과 경고 로그 추가
- [x] hook duration histogram 메트릭(`plugin_hook_duration_seconds`) 추가
- [x] 관련 테스트 보강 및 TODO 체크 상태 반영
- [x] 빌드/테스트/LSP 진단으로 회귀 검증

리뷰:

- [x] 변경 파일/검증 결과/남은 리스크 기록

- 진행 결과 보강(2026-03-05, 후속): `docker/observability/grafana/dashboards/server-metrics.json`에 Extensibility row를 추가해 reload 성공률, hook 호출 빈도, 에러율, Lua 메모리 사용량, 자동 비활성화 이벤트 패널을 반영했다.
- 검증: `python -m json.tool docker/observability/grafana/dashboards/server-metrics.json` 파싱 성공, 관련 TODO(`tasks/runtime-extensibility-todo.md` 4.5) 체크 반영.
- 남은 리스크: 대시보드 쿼리는 메트릭 존재 전제이므로, 운영 환경에서 플러그인/스크립팅 비활성 상태에서는 일부 패널이 No data로 보일 수 있다(정상).
- 진행 결과 보강(2026-03-05, 후속2): `tests/server/test_hook_auto_disable.cpp`에 Lua instruction/memory limit 실패 시 관리자 경로 정상 지속 검증을 추가했고, auto-disable 임계 검증을 3회 연속 실패 기준으로 상향했다.
- 검증: `ctest --preset windows-test -R "HookAutoDisableTest|LuaHookIntegrationTest|ChatServiceTest\.LuaColdHookDenySkipsAdminRuntimeSettingReload" --output-on-failure` (6/6 pass), `ctest --preset windows-test -R "ChatPluginChainV2Test|ChatPluginChainTest" --output-on-failure` (16/16 pass).
- 검증(추가): `build-windows/tests/Debug/server_general_tests.exe --gtest_filter="HookAutoDisableTest..."` 직접 실행으로 Lua 관련 3개 테스트가 SKIP 없이 PASS됨을 확인했고, `server_app` 기동 후 `/metrics`에서 `chat_frame_total`, `hook_auto_disable_total`, `lua_script_calls_total`, `lua_memory_used_bytes` 노출을 확인했다.
- 체크리스트 반영: `tasks/runtime-extensibility-todo.md`의 4.6 항목 갱신(현재 scaffold limit 시뮬레이션 기준), 메트릭 관측성 항목 갱신, 실 Lua VM 악성 스크립트 내결함성 DoD는 보수적으로 미완(unchecked) 유지.
- 진행 결과 보강(2026-03-05, 후속3): `docs/extensibility/`에 `plugin-quickstart.md`, `lua-quickstart.md`, `conflict-policy.md`, `recipes.md`를 신규 추가하고, Phase 7 문서 항목(490-493, 580-584)을 반영했다.
- 정합성 보정: Oracle 리뷰 지적을 반영해 ABI v2 스켈레톤(`on_chat_send` 필수, `LoginEventV2::user`), fallback 롤백 안내, Lua scaffold 범위, conflict policy의 "운영 규약 vs Phase 7 강제" 경계를 문서에 명시했다.
- 문서 동기화 보강: `docs/runtime-extensibility-plan.md`에 2026-03-05 진행 반영 섹션을 추가하고, 해당 TODO(484)를 체크했다.
- 문서 동기화 보강(추가): `docs/core-api/compatibility-policy.md`에 Chat hook ABI(v1/v2) 호환 규칙을 추가하고, `docs/core-api-boundary.md`에 `core/plugin/*`, `core/scripting/*` 안정성(Transitional) 분류를 반영했다. 관련 TODO(486, 487) 체크 완료.
- Oracle 후속 보정: `docs/core-api/extensions.md`에 `on_chat_send` 필수 validator 계약을 명시하고, `docs/core-api/compatibility-policy.md`에 transitional 거버넌스 범위/심볼 미존재 시점 폴백 semantics를 명시했다.
- 진행 결과 보강(2026-03-05, 후속5): `server/AGENTS.md`, `core/AGENTS.md`, 루트 `AGENTS.md`에 runtime extensibility 최신 모듈/흐름/탐색 경로를 반영했고, `tasks/runtime-extensibility-todo.md`의 문서 동기화 항목(494-496, 500)을 체크 완료했다.
- 진행 결과 보강(2026-03-05, 후속6): Phase 8.6 템플릿 4종(`server/plugins/templates/chat_hook_v2_template.cpp`, `server/scripts/templates/on_join_template.lua`, `server/plugins/templates/plugin_manifest.template.json`, `server/scripts/templates/script_manifest.template.json`)과 스캐폴드 도구 2종(`tools/new_plugin.py`, `tools/new_script.py`)을 추가했다.
- 검증: `python -m py_compile tools/new_plugin.py tools/new_script.py`, 각 `--help` 출력 확인, 그리고 임시 출력 경로(`build-windows/runtime_ext_scaffold_smoke`)에서 플러그인/스크립트 생성 스모크를 수행한 뒤 생성 경로 확인 후 정리했다. 관련 TODO(572-575, 577-578, 595) 체크 완료.
- 진행 결과 보강(2026-03-05, 후속7): `tools/ext_inventory.py`를 추가해 `plugins/`/`scripts/` manifest 인벤토리 스캔과 스키마 검증을 구현했다(공통 필드 + 선택 필드 검증, kind/hook_scope/stage/priority/checksum/compat 규칙 포함).
- 검증: `python -m py_compile tools/ext_inventory.py`, `python tools/ext_inventory.py --manifest server/plugins/templates/plugin_manifest.template.json --manifest server/scripts/templates/script_manifest.template.json --allow-missing-artifact --check --json`(error_count=0), `python tools/ext_inventory.py --plugins-dir server/plugins --scripts-dir server/scripts --check` 실행. 관련 TODO(529-533) 체크 완료.
- 진행 결과 보강(2026-03-05, 후속8): `docs/extensibility/control-plane-api.md`를 추가해 Phase 8.1 제어면 설계(API 4종, deployment DTO, selector/rollout 모델, 상태 모델/전이 규칙)를 문서화했고, 관련 TODO(514-525)를 체크 완료했다.
- 진행 결과 보강(2026-03-05, 후속9): `tools/ext_inventory.py`에 인벤토리 fingerprint 캐시 + polling watcher(`--watch-interval-ms`, `--watch-iterations`)를 추가해 변경 감지 시 재검증/재출력을 지원했다. 관련 TODO(534) 체크 완료.
- 진행 결과 보강(2026-03-05, 후속10): `SERVER_ROLE`, `SERVER_REGION`, `SERVER_SHARD`, `SERVER_TAGS` 환경 변수를 `ServerConfig`/registry 경로에 반영해 서버 메타데이터 표준화를 진행했다. `InstanceRecord` 직렬화/역직렬화는 `region`, `shard`, `tags`(pipe-delimited) 필드를 지원하도록 확장했다.
- 검증(후속10): `pwsh scripts/build.ps1 -Config Debug -Target server_state_tests`, `pwsh scripts/build.ps1 -Config Debug -Target server_general_tests`, `ctest --preset windows-test -R "ServerConfigTest|InMemoryStateBackendTests|RedisInstanceStateBackendTests|InstanceRegistryJsonTests|ConsulInstanceStateBackendTests" --output-on-failure` 실행 결과 9/9 pass.
- 진행 결과 보강(2026-03-05, 후속11): `server/state/instance_registry`에 제어면 selector resolver 유틸(`InstanceSelector`, `matches_selector`, `select_instances`, `SelectorMatchStats`)을 추가해 `all/server_ids/roles/regions/shards/tags` 필터링을 구현했다.
- 검증(후속11): `lsp_diagnostics`(변경 3파일) 에러 0, `pwsh scripts/build.ps1 -Config Debug -Target server_state_tests`, `ctest --preset windows-test -R "InstanceRegistryJsonTests|InstanceSelectorTests|InMemoryStateBackendTests|RedisInstanceStateBackendTests|ConsulInstanceStateBackendTests" --output-on-failure` 실행 결과 12/12 pass.
- 진행 결과 보강(2026-03-05, 후속12): `tools/admin_app/main.cpp`의 `GET /api/v1/instances`에 selector query(`all`, `server_ids`, `roles`, `regions`, `shards`, `tags`) 파서를 연동해 서버 목록 필터링을 적용했고, `admin_instances_selector_requests_total`, `admin_instances_selector_mismatch_total` 메트릭을 추가했다. 인스턴스/상세 JSON에도 `region`, `shard`, `tags`를 노출하도록 확장했다.
- 검증(후속12): `lsp_diagnostics tools/admin_app/main.cpp` 에러 0, `pwsh scripts/build.ps1 -Config Debug -Target admin_app` 빌드 성공, `ctest --preset windows-test -R "InstanceSelectorTests|InstanceRegistryJsonTests" --output-on-failure` 8/8 pass.

---

## 15) tasks 폴더 통합 실행 계획 (2026-03-05)

### 15.1 백로그 정합성 정리 (선행)

- [x] `tasks/next-plan.md`와 `tasks/core-engine-full-todo.md`를 대조해 완료 중복 항목 체크 상태를 동기화한다.
- [x] `tasks/runtime-extensibility-todo.md` 3.1/3.3 미체크 항목을 코드 기준으로 재검증해 "실제 미구현 vs 체크 누락"을 재분류한다.
- [x] `tasks/todo.md` 11.x (RUDP)에서 하위 완료 항목 대비 상위 미체크 항목을 정리한다.
- [x] 정합성 정리 결과를 `tasks/todo.md` 리뷰 섹션에 기록한다.

리뷰 (2026-03-05, 15.1 정합성 보정):

- `tasks/next-plan.md`의 Phase A-D/Acceptance 체크 상태를 `tasks/core-engine-full-todo.md` 완료 상태와 동기화했다.
- `tasks/runtime-extensibility-todo.md` 3.1/3.3은 코드 재검증 결과를 반영해 "체크 누락"과 "실제 미구현"을 분리했다:
  - 체크 누락: `BUILD_LUA_SCRIPTING` 옵션, `chat_lua_bindings` 파일 생성
  - 실제 미구현: LuaJIT/Sol2 의존성 연결, VM 수준 sandbox 제한(`lua_sethook`/allocator)
- `tasks/todo.md` 11.x는 하위 항목이 모두 완료된 상위 체크박스(메트릭/파일반영/플래그/테스트/CI)를 완료로 정리했다.

### 15.2 Priority P0 - Runtime Extensibility DoD 마감

- [x] 8.3 서버별 정책 계층(`global -> game_mode -> region -> shard -> server`) 규칙을 구현/문서화한다.
- [x] 타겟 불일치 서버 명령 무시 + mismatch 메트릭 기록을 구현한다.
- [x] 8.4 예약 교체(`run_at_utc` 저장소, UTC scheduler, idempotency, canary/wave, 실패 중단/롤백)를 구현한다.
- [x] 8.5 충돌 precheck 정책(stage/exclusive_group/priority/terminal decision)을 구현한다.
- [x] 8.7 관리 콘솔 E2E(단일/그룹/전체/예약/canary 실패)를 완료한다.
- [x] 4.x/10 DoD의 "악성 스크립트/플러그인 내결함성" 검증 시나리오를 추가하고 완료 처리한다.

진행 메모 (2026-03-05, 15.2 후속1):

- `InstanceRecord`/`InstanceSelector`에 `game_mode`/`game_modes`를 추가하고, selector 계층 분류 유틸(`classify_selector_policy_layer`, `selector_policy_layer_name`)로 `global -> game_mode -> region -> shard -> server` 우선순위를 코드에 고정했다.
- `server_app` 설정에 `SERVER_GAME_MODE`를 추가해 registry heartbeat 경로까지 반영했다.
- `admin_app` 인스턴스 API에 `game_modes` selector 파라미터와 `selector.layer`, `instance.game_mode` 노출을 추가했다.
- 검증: `pwsh scripts/build.ps1 -Config Debug -Target server_state_tests`, `pwsh scripts/build.ps1 -Config Debug -Target admin_app`, `ctest --preset windows-test -R "InstanceSelectorTests|InstanceRegistryJsonTests|ServerConfigTest" --output-on-failure` (11/11 pass).

진행 메모 (2026-03-05, 15.2 후속2):

- `server/src/app/bootstrap.cpp` admin fanout 수신 경로에 selector(`all/server_ids/roles/game_modes/regions/shards/tags`) 파싱을 추가하고, 현재 서버 메타(`SERVER_INSTANCE_ID/ROLE/GAME_MODE/REGION/SHARD/TAGS`)와 불일치 시 명령을 즉시 무시하도록 적용했다.
- 불일치 관측 지표 `chat_admin_command_target_mismatch_total`를 추가해 `/metrics`로 노출했다(`server/src/app/metrics_server.cpp`).
- `tools/admin_app/main.cpp` 쓰기 엔드포인트(`disconnect/announce/settings/moderation`)가 동일 selector query를 fanout payload에 포함하도록 확장하고, 응답 JSON에 `selector.applied/layer`를 추가했다.
- 검증: `lsp_diagnostics`(변경 3파일) 에러 0, `pwsh scripts/build.ps1 -Config Debug -Target server_app`, `pwsh scripts/build.ps1 -Config Debug -Target admin_app` 성공, `ctest --preset windows-test -R "Admin|InstanceSelectorTests|InstanceRegistryJsonTests|ServerConfigTest|ServerChat" --output-on-failure` 23/23 pass.

진행 메모 (2026-03-05, 15.2 후속3):

- `tools/admin_app/main.cpp`에 `/api/v1/ext/*` 제어면 엔드포인트를 추가했다:
  - `GET /api/v1/ext/inventory`
  - `POST /api/v1/ext/precheck`
  - `GET/POST /api/v1/ext/deployments`
  - `POST /api/v1/ext/schedules`
- 8.4 구현: `run_at_utc` 포함 배포 저장소(JSON 파일) + poll 기반 UTC scheduler + `command_id` 중복 거부 + `canary_wave` 기본 wave(`5,25,100`) + wave 실패 시 중단 및 `rollback_on_failure` 옵션 반영.
- 8.5 구현: manifest stage 검증(`pre_validate/mutate/authorize/side_effect/observe`), `(hook_scope, stage, exclusive_group)` 충돌 precheck 차단, terminal decision precedence 검증(`block/deny > handled > modify > pass`), observe stage의 상태 변경 결정 차단, precheck 실패 시 `409 PRECHECK_FAILED` 상세 이슈 반환.
- 관측성: `admin_ext_*` 메트릭 시리즈(요청/precheck fail/command_id 충돌/scheduler/wave/rollback/store read-write/inventory error)를 `/metrics`에 노출.
- 문서 동기화: `tools/admin_app/README.md`에 ext 엔드포인트 및 `ADMIN_EXT_SCHEDULE_STORE_PATH`, `ADMIN_EXT_MAX_CLOCK_SKEW_MS` 환경 변수를 반영.
- 검증: `lsp_diagnostics`(main.cpp) 에러 0, `pwsh scripts/build.ps1 -Config Debug -Target admin_app`, `pwsh scripts/build.ps1 -Config Debug -Target server_app` 성공, `ctest --preset windows-test -R "Admin|InstanceSelectorTests|InstanceRegistryJsonTests|ServerConfigTest|ServerChat" --output-on-failure` 23/23 pass.

진행 메모 (2026-03-05, 15.2 후속4):

- Docker stack에서 `admin-app`에 `./scripts`, `./plugins` 볼륨과 ext 경로 env를 연결해 `/api/v1/ext/inventory`가 실 artifact를 반환하도록 고정했다.
- `tests/python/verify_admin_api.py`를 확장해 ext selector 배포 E2E를 `server_ids`(단일), `roles`/`shards`(그룹), `all`(전체)로 검증하고, `run_at_utc` 예약 + precheck 차단(`409 PRECHECK_FAILED`)도 함께 확인했다.
- `tasks/admin_e2e.env`에 `ADMIN_EXT_FORCE_FAIL_WAVE_INDEX=2`를 추가하고, 동일 스크립트를 해당 env로 실행해 canary 실패(`status_reason=wave_forced_failure`)와 rollback 카운터(`admin_ext_rollbacks_total`) 증가를 검증했다.
- 검증: `python -m py_compile tests/python/verify_admin_api.py`, `python tests/python/verify_admin_api.py`, `pwsh -NoProfile -Command '$env:ADMIN_EXT_FORCE_FAIL_WAVE_INDEX="2"; python "tests/python/verify_admin_api.py"'`, `python tests/python/verify_admin_control_plane_e2e.py` 모두 PASS.

진행 메모 (2026-03-05, 15.2 후속5):

- `tests/server/test_server_chat.cpp`에 `ChatServiceTest.ThrowingChatHookExceptionDoesNotStopChatOrAdminSettingPath`를 추가해 `on_chat_send` 예외 이후에도 채팅 브로드캐스트와 admin runtime setting 적용이 계속 성공함을 서비스 레벨에서 검증했다.
- `tests/CMakeLists.txt`에서 `server_general_tests`에 `TEST_CHAT_HOOK_THROWING_PATH`와 `test_chat_hook_throwing` 의존성을 연결해 해당 회귀를 항상 실행 경로에 포함했다.
- 검증: `pwsh scripts/build.ps1 -Config Debug -Target server_general_tests` 성공, `ctest --preset windows-test -R "ChatServiceTest\.ThrowingChatHookExceptionDoesNotStopChatOrAdminSettingPath|ChatPluginChainV2Test\.SwallowsPluginExceptionAndContinuesDefaultPath|HookAutoDisableTest" --output-on-failure` 5/5 PASS.

진행 메모 (2026-03-05, 15.2 후속6):

- 온보딩 실증: `tools/new_script.py`로 `onboarding_smoke` artifact를 생성하고(`docker/stack/scripts`), `tools/ext_inventory.py --check` 통과 후 제어면 배포(`/api/v1/ext/deployments`)가 `completed`까지 도달함을 확인했다.
- 회귀 게이트: `pwsh scripts/build.ps1 -Config Debug -Target server_general_tests`, `pwsh scripts/build.ps1 -Config Release`, `ctest --preset windows-test --output-on-failure`를 실행해 `0 failed / 218`(StorageBasic 1건 skip) 상태를 확인했다.
- 보조 검증: `python tools/gen_opcode_docs.py --check`, `python tools/check_core_api_contracts.py --check-boundary`, `python tests/python/verify_admin_api.py`, `python tests/python/verify_admin_control_plane_e2e.py` 모두 통과했다.
- 문서 동기화: `docs/extensibility/lua-quickstart.md`에 제어면 기반 신규 artifact 생성/검증/배포 smoke 절차를 추가해 Runtime Extensibility 최종 DoD 문구와 실행 절차를 일치시켰다.

### 15.3 Priority P1 - Transport 안정화 마무리

- [x] Phase 3 미완(전송 인터페이스 추상화 + TCP/UDP 구현체 정리)을 완료한다.
- [x] UDP bind 재시도 정책을 정교화하고 초기 UDP opcode allowlist를 고정한다.
- [x] 손실/중복/역순 테스트와 TCP fallback 검증을 완료한다.
- [ ] mixed traffic(TCP+UDP) 장시간 soak 테스트를 완료한다.
- [ ] RUDP 11.x 미완 항목(OFF 불변성, ON canary fallback, 운영 검증)을 완료한다.
- [ ] NAT rebinding/MTU/메모리/보안 강화 항목은 설계 문서 + 후속 이슈로 분리해 추적한다.
- 상세 연계: RUDP 미완 세부 체크는 `11.x`를 참고하고, 상태 집계/보고는 본 섹션(`15.3`) 기준으로 유지한다.

진행 메모 (2026-03-05, 15.3 후속1):

- `gateway/include/gateway/gateway_app.hpp`에 전송 추상화 인터페이스(`GatewayApp::ITransportSession`)를 도입하고, 기존 `BackendConnection`이 해당 인터페이스 구현체가 되도록 정리했다.
- `gateway/src/gateway_app.cpp`의 세션 저장 타입을 `BackendConnectionPtr`에서 `TransportSessionPtr` 기반으로 전환해 UDP/RUDP ingress 경로가 concrete 타입이 아닌 전송 인터페이스를 통해 backend forward를 수행하도록 정리했다.
- 테스트 보강: `tests/core/test_core_net.cpp`에 `GatewayTransportAbstractionTest.BackendConnectionImplementsTransportSession`, `DispatcherTest.AllowsHandlerWhenTransportAllowsBothTcpAndUdp`를 추가했고, `tests/core/test_udp_bind_abuse_guard.cpp`에 endpoint별 차단 독립성 회귀(`BlockProgressIsTrackedIndependentlyPerEndpoint`)를 추가했다.
- 정적/회귀 검증: `lsp_diagnostics`(변경 4파일) 에러 0, `pwsh scripts/build.ps1 -Config Debug -Target gateway_app`, `pwsh scripts/build.ps1 -Config Debug -Target core_general_tests`, `pwsh scripts/build.ps1 -Config Debug -Target server_general_tests`, `ctest --preset windows-test -R "GatewayTransportAbstractionTest\.BackendConnectionImplementsTransportSession|DispatcherTest\.AllowsHandlerWhenTransportAllowsBothTcpAndUdp|UdpBindAbuseGuardTest\.BlockProgressIsTrackedIndependentlyPerEndpoint" --output-on-failure`(3/3 pass), `pwsh scripts/build.ps1 -Config Release`, `ctest --preset windows-test --output-on-failure`(0 failed/221, StorageBasic 1 skipped), `python tools/gen_opcode_docs.py --check`, `python tools/check_core_api_contracts.py --check-boundary`를 통과했다.

진행 메모 (2026-03-05, 15.3 후속2):

- `tests/core/test_rudp_rollout_policy.cpp`에 UDP allowlist 회귀 3건을 추가해 `parse_udp_opcode_allowlist`의 동작을 고정했다.
  - `ParseUdpAllowlistMatchesRudpParserSemantics`
  - `ParseUdpAllowlistEmptyInputReturnsEmptySet`
  - `OpcodeAllowedRejectsWhenAllowlistIsEmpty`
- out-of-range 토큰(`65536`)이 우회 래핑(`0`)으로 들어오지 않도록 size(3) + `opcode 0` 미포함 assert를 추가해 파서 회귀를 강화했다.
- 정적/회귀 검증: `lsp_diagnostics tests/core/test_rudp_rollout_policy.cpp` 에러 0, `pwsh scripts/build.ps1 -Config Debug -Target core_general_tests`, `pwsh scripts/build.ps1 -Config Release -Target core_general_tests`, `ctest --preset windows-test --output-on-failure`(0 failed/224, StorageBasic 1 skipped), `python tools/gen_opcode_docs.py --check`, `python tools/check_core_api_contracts.py --check-boundary`를 통과했다.

진행 메모 (2026-03-05, 15.3 후속3):

- 손실/중복/역순 + fallback 관련 회귀를 집중 실행해 현재 단위 레벨 품질 게이트를 재확인했다.
  - 실행: `ctest --preset windows-test -R "UdpSequencedMetricsTest|RudpFallbackTest|RudpHandshakeTest|RudpEngineTest" --output-on-failure`
  - 결과: 15/15 pass
- 결론: Phase 3/11 계열 단위 테스트 범위에서는 UDP sequenced quality와 RUDP fallback 경로가 정상 동작하며, 15.3 잔여 항목은 mixed traffic 장시간 soak + 운영 검증으로 축소된다.

### 15.5 공통 검증 게이트

- 실행 규칙: 단계별 변경마다 `pwsh scripts/build.ps1 -Config Debug -Target server_general_tests` 빌드 성공을 확인한다.
- 실행 규칙: `ctest --preset windows-test --output-on-failure` 회귀 0건을 확인한다.
- 실행 규칙: `python tools/gen_opcode_docs.py --check` 및 `python tools/check_core_api_contracts.py --check-boundary`를 통과한다.
- 기록 규칙: 변경된 문서/운영 가이드와 TODO 체크 상태를 같은 커밋에서 동기화한다.

### 15.6 권장 실행 순서 (완료 이력)

- 완료(Week 1): 15.1 + 15.2(8.3/8.5 핵심 구현)
- 완료(Week 2): 15.2(8.4/8.7 E2E) + 15.3(transport 안정화 선행분)
- 진행 중(Week 3 성격): 12.2/12.3의 CI cache 결론 + 15.3 잔여 DoD(mixed soak/운영 검증)

### 15.7 Runtime Extensibility Phase 16 Backlog 마감 (2026-03-06)

기준 문서:

- `docs/runtime-extensibility-plan.md:126`
- `tasks/runtime-extensibility-todo.md` 0.1

목표:

- Lua host API를 placeholder가 아닌 실제 읽기/액션/log/meta 경로로 연결한다.
- cold hook 실행과 reload를 동일한 serialized executor 규칙으로 정리한다.
- directive 시뮬레이션이 아닌 실제 Lua VM instruction/memory limit를 적용한다.
- Linux/Docker Lua capability와 OFF regression, function-style sample/docs를 같은 모델로 마감한다.

체크리스트:

- [x] Stream A - Lua host API 실체화
  - [x] `core/include/server/core/scripting/lua_runtime.hpp` / `core/src/scripting/lua_runtime.cpp` host API 시그니처를 인자/반환이 가능한 형태로 확장
  - [x] `server/src/scripting/chat_lua_bindings.cpp` placeholder lambda 제거
  - [x] read-only API를 `ChatService` 실제 상태 조회로 연결
  - [x] action/log/meta API를 실제 job queue/log/context 경로로 연결
  - [x] function-style sample script 최소 1개를 실제 host API 호출 기반으로 전환
  - [x] `tests/server/test_chat_lua_bindings.cpp`, `tests/server/test_lua_hook_integration.cpp`, 관련 `server_general_tests` 회귀 보강
- [x] Stream B - Lua 실행 스레드 경계 마감
  - [x] cold hook 호출을 reload와 동일한 strand/executor로 직렬화
  - [x] `server/src/chat/chat_service_core.cpp` 직접 `lua_runtime_->call_all()` 경로 제거
  - [x] action API non-blocking contract를 테스트/문서로 고정
  - [x] hot path(`on_chat_send`) Lua 차단 회귀 유지
- [x] Stream C - 실 sandbox enforcement
  - [x] `core/src/scripting/lua_sandbox.cpp`에 safe environment/runtime helper 추가
  - [x] 실제 VM instruction limit(`lua_sethook`) 적용
  - [x] 실제 allocator 기반 memory limit 적용
  - [x] directive-only limit 시뮬레이션 테스트를 실제 limit failure 검증으로 교체/보강
- [x] Stream D - Linux/Docker capability 마감
  - [x] Linux `BUILD_LUA_SCRIPTING=OFF` preset 또는 동등 경로 추가
  - [x] CI/Linux 경로에 ON/OFF source selection regression 반영
  - [x] Docker/runtime path에서 Lua sample/capability 검증 정리
- [x] Stream E - 문서/샘플 정합화
  - [x] `server/scripts` / `docker/stack/scripts` sample 드리프트 정리
  - [x] `server/README.md`, `docs/extensibility/lua-quickstart.md`, `docs/extensibility/recipes.md`, `docs/runtime-extensibility-plan.md`를 function-style hook 우선 모델로 정리
  - [x] directive/return-table은 fallback/testing aid로 위치 하향
  - [x] plugin 쪽 완료, script 쪽 운영 마감 중 상태를 명시

검증 게이트:

- [ ] 변경 파일 `lsp_diagnostics` 에러 0 (`lsp_diagnostics` 명령이 현재 로컬 셸에 없어 미실행)
- [x] `pwsh scripts/build.ps1 -Config Debug -Target server_general_tests`
- [x] `pwsh scripts/build.ps1 -Config Debug -Target core_plugin_runtime_tests`
- [x] `ctest --preset windows-test -R "LuaRuntimeTest|LuaSandboxTest|ChatLuaBindingsTest|LuaHookIntegrationTest|HookAutoDisableTest|ChatServiceTest\\.Lua" --output-on-failure`
- [x] `python tools/check_lua_build_toggle.py --build-dir build-windows --expect on`
- [x] 필요 시 Linux/Docker capability 명령 실행 결과 기록

리뷰:

- [x] 구현 후 변경 범위/테스트 결과/잔여 리스크를 본 섹션에 기록
- 변경 범위: `LuaRuntime`를 실제 VM 기반 host callback/`ctx` 전달/allocator+instruction limit 경로로 교체했고, `ChatService`는 read-only/action/log/meta Lua host API를 실제 상태/알림/moderation 경로에 연결했다. cold hook와 reload는 같은 strand에서 직렬화되며, 샘플/문서/CI는 function-style hook 우선 모델로 정리했다.
- 릴리스 회귀 보강: `tests/CMakeLists.txt`의 `chat_history_tests`에 `server/src/scripting/chat_lua_bindings.cpp`를 추가해 Release 전체 빌드에서 `register_chat_lua_bindings(...)` LNK2019를 해소했다.
- 검증(2026-03-06): `pwsh scripts/build.ps1 -Config Debug -Target server_general_tests`, `pwsh scripts/build.ps1 -Config Debug -Target core_plugin_runtime_tests`, `ctest --preset windows-test -R "LuaRuntimeTest|LuaSandboxTest|ChatLuaBindingsTest|LuaHookIntegrationTest|HookAutoDisableTest|ChatServiceTest\\.Lua" --output-on-failure` 실행 결과 29/29 pass.
- 검증(추가): `pwsh scripts/build.ps1 -Config Release`, `ctest --preset windows-test --output-on-failure` 실행 결과 `0 failed / 239`이며 `StorageBasic.RoomMessageMembershipHappyPath` 1건과 stack Python 8건은 skip으로 처리됐다.
- 검증(추가): `python tools/check_lua_build_toggle.py --build-dir build-windows --expect on` 통과. `compile_commands.json` 부재로 source-selection 세부 체크는 skip되었지만 `BUILD_LUA_SCRIPTING=ON` cache 상태는 확인했다.
- Docker follow-up(2026-03-06): `.dockerignore`가 `external/` 전체를 제외해 Linux image build에서 `external/luajit`/`external/sol2`가 빠지는 문제를 확인했고, 두 서브모듈만 재포함하도록 수정해 `scripts/deploy_docker.ps1 -Action up -Detached -Build` 경로를 복구했다.
- Docker follow-up(2026-03-06): gateway가 로그인 전 첫 프레임 `MSG_PING`을 닫아 baseline `verify_pong.py`가 실패하는 문제를 확인했고, `gateway/src/gateway_connection.cpp`에서 pre-login `MSG_PING -> MSG_PONG` 로컬 응답 후 로그인 대기를 유지하도록 보강했다.
- Docker 검증(2026-03-06): baseline/off stack에서 `verify_runtime_toggle_metrics.py --expect-chat-hook-enabled 0 --expect-lua-enabled 0`, `verify_pong.py`, `verify_chat.py`를 통과했다.
- Docker 검증(2026-03-06): runtime on stack에서 health/ready, `verify_runtime_toggle_metrics.py --expect-chat-hook-enabled 1 --expect-lua-enabled 1`, `verify_pong.py`, `test_load_balancing.py`, `verify_whisper_cross_instance.py`, `verify_admin_api.py`, `verify_admin_auth.py`, `verify_admin_control_plane_e2e.py`, `verify_soak_perf_gate.py`를 통과했다.
- Docker 검증(2026-03-06): plugin/script smoke로 `verify_plugin_hot_reload.py --check-only`, `verify_plugin_hot_reload.py`, `verify_plugin_v2_fallback.py`, `verify_plugin_rollback.py`, `verify_script_hot_reload.py`, `verify_script_fallback_switch.py`, `verify_chat_hook_behavior.py`를 모두 통과했다.
