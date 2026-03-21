# 엔진 준비도 기준선 결정

이 문서는 common engine-readiness baseline이 downstream runtime 작업을 막지 않을 만큼 충분하다고 받아들여진 결정을 기록한다. 지금은 과거 결정 문서지만, 왜 그 시점에 기준선을 닫아도 된다고 보았는지를 설명하는 역할은 여전히 남아 있다.

이 문서가 필요한 이유는 readiness 판단이 보통 테스트 통과 여부만으로 끝나지 않기 때문이다. 어떤 gap은 blocker이고 어떤 gap은 downstream concern인지 선을 그어야 branch split과 후속 병합을 정당화할 수 있다.

## 결정 날짜

- 2026-03-14

## 결정

- `ready to split from the common baseline`: 예
- `reason`: outage/recovery와 restart 리허설이 bounded하고 recoverable했으며, worker Redis signaling gap이 닫혔고, overload blocker는 무효 처리 후 깨끗하게 재실행되었다.
- `현재 상태`: 이 결정에 의존하던 downstream continuity, MMORPG, FPS 작업 줄기는 현재 모두 `main`에 병합되었다.

여기서 중요한 것은 "문제가 전혀 없었다"가 아니라 "공통 blocker로 볼 문제는 충분히 해소되었다"는 점이다.

## 해결된 공통 blocker

### Worker Redis degraded-state 가시성

- `wb_worker`는 이제 readiness와 dependency metric을 통해 Redis 저하 상태를 드러낸다.
- 이전 worker 측 관측성 공백은 branch-cut 기준으로는 닫힌 것으로 본다.

이 gap이 blocker였던 이유는 worker만 조용히 망가지면 gateway와 server가 보여 주는 상태와 운영 판단이 어긋나기 때문이다. 의존성 신호는 모든 주요 프로세스에서 비슷한 의미로 보여야 한다.

### Overload/login-collapse blocker

- 최초 실패한 overload 리허설은 동시 loadgen 프로세스가 같은 login ID를 재사용했다.
- 따라서 그 결과는 엔진 overload 신호로 볼 수 없었다.
- 수용된 재실행은 identity-safe username을 사용했고, `0` login failure와 `0` total error로 끝났다.

이 부분을 굳이 적는 이유는 잘못된 실험 결과를 blocker로 남겨 두면 이후 설계 판단이 모두 흔들릴 수 있기 때문이다. readiness 문서에서는 실험의 유효성 자체도 명시해야 한다.

## 미뤄 둔 caveat

- gateway나 server 재시작 중 투명한 in-flight continuity는 downstream concern으로 남겼고, 공통 기준선 blocker로는 보지 않았다.
- worker backlog-depth 가시성은 이상적이지 않았지만 recovery correctness를 무효로 만들 정도는 아니었다.
- 게임플레이 수준의 UDP/RUDP transport 성숙도는 공통 기준선 밖으로 두었다.

이 caveat를 남겨 둔 이유는 기준선 종료가 완전성을 뜻하지 않기 때문이다. downstream 팀은 무엇을 새로 해결해야 하는지 명확히 알아야 한다.

## 현재 downstream 소유 범위

- continuity와 restart 의미론: `docs/ops/session-continuity-contract.md`
- world residency/lifecycle와 운영자 정책: `docs/ops/mmorpg-world-residency-contract.md`
- gameplay-frequency transport/runtime substrate: `docs/ops/realtime-runtime-contract.md`

## 메모

이 문서는 더 이상 live branch tracker가 아니다. 공통 기준선을 왜 닫을 수 있었는지 설명하는 retained explanation으로 남는다.
