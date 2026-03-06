# Loadgen Next Steps

상태: handoff note for the next implementation session

현재 기준:

- 단일 실행 파일: `stack_loadgen`
- 구현된 transport: `tcp`
- 시나리오 포맷: schema-driven JSON
- 미구현 transport: `udp`, `rudp`
- 장기 과제: richer scenario language

관련 문서:

- 설계/현재 상태: [loadgen-plan.md](loadgen-plan.md)
- 실행 가이드: [README.md](../../tools/loadgen/README.md)
- active backlog: [todo.md](../../tasks/todo.md)
- 정량 측정 backlog: [quantitative-validation.md](../../tasks/quantitative-validation.md)

## 1. 왜 다음 작업이 필요한가

현재 `stack_loadgen`은 “단일 harness”라는 구조적 목표는 달성했지만, 실질 검증 범위는 아직 TCP에 머물러 있다.

다음 단계의 목적은 다음과 같다.

- `udp` / `rudp` 경로도 같은 시나리오/리포트 체계 아래에서 검증할 수 있게 한다.
- transport가 달라도 같은 workload intent를 비교 가능하게 만든다.
- 향후 정량 검증(mixed TCP+UDP soak, canary fallback, rollout rehearsal)의 실행 도구를 미리 정돈한다.

## 2. 다음 세션의 권장 범위

권장 범위:

1. `udp` transport client 추가
2. `rudp` transport client 추가 또는 최소 scaffold + explicit not-supported boundary 정리
3. 시나리오 validation 강화를 위한 `schema_version` 도입
4. transport별 stats breakdown 확장
5. 관련 문서와 `tasks/quantitative-validation.md` 연결

비권장 범위:

- richer scenario language 구현
- distributed coordinator/worker 구조
- GUI 통합
- loadgen 자체를 별도 서비스처럼 운영하는 작업

## 3. 구현 우선순위

### Phase A. Scenario Contract Hardening

목적:

- transport 확장 전에 시나리오 포맷이 흔들리지 않게 고정한다.

작업:

- `schema_version` 필드 추가
- `transport`를 top-level default + `groups[]` override 형태로 유지
- unsupported transport / invalid mode / count mismatch / rate validation을 명시적 에러로 유지
- 가능하면 시나리오 parsing/validation을 `main.cpp`에서 별도 파일로 분리

완료 기준:

- invalid scenario가 조용히 fallback되지 않고 명시적으로 실패
- `tcp` 시나리오는 기존과 동일하게 동작

### Phase B. UDP Transport

목적:

- 기존 workload intent를 UDP path에서 재사용 가능한 최소 transport 구현을 추가한다.

작업:

- `UdpSessionClient` 추가
- connect/login/join/chat/ping 중 실제 UDP path에 맞는 subset 정의
- unsupported operation은 명시적 에러로 처리
- report에 `transports=["udp"]` 및 UDP-specific transport stats 추가 검토

주의:

- 현재 제품 구조에서 어떤 opcode가 UDP path에 실질적으로 적합한지 먼저 확인할 것
- session/bootstrap 계약이 TCP와 다를 수 있으므로 “TCP 흐름 복사”로 밀어붙이지 말 것

완료 기준:

- 최소 1개 UDP scenario가 deterministic하게 성공
- unsupported workload는 명시적으로 실패

### Phase C. RUDP Transport

목적:

- rollout/canary/fallback 검증의 기반을 만든다.

작업:

- `RudpSessionClient` 추가 또는 최소 scaffold
- allowlist/canary/runtime flag 조건에서 attach 가능한지 확인
- fallback 발생 시 결과가 report에 드러나게 설계

주의:

- 이 단계는 transport 구현보다 “fallback/selection visibility”가 더 중요하다
- 성공/실패만 보지 말고 어떤 경로로 붙었는지 기록해야 한다

완료 기준:

- RUDP attach 여부 / fallback 여부가 summary와 report에 표시
- `tasks/quantitative-validation.md`의 RUDP 항목을 실행 가능한 수준으로 연결

## 4. 추천 파일 분해 방향

현재 결합이 높은 파일:

- [main.cpp](../../tools/loadgen/main.cpp)

다음 세션에서 우선 분리할 후보:

- `scenario_types.hpp`
- `scenario_loader.cpp`
- `scenario_runner.cpp`
- `report_writer.cpp`
- `transport_factory.cpp`

유지할 것:

- [session_client.hpp](../../tools/loadgen/session_client.hpp)
- [session_client.cpp](../../tools/loadgen/session_client.cpp)

의도:

- `main.cpp`는 CLI 진입점만 남기고, transport/scenario/reporting 로직을 파일 단위로 나눈다.

## 5. 검증 전략

로컬 최소 게이트:

- `pwsh scripts/build.ps1 -Config Release -Target stack_loadgen`
- 기존 TCP scenario 2개 재실행
- 새 transport scenario 1개 이상 실행

권장 추가 검증:

- runtime off / on stack 모두에서 attach behavior 확인
- unsupported transport/mode scenario가 기대한 에러 메시지로 실패하는지 확인
- report JSON에 transport 식별자와 카운터가 들어가는지 확인

가능하면 다음 형식으로 결과를 남길 것:

- command
- summary line
- report path
- transport-specific notes

## 6. 현재 알고 있는 함정

- `haproxy -> gateway_app` TCP 경로는 connect 직후 `MSG_HELLO`를 보장하지 않는다.
- join 성공 확인은 현재 `MSG_STATE_SNAPSHOT`보다 시스템 `MSG_CHAT_BROADCAST`가 더 안정적이었다.
- room 재사용은 이전 run의 history/state를 끌고 와 결과를 흔들 수 있으므로 `unique_room_per_run=true` 기본값을 유지하는 편이 안전하다.
- loadgen 반복 실행 중 드러났던 gateway double-free는 이미 수정됐지만, 새 transport 추가 후 same-stack repeated run은 다시 확인해야 한다.
- JSON은 현재 적절하지만, 분기/반복/장애 주입이 커지면 richer scenario language를 검토해야 한다.

## 7. 다음 세션 시작 명령

빌드:

```powershell
pwsh scripts/build.ps1 -Config Release -Target stack_loadgen
```

스택:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build
```

기존 TCP 회귀:

```powershell
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 6000 `
  --scenario tools/loadgen/scenarios/steady_chat.json `
  --report build/loadgen/steady_chat.json

build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 6000 `
  --scenario tools/loadgen/scenarios/mixed_session_soak.json `
  --report build/loadgen/mixed_session_soak.json
```

종료:

```powershell
pwsh scripts/deploy_docker.ps1 -Action down
```

## 8. 다음 세션에서 먼저 읽을 파일

- [loadgen-plan.md](loadgen-plan.md)
- [session_client.hpp](../../tools/loadgen/session_client.hpp)
- [main.cpp](../../tools/loadgen/main.cpp)
- [quantitative-validation.md](../../tasks/quantitative-validation.md)

## 9. 한 줄 요약

다음 세션의 핵심 목표는 “새 프로그램을 더 만드는 것”이 아니라, `stack_loadgen` 하나 아래에 `udp` / `rudp` transport를 같은 시나리오·리포트 체계로 붙이는 것이다.
