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

- [ ] `SessionStatus`, `ProcessingPlace`, `TransportMask`, `DeliveryClass` 열거값 정의
- [ ] 기본값 정책(legacy opcode 호환용) 정의
- [ ] 정책 필드 유효성 규칙(조합 제한) 정의

## 1.2 opcode 분류표 작성

- [ ] 시스템 opcode 분류(TCP-only/UDP-candidate/dual)
- [ ] 게임 opcode 분류(TCP-only/UDP-candidate/dual)
- [ ] 고위험 opcode(인증/권한/정합성) TCP-only 고정 목록 작성

## 1.3 문서 동기화

- [ ] `docs/protocol/opcodes.md`에 정책 컬럼 초안 반영
- [ ] 운영 문서(`docs/ops/observability.md`, `docs/ops/runbook.md`)에 변경 예정 지표/운영 포인트 추가

완료 기준:

- [ ] 스키마와 분류표가 리뷰 승인됨

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

## 4.1 전송 추상화

- [ ] 코어 전송 인터페이스(`ITransportSession` 성격) 도입
- [ ] 기존 TCP `Session`을 인터페이스 구현체로 연결
- [ ] UDP 세션 구현체 추가(수신/송신/세션 수명)

## 4.2 Gateway 연동

- [ ] `gateway/src/gateway_app.cpp`에 UDP listener 경로 추가
- [ ] TCP 인증 세션과 UDP 세션 바인딩 절차 구현
- [ ] 바인딩 실패/만료/재시도 정책 구현

## 4.3 정책 기반 전송 분기

- [ ] `TransportMask` + `DeliveryClass` 기반 송수신 분기 구현
- [ ] `UnreliableSequenced`용 seq window/replay guard 구현
- [ ] 초기 대상 opcode를 최소 세트로 제한

## 4.4 검증

- [ ] 손실/중복/역순 시뮬레이션 테스트 통과
- [ ] TCP fallback 정상 동작 확인
- [ ] 혼합 트래픽(TCP+UDP) 장시간 soak 테스트

완료 기준:

- [ ] UDP 대상 opcode가 안정적으로 처리되고 TCP 회귀가 없음

---

## 5) Phase 4 - 관측성/보안/운영 정착

## 5.1 메트릭/대시보드

- [ ] UDP 품질 지표 추가(RTT/loss/jitter/reorder/dup/retransmit)
- [ ] transport/delivery 레이블 기반 대시보드 갱신
- [ ] 임계치 기반 알람 규칙 반영

## 5.2 보안/남용 방지

- [ ] UDP 바인딩 토큰 서명/TTL 검증
- [ ] rate limit 및 반복 실패 차단
- [ ] replay/session hijack 방어 테스트

## 5.3 운영 문서

- [ ] `docs/ops/observability.md` 갱신
- [ ] `docs/ops/runbook.md`에 장애 대응 절차 추가
- [ ] 배포/롤백 체크리스트(`docs/ops/deployment.md`) 갱신

완료 기준:

- [ ] 운영팀이 투트랙 장애 대응을 문서만으로 수행 가능

---

## 6) CI/빌드/테스트 체계

- [ ] CMake 옵션(UDP on/off, feature flag) 정비
- [ ] `.github/workflows/ci.yml`에 schema/policy/test 단계 추가
- [ ] 빠른 테스트와 느린 테스트를 분리해 파이프라인 최적화

완료 기준:

- [ ] main 브랜치에서 지속적으로 안정적인 CI 결과 확보

---

## 7) 롤아웃/롤백 실행 체크리스트

## 7.1 롤아웃

- [ ] canary 환경에서 UDP 대상 opcode 제한 오픈
- [ ] 핵심 메트릭 안정화 확인 후 점진 확장
- [ ] 이슈 발생 시 즉시 feature flag rollback

## 7.2 롤백

- [ ] UDP ingress 즉시 차단 절차 검증
- [ ] TCP-only 모드 복귀 시 데이터/세션 정합성 확인
- [ ] 사후 분석 및 재시도 조건 문서화

완료 기준:

- [ ] 10분 내 안전 롤백이 가능함을 리허설로 증명

---

## 8) 최종 DoD

- [ ] 모든 opcode에 정책 메타가 존재한다(기본값 포함).
- [ ] TCP 기존 회귀 테스트가 모두 통과한다.
- [ ] UDP 대상 opcode의 손실/중복/역순 테스트가 통과한다.
- [ ] 메트릭/알람/런북이 운영 수준으로 준비된다.
- [ ] 단계별 rollout/rollback이 실제 리허설로 검증된다.
