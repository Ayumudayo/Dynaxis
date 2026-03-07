# Loadgen Next Steps

상태: UDP/RUDP attach success/fallback smoke phase complete; next session should focus on quantitative validation and rollout rehearsal

현재 기준:

- 단일 실행 파일: `stack_loadgen`
- 구현된 transport/workload:
  - `tcp`: `chat` / `ping` / `login_only`
  - `udp`: deterministic bind attach validation (`login_only` only)
  - `rudp`: bind + HELLO attach success/fallback visibility (`login_only` only)
- 시나리오 포맷: schema-driven JSON (`schema_version=1` required)
- 최근 수정 핵심: gateway UDP bind response `async_send_to` buffer lifetime fix
- 장기 과제: richer scenario language

관련 문서:

- 설계/현재 상태: [loadgen-plan.md](loadgen-plan.md)
- 실행 가이드: [README.md](../../tools/loadgen/README.md)
- active backlog: [todo.md](../../tasks/todo.md)
- 정량 측정 backlog: [quantitative-validation.md](../../tasks/quantitative-validation.md)

## 1. 왜 다음 작업이 필요한가

현재 `stack_loadgen`은 attach-capable transport harness까지 올라왔고, 기본 attach smoke는 same-network / Windows host-path direct same-gateway에서 모두 통과한다.

다음 단계의 목적은 다음과 같다.

- baseline mixed TCP+UDP soak과 RUDP success/fallback/OFF proof를 더 긴 샘플과 운영형 명령으로 확장한다.
- immediate long samples(`mixed_session_soak_long`, `mixed_direct_udp_soak_long`, `mixed_direct_rudp_soak_long`)는 이미 추가됐다.
- mixed long RUDP policy comparison(`attach` / `fallback` / `off` / `restore`)도 이미 확보됐다.
- attach smoke에서 끝나지 않고 duration/error/fallback 관점의 운영형 검증으로 연결한다.
- 향후 richer scenario language가 필요할지 판단할 만큼 실행 데이터를 축적한다.

## 2. 다음 세션의 권장 범위

권장 범위:

1. 새 long sample을 더 긴 soak / 더 큰 세션 수로 한 단계 더 확장
2. 확보된 long mixed RUDP policy baseline을 바탕으로 더 큰 rollout rehearsal을 설계
3. attach trace를 기본 smoke proof로 남기고 운영형 검증 명령을 정리
4. 필요 시 attach-only sample과 mixed sample 사이의 시나리오 공백을 메우는 새 sample 추가
5. richer scenario language가 필요한지 판단 근거를 남기기

비권장 범위:

- richer scenario language 구현
- distributed coordinator/worker 구조
- GUI 통합
- loadgen 자체를 별도 서비스처럼 운영하는 작업

## 3. 구현 우선순위

### Phase A. Quantitative UDP Validation

목적:

- attach smoke를 넘어 mixed traffic 숫자를 확보한다.

작업:

- `mixed_session_soak.json`을 기반으로 TCP+UDP 혼합 soak 시나리오를 설계/실행한다.
- baseline sample `mixed_direct_udp_soak.json`은 이미 추가됐다.
- immediate expansion sample `mixed_direct_udp_soak_long.json`도 추가됐다.
- duration / error rate / attach failure / fallback 수치를 report에 남긴다.
- direct same-gateway TCP+UDP 경로를 기준으로 명령/리포트 경로를 문서화한다.

완료 기준:

- baseline 60초 mixed soak 결과는 확보했다 (`build/loadgen/mixed_direct_udp_soak.host.json`).
- long mixed sample 결과도 확보했다 (`build/loadgen/mixed_direct_udp_soak_long.host.json`, `build/loadgen/mixed_direct_rudp_soak_long.host.json`).
- 다음 표본은 duration / 세션 수를 늘려도 `udp_bind_failures=0`을 유지하는지 확인한다.

### Phase B. RUDP Canary / Fallback Validation

목적:

- canary/fallback 정책이 실제 런타임에서 report에 어떻게 드러나는지 검증한다.

작업:

- `GATEWAY_RUDP_ENABLE`, `GATEWAY_RUDP_CANARY_PERCENT`, allowlist 조합을 바꿔 attach success / fallback / OFF invariance 결과를 추가 표본으로 수집한다.
- `mixed_direct_rudp_soak_long.json`을 success-path baseline으로 삼아 policy 변화 시 표본을 비교한다.
- fallback이 summary/report에 기대한 대로 드러나는지 확인한다.
- 운영/롤백 리허설 근거로 남길 최소 표본을 만든다.
- `.env.rudp-*.example`를 사용할 때는 `GATEWAY_UDP_BIND_SECRET`를 non-empty 값으로 교체한 뒤 실행한다.

주의:

- 현재 attach smoke에서는 success path(`rudp_attach_ok=4`), forced fallback path(`rudp_attach_fallback=4`), OFF invariance(`GATEWAY_RUDP_ENABLE=0`에서도 fallback)까지 확보했다.
- `mixed_direct_rudp_soak_long.json`도 success / fallback / OFF / restore 표본이 모두 확보됐다.
- attach 결과는 gateway-local bind ticket 때문에 HAProxy가 아니라 direct same-gateway TCP+UDP 경로에서 측정해야 한다.

완료 기준:

- canary/fallback/OFF 조합별 report path와 summary line이 누적된다.
- 현재 확보된 baseline proof를 바탕으로 운영형 표본과 rollout rehearsal을 확장한다.
- full scenario matrix recheck도 완료돼 현재 샘플 전체가 깨지지 않음을 다시 확인했다.

### Phase C. Follow-up Cleanup

목적:

- 다음 장기 후속을 위한 문서와 샘플을 정리한다.

작업:

- attach trace를 기본 smoke command로 문서에 남긴다.
- 필요 시 1-session debug scenario를 추가한다.
- richer scenario language 필요 여부를 기록한다.

주의:

- 새 구현보다 실행 데이터 정리와 운영형 재현성이 더 중요하다.
- trace는 유지하되 일상 실행에서 지나치게 시끄럽지 않도록 `--verbose` 경로를 유지한다.

완료 기준:

- 다음 세션이 곧바로 quantitative backlog를 실행할 수 있다.
- attach smoke / mixed soak / canary fallback / OFF invariance 명령이 문서에 정리된다.
- long sample 3종의 command/report path와 mixed long RUDP policy comparison 결과가 문서에 정리된다.

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
- attach smoke는 현재 수동 게이트이므로 결과 path/summary line을 문서에 남겨 회귀 기준으로 삼을 것

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
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build -EnvFile "docker/stack/.env.rudp-attach.example"
```

attach smoke:

```powershell
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 36100 `
  --udp-port 7000 `
  --scenario tools/loadgen/scenarios/udp_attach_login_only.json `
  --report build/loadgen/udp_attach_login_only.host.json `
  --verbose

build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 36100 `
  --udp-port 7000 `
  --scenario tools/loadgen/scenarios/rudp_attach_login_only.json `
  --report build/loadgen/rudp_attach_login_only.host.json `
  --verbose

# forced fallback proof
pwsh scripts/deploy_docker.ps1 -Action up -Detached -EnvFile "docker/stack/.env.rudp-fallback.example"
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 36100 `
  --udp-port 7000 `
  --scenario tools/loadgen/scenarios/rudp_attach_login_only.json `
  --report build/loadgen/rudp_attach_login_only.fallback.json `
  --verbose

# long samples
pwsh scripts/deploy_docker.ps1 -Action up -Detached -EnvFile "docker/stack/.env.rudp-attach.example"
build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 6000 `
  --scenario tools/loadgen/scenarios/mixed_session_soak_long.json `
  --report build/loadgen/mixed_session_soak_long.json

build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 36100 `
  --udp-port 7000 `
  --scenario tools/loadgen/scenarios/mixed_direct_udp_soak_long.json `
  --report build/loadgen/mixed_direct_udp_soak_long.host.json

build-windows\Release\stack_loadgen.exe `
  --host 127.0.0.1 `
  --port 36100 `
  --udp-port 7000 `
  --scenario tools/loadgen/scenarios/mixed_direct_rudp_soak_long.json `
  --report build/loadgen/mixed_direct_rudp_soak_long.host.json
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

다음 세션의 핵심 목표는 “transport를 붙이는 것”이 아니라, 이미 붙은 `udp` / `rudp` 경로를 운영형 수치와 fallback rehearsal로 검증하는 것이다.
