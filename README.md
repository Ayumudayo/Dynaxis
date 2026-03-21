# Dynaxis

Dynaxis는 C++20 기반 `server_core` 저장소이며, 그 위에 분산 채팅 스택과 실시간(runtime) 실험 계층을 함께 올려 둔 모노레포다.

가장 먼저 이해해야 할 점은 이 저장소가 단순 채팅 서버 하나만 담고 있지 않다는 점이다.

- `core/`는 여러 실행 파일이 공통으로 쓰는 실행 플랫폼이다.
- `gateway/`, `server/`, `tools/`는 그 플랫폼 위에서 조립되는 앱과 운영 도구다.

이 구조가 중요한 이유는 공통 런타임 규약을 한곳에 모아야 하기 때문이다. 이런 분리가 없으면 각 실행 파일이 수명주기, 관측성, 네트워크 제어, 장애 대응을 제각각 구현하게 되고, 장애 분석과 유지보수가 급격히 어려워진다.

기본 런타임 형태는 다음과 같다.

`HAProxy -> gateway_app -> server_app`

여기에 Redis와 Postgres가 상태, fanout, write-behind 영속성을 담당한다.

## 이 저장소에 있는 것

- `core/`
  - reusable runtime/core library (`server_core`)
  - 공통 lifecycle, 네트워크 substrate, 관측성, 저장소 실행 seam, 확장 메커니즘을 소유한다
- `gateway/`
  - 엣지 세션 수용, sticky routing, direct UDP/RUDP ingress를 담당한다
- `server/`
  - 채팅 로직, 세션 연속성, 월드(world) residency, realtime substrate consumer를 담당한다
- `tools/`
  - admin, loadgen, migration, write-behind 관련 도구를 담는다
- `docker/stack/`
  - 로컬 및 표준 풀스택 실행 진입점이다

## 아키텍처 요약

- 외부 L4 로드밸런서(예: HAProxy)가 TCP 진입 트래픽을 분산한다.
- `gateway_app`은 클라이언트 연결을 받아 첫 홉 인증과 backend 선택을 수행한다.
- backend 선택은 Redis 기반 sticky binding과 least-load 판단을 함께 사용한다.
- `server_app`은 채팅 상태, 세션 연속성 상태, world residency, FPS fixed-step substrate를 소유한다.
- write-behind 영속성은 Redis Streams를 거쳐 `wb_worker`가 Postgres로 반영한다.

이렇게 나눈 이유는 역할별 failure domain을 분리하기 위해서다. 모든 기능을 한 프로세스에 넣으면 구현은 단순해 보이지만, 실제 운영에서는 ingress 병목, 저장소 장애, fanout 지연, 세션 연속성 문제를 서로 독립적으로 제어하기가 훨씬 어려워진다.

제어면(control-plane)과 장애 경로를 그림으로 보고 싶다면 [docs/ops/architecture-diagrams.md](docs/ops/architecture-diagrams.md)를 먼저 본다.

## 먼저 읽을 문서

- 로컬 설치와 첫 실행: [docs/getting-started.md](docs/getting-started.md)
- 빌드와 환경: [docs/build.md](docs/build.md)
- 런타임 설정: [docs/configuration.md](docs/configuration.md)
- 테스트와 검증: [docs/tests.md](docs/tests.md)
- 프로토콜과 상태 흐름: [docs/protocol.md](docs/protocol.md), [docs/protocol/snapshot.md](docs/protocol/snapshot.md)
- 문서 진입점: [docs/README.md](docs/README.md)

## 현재 핵심 계약

- 엔진 준비도 기준선: [docs/ops/engine-readiness-baseline.md](docs/ops/engine-readiness-baseline.md)
- 세션 연속성: [docs/ops/session-continuity-contract.md](docs/ops/session-continuity-contract.md)
- 월드 residency / lifecycle: [docs/ops/mmorpg-world-residency-contract.md](docs/ops/mmorpg-world-residency-contract.md)
- 실시간 전송/런타임 substrate: [docs/ops/realtime-runtime-contract.md](docs/ops/realtime-runtime-contract.md)
- 관측성과 운영: [docs/ops/observability.md](docs/ops/observability.md), [docs/ops/runbook.md](docs/ops/runbook.md)

위 문서들을 먼저 잡아 두는 것이 중요한 이유는, 이 저장소의 동작 원리가 코드만으로는 바로 보이지 않기 때문이다. 특히 continuity, topology, realtime transport 쪽은 "왜 이렇게 했는가"를 문서와 함께 읽어야 유지보수 판단이 쉬워진다.

## 하위 프로젝트

| 프로젝트 | 경로 | 설명 |
| --- | --- | --- |
| Core | [core/README.md](core/README.md) | 공용 runtime/core 라이브러리 진입점 |
| Server | [server/README.md](server/README.md) | 앱 런타임 동작과 서비스 규칙 |
| Gateway | [gateway/README.md](gateway/README.md) | 엣지 라우팅과 전송 계층 운영 |
| Client GUI | [client_gui/README.md](client_gui/README.md) | Windows ImGui 개발 클라이언트 |
| Tools | [tools/README.md](tools/README.md) | 도구 인덱스와 per-tool 문서 |

## 빠른 명령

```powershell
pwsh scripts/build.ps1 -Config Debug
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build
ctest --preset windows-test
python tools/gen_opcode_docs.py --check
```

스택을 내릴 때는 다음을 사용한다.

```powershell
pwsh scripts/deploy_docker.ps1 -Action down
```
