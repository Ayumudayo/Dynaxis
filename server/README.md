# 서버 애플리케이션(server_app)

`server_app`은 Dynaxis의 메인 채팅/런타임 서버다. 클라이언트와 직접 붙는 경우도 있지만, 표준 배치에서는 `gateway_app` 뒤에서 backend 역할을 맡는다.

이 모듈이 중요한 이유는 단순 채팅 처리만 하는 서버가 아니기 때문이다. 현재 구현은 다음을 함께 소유한다.

- 로그인과 채팅 비즈니스 로직
- 세션 연속성(session continuity)
- world residency와 runtime assignment consumer
- FPS fixed-step substrate의 실제 앱 소비자
- Redis / Postgres / write-behind와의 실제 통합

즉 `core`가 공용 플랫폼이라면, `server_app`은 그 플랫폼 위에서 실제 제품 동작을 만들어 내는 대표 앱이다.

## 현재 구조

서버는 크게 네 층으로 나뉜다.

1. `src/app/`
   - bootstrap, config, router, metrics server
2. `src/chat/`
   - 핵심 채팅 비즈니스 로직
3. `src/state/`
   - 분산 상태/레지스트리 adapter
4. `src/storage/`
   - Redis/Postgres concrete adapter

이렇게 나누는 이유는 공용 contract와 앱 통합 로직을 섞지 않기 위해서다. 예를 들어 DB/Redis adapter는 core contract를 소비하지만, 구체적인 연결 전략과 repository wiring은 여전히 `server/`가 소유한다.

## 부트스트랩과 요청 처리

- 부트스트랩 진입점: `src/app/bootstrap.cpp`
- opcode 라우팅 등록: `src/app/router.cpp`
- 핵심 비즈니스 로직: `src/chat/`

현재 런타임은 다음 흐름으로 움직인다.

1. bootstrap이 `EngineRuntime`, `Hive`, worker queue, storage/state adapter를 조립한다.
2. packet session ingress는 internal `core::Session`을 통해 들어온다.
3. `Dispatcher`가 opcode별 handler로 분기한다.
4. `ChatService`가 메모리 상태, Redis, DB/write-behind를 중재한다.
5. metrics/admin surface는 별도 HTTP 경로로 노출된다.

이 흐름이 좋은 이유는 네트워크 수락, 실행 위치 결정, 비즈니스 처리, 영속화를 한 함수 안에 몰아넣지 않기 때문이다. 그렇지 않으면 장애 원인을 계층별로 분리하기 어렵고, 성능 문제와 도메인 문제를 같은 레벨에서 고치게 된다.

## 주요 기능

- Opcode 라우팅
  - 패킷을 handler로 연결하되, 실제 처리 위치는 `OpcodePolicy`가 결정한다
- Snapshot + Redis cache
  - 방 입장 시 최근 상태를 빠르게 복원한다
- Refresh fanout 정리
  - 불필요한 fanout 신호를 줄여 noisy refresh path를 완화한다
- RoomUsers lock scope 축소
  - lock 경쟁을 줄여 hot path 지연을 낮춘다
- Write-behind
  - 즉시 DB에 쓰지 않고 Redis Streams를 거쳐 비동기 영속화한다
- Instance Registry
  - Gateway가 backend를 찾을 수 있도록 자신의 identity와 heartbeat를 등록한다
- Metrics
  - `/metrics`에서 Prometheus 호환 지표를 노출한다

각 기능이 이런 형태를 갖는 이유도 중요하다.

- 캐시를 쓰지 않으면 방 입장 시 DB round-trip 때문에 응답이 느려진다.
- write-behind가 없으면 DB 지연이 바로 사용자 체감 지연으로 번진다.
- registry가 없으면 gateway는 어느 backend가 살아 있고 얼마나 바쁜지 판단하기 어렵다.
- 지표가 없으면 로그인 지연, fanout 병목, drain timeout 같은 운영 문제를 재현 없이 찾기 어렵다.

## 빌드와 실행

### 빌드

```powershell
pwsh scripts/build.ps1 -Config Debug -Target server_app
pwsh scripts/build.ps1 -Config RelWithDebInfo -Target server_app
```

### 권장 실행

표준 런타임은 Linux/Docker 스택이다.

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build
```

### Windows 단일 실행(옵션)

```powershell
.\build-windows\server\Debug\server_app.exe 5000
```

Windows 단일 실행이 가능한 이유는 개발/디버깅 편의 때문이다. 하지만 운영 문제를 재현하거나 표준 네트워크 경로를 검증할 때는 Docker 스택을 우선하는 편이 더 안전하다.

## 설정 메모

자세한 환경 변수 설명은 `docs/configuration.md`를 기준으로 본다. 여기서는 운영적으로 특히 중요한 항목만 다시 적는다.

- `DB_URI`
  - DB가 느리거나 죽어 있으면 로그인/영속화 품질에 직접 영향이 간다
- `REDIS_URI`
  - continuity, fanout, registry, write-behind가 모두 얽혀 있어 사실상 핵심 의존성이다
- `SERVER_ADVERTISE_HOST`, `SERVER_ADVERTISE_PORT`
  - gateway가 실제로 접속할 주소이므로 잘못 잡히면 registry는 살아 있어도 backend 연결이 실패한다
- `SERVER_REGISTRY_PREFIX`, `SERVER_REGISTRY_TTL`
  - gateway와 같은 prefix를 써야 registry가 맞물린다
- `WRITE_BEHIND_ENABLED`
  - 켜면 응답성과 영속화가 분리되지만, worker/stream 운영이 함께 필요하다
- `METRICS_PORT`
  - 운영 가시성 확보에 필수다

## 종료(Graceful Drain)

`server_app`은 종료 신호를 받으면 다음 순서로 내려간다.

1. readiness를 즉시 `false`로 내린다
2. 새 연결을 막는다
3. 기존 연결을 drain한다
4. timeout을 넘으면 남은 연결을 강제 정리한다

이 순서가 필요한 이유는, 종료 중에도 새 연결을 계속 받으면 drain이 끝나지 않기 때문이다. 반대로 readiness를 너무 늦게 내리면 상위 ingress가 아직 정상 backend라고 오해해 트래픽을 계속 보낼 수 있다.

## 관련 문서

- `docs/configuration.md`
- `docs/tests.md`
- `docs/core-design.md`
- `docs/core-architecture-rationale.md`
- `docs/db/write-behind.md`
- `docs/ops/session-continuity-contract.md`
- `docs/ops/mmorpg-world-residency-contract.md`
