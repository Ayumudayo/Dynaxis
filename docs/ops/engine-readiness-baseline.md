# 엔진 준비도 기준선 체크포인트

이 문서는 downstream continuity, MMORPG, FPS tranche가 합쳐지기 전에 accepted common baseline으로 받아들여졌던 공통 증거를 기록한다. 기준선 자체는 이미 종료된 과거 단계이지만, 왜 그 시점에 "이 정도면 공통 기반은 닫아도 된다"고 판단했는지를 남겨 두는 데 의미가 있다.

이 문서가 필요한 이유는 downstream 작업이 모두 합쳐진 뒤에도 "당시 어떤 위험을 공통 blocker로 봤고, 어떤 위험은 후속 품질 이슈로 넘겼는가"를 다시 설명할 수 있어야 하기 때문이다. 기록이 없으면 나중에 같은 논의를 다시 반복하게 된다.

## 기준선 상태

- `accepted shared-proof baseline`: 예
- `branch split`: 과거에 완료됨
- `downstream work on top of the baseline`: 현재는 `main`에 병합됨
- 남아 있던 restart/backlog 관련 caveat은 후속 품질 이슈였으며, 기준선 blocker로 보지는 않음

기준선 상태를 별도로 적는 이유는 "완벽해서 닫은 것"이 아니라 "공통 기반으로서 충분히 검토 가능해서 닫은 것"임을 분명히 하기 위해서다.

## 과거 실행 규칙

- 체크포인트는 bounded, reviewable slice 단위로 실행했다.
- 원시 증거는 `build/engine-readiness/<run_id>/` 아래에 보관했다.
- 상세 작업 메모는 local-only `tasks/`에 유지했다.

이 규칙을 따랐던 이유는 큰 readiness 판단을 한 번에 내리면 무엇이 통과했고 무엇이 미뤄졌는지 흐려지기 때문이다. slice 단위로 끊어야 리뷰와 재현이 가능하다.

## 체크포인트 원장

| 순서 | 체크포인트 | 실행 루트 | 판정 | 핵심 결과 |
| --- | --- | --- | --- | --- |
| 1 | 제어 기준선 재실행 | `build/engine-readiness/20260312-0228-control-baseline/` | pass | TCP, UDP, RUDP 제어 대조군이 충분히 깨끗하게 유지되어 failure/recovery 리허설을 시작할 수 있었다. |
| 2 | Redis 장애/복구 | `build/engine-readiness/20260314-0150-redis-recovery/` | caveat 포함 pass | gateway와 server는 눈에 띄게 저하되었다가 자동 복구했고, worker의 Redis 장애 신호는 상대적으로 더 약하게 드러났다. |
| 3 | Postgres 장애/복구 | `build/engine-readiness/20260314-0155-postgres-recovery/` | pass | server와 worker가 DB 저하를 명확히 드러냈고 자동 복구도 확인되었다. |
| 4 | live traffic 중 gateway 재시작 | `build/engine-readiness/20260314-0212-gateway-restart/` | caveat 포함 pass | 스택은 복구되었고 신규 트래픽도 계속 라우팅되었지만, 재시작된 gateway 위 세션은 유실되었다. |
| 5 | live traffic 중 server 재시작 | `build/engine-readiness/20260314-0216-server-restart/` | caveat 포함 pass | 스택은 복구되었고 probe도 계속 통과했지만, 재시작된 backend 위 세션은 유실되었다. |
| 6 | backlog 처리 중 worker 재시작 | `build/engine-readiness/20260314-0219-worker-restart/` | caveat 포함 pass | 재시작 뒤 backlog는 비워졌지만, backlog-depth metric보다 flush 로그 쪽 가시성이 더 강했다. |
| 7 | overload/backpressure 리허설 | `build/engine-readiness/20260314-0223-overload-rehearsal/` | superseded | 동시 loadgen 프로세스가 같은 login ID를 재사용해 duplicate-login collision이 발생했고, 초기 실행은 무효 처리되었다. |
| 8 | Redis worker degraded-state 보강 | `build/engine-readiness/20260314-024138-redis-remediation/` | pass | worker의 Redis 의존성 신호가 명시적으로 드러나 gateway와 server 동작과 더 잘 맞게 정렬되었다. |
| 9 | overload/backpressure 보강 재실행 | `build/engine-readiness/20260314-025202-overload-remediation-v3/` | pass | identity-safe 재실행이 `0` login failure, `0` total error로 완료되었다. |

이 원장을 남겨 두는 이유는 최종 결론보다 각 checkpoint의 caveat이 더 중요할 수 있기 때문이다. downstream 팀은 무엇이 이미 증명됐고, 무엇은 그 시점에도 남아 있었는지를 여기서 읽을 수 있어야 한다.

## 수용된 결론

- 공통 기준선은 2026-03-14에 수용되었다.
- 이후 downstream 작업은 공통 기준선 blocker를 다시 열지 않고 진행할 수 있도록 허용되었다.
- 이후 아래 downstream 줄기들은 모두 병합되었다.
  - session continuity substrate
  - MMORPG world residency/lifecycle substrate
  - FPS transport/runtime substrate

결론을 이렇게 적는 이유는 baseline close가 "모든 문제가 해결되었다"는 선언이 아니라 "공통 기반 blocker는 충분히 닫혔다"는 결정이었기 때문이다.

## 미뤄 둔 caveat

- gateway나 server 재시작 중 투명한 in-flight continuity는 기준선 이후 과제로 남겼다.
- worker backlog-depth 가시성은 이상적 수준보다 약했다.
- 게임플레이 수준의 UDP/RUDP 성숙도는 공통 기준선이 아니라 downstream FPS 과제로 남겼다.

이 항목들을 남겨 둔 이유는, 당시 기준으로도 불편한 점이 있다는 것을 숨기지 않기 위해서다. 미해결 caveat를 문서에 남겨야 이후 작업이 무엇을 이어받는지 분명해진다.

## 관련 문서

- `docs/ops/engine-readiness-decision.md`
- `docs/ops/session-continuity-contract.md`
- `docs/ops/mmorpg-world-residency-contract.md`
- `docs/ops/realtime-runtime-contract.md`
