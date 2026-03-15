# 도커(Docker) 스택 (검증)

`docker/stack/docker-compose.yml`는 검증/스모크 테스트용으로 전체 스택을 Docker로 기동한다.

구성:
- Client(호스트) -> HAProxy(container) -> gateway_app(containers) -> server_app(containers)
- Redis/Postgres는 compose로 함께 띄운다.
- `server_app` service topology는 committed manifest에서 생성된다.

## 실행

권장: `scripts/deploy_docker.ps1`를 사용한다. (base 이미지 빌드/compose profile/포트 매핑을 일관되게 유지)

현재 스택은 서비스별 런타임 이미지로 분리되어 빌드된다.
- `dynaxis-server:local`
- `dynaxis-gateway:local`
- `dynaxis-worker:local`
- `dynaxis-admin:local`
- `dynaxis-migrator:local`

```powershell
# 스택 기동(build + detached)
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build

# 스택 기동 + 관측성(Observability: Prometheus/Grafana)
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build -Observability

# same-world replacement proof topology
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build -TopologyConfig docker/stack/topologies/mmorpg-same-world-proof.json

# 또는 wrapper 사용
pwsh scripts/run_full_stack_observability.ps1
pwsh scripts/run_full_stack_observability.ps1 -TopologyConfig docker/stack/topologies/mmorpg-same-world-proof.json
```

## Topology manifests

- 기본 topology manifest: `docker/stack/topologies/default.json`
- same-world proof manifest: `docker/stack/topologies/mmorpg-same-world-proof.json`
- generator output:
  - `docker/stack/docker-compose.topology.generated.yml` (gitignored)
  - `docker/stack/topology.active.json` (gitignored)

`scripts/deploy_docker.ps1`는 base compose 뒤에 generated compose를 자동 포함하고,
`docker compose config --quiet`로 topology 결과를 검증한 뒤에만 기동한다.

현재 이 surface는 startup-only다.

- manifest 선택은 stack 시작 시점에만 적용된다
- 운영 중 live scale out/in semantics는 제공하지 않는다
- future desired-topology/live scaling design은:
  - `docs/ops/mmorpg-desired-topology-contract.md`
  - `docs/ops/mmorpg-scaling-orchestration-charter.md`
- 현재 startup manifest는 dev/proof용 concrete topology artifact이며, future desired-topology contract와 동일하지 않다

접속:
- 게임 트래픽: `127.0.0.1:6000` (HAProxy)
- HAProxy stats: `http://127.0.0.1:8404/`
- gateway metrics: `http://127.0.0.1:36001/metrics`, `http://127.0.0.1:36002/metrics`
- server metrics: `http://127.0.0.1:39091/metrics`, `http://127.0.0.1:39092/metrics`
- wb_worker metrics: `http://127.0.0.1:39093/metrics`
- admin console(UI): `http://127.0.0.1:39200/admin`
- admin API/metrics: `http://127.0.0.1:39200/api/v1/overview`, `http://127.0.0.1:39200/metrics`
- (옵션) Prometheus: `http://127.0.0.1:39090/`
- (옵션) Grafana: `http://127.0.0.1:33000/` (admin password: `GRAFANA_ADMIN_PASSWORD`, 기본 `admin`)

포트는 `docker/stack/docker-compose.yml`의 `*_HOST_PORT` 환경 변수로 재지정할 수 있다. (`ADMIN_APP_HOST_PORT` 포함)
server topology manifest는 각 server의 `tcp_host_port`, `metrics_host_port`를 직접 선언한다.

기본 topology의 server metrics 포트:
- `server-1`: `39091`
- `server-2`: `39092`

same-world proof topology는 추가로:
- `server-3`: `39094`

UDP canary/rollback 리허설은 env override로 실행한다:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Observability -EnvFile docker/stack/.env.udp-canary.example
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Observability -EnvFile docker/stack/.env.udp-rollback.example

# 종단 간 리허설(end-to-end, 10분 기준)
pwsh scripts/rehearse_udp_rollout_rollback.ps1

# 재리허설(이미지 재빌드 생략)
pwsh scripts/rehearse_udp_rollout_rollback.ps1 -NoBuild
```

## 종료

```powershell
pwsh scripts/deploy_docker.ps1 -Action down
```

## 참고
- `server_app`은 (실험) chat hook 플러그인을 사용할 수 있다. 기본 스택은 `CHAT_HOOK_ENABLED=0`(비활성)이며, 필요 시 `CHAT_HOOK_ENABLED=1`로 활성화한다. 활성화 시 `CHAT_HOOK_PLUGINS_DIR=/app/plugins`를 우선 사용하고, 디렉터리가 비어 있으면 `CHAT_HOOK_FALLBACK_PLUGINS_DIR=/app/plugins_builtin`으로 이미지 내 샘플 플러그인을 로드한다. (`server/README.md` 참고)
- Lua cold-hook 샘플 스크립트는 `server/scripts/`에서 runtime image `/app/scripts_builtin`으로 복사된다. 기본 스택은 `LUA_ENABLED=0`(비활성)이며, 필요 시 `LUA_ENABLED=1`로 활성화하면 `docker/stack/scripts/*.lua`를 `/app/scripts`로 read-only 마운트해 우선 로드하고, `/app/scripts`가 비어 있거나 읽기 실패면 `LUA_FALLBACK_SCRIPTS_DIR=/app/scripts_builtin`으로 fallback한다.
- 겹치는 샘플 이름은 `server/scripts/`와 `docker/stack/scripts/`에서 같은 function-style hook 내용을 유지하는 것을 기준으로 한다. directive/return-table은 fallback/testing aid로만 남긴다.
- 기본 `haproxy.cfg`는 로컬 검증용 TCP 구성이며, 운영 TLS baseline은 `docker/stack/haproxy/haproxy.tls13.cfg` 템플릿(TLS 1.3 기본 + 레거시 예외 분리 + 내부 mTLS)을 참고한다.
