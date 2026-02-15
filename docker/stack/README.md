# Docker Stack (Verification)

`docker/stack/docker-compose.yml`는 검증/스모크 테스트용으로 전체 스택을 Docker로 기동한다.

구성:
- Client(호스트) -> HAProxy(container) -> gateway_app(containers) -> server_app(containers)
- Redis/Postgres는 compose로 함께 띄운다.

## 실행

권장: `scripts/deploy_docker.ps1`를 사용한다. (base 이미지 빌드/compose profile/포트 매핑을 일관되게 유지)

```powershell
# Stack up (build + detached)
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build

# Stack up + Observability(Prometheus/Grafana)
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build -Observability

# 또는 wrapper 사용
pwsh scripts/run_full_stack_observability.ps1
```

접속:
- 게임 트래픽: `127.0.0.1:6000` (HAProxy)
- HAProxy stats: `http://127.0.0.1:8404/`
- gateway metrics: `http://127.0.0.1:36001/metrics`, `http://127.0.0.1:36002/metrics`
- server metrics: `http://127.0.0.1:39091/metrics`, `http://127.0.0.1:39092/metrics`
- wb_worker metrics: `http://127.0.0.1:39093/metrics`
- (옵션) Prometheus: `http://127.0.0.1:39090/`
- (옵션) Grafana: `http://127.0.0.1:33000/` (admin password: `GRAFANA_ADMIN_PASSWORD`, 기본 `admin`)

## 종료

```powershell
pwsh scripts/deploy_docker.ps1 -Action down
```

## Notes
- `server_app`은 (실험) chat hook 플러그인을 사용할 수 있다. 기본 스택은 `CHAT_HOOK_PLUGINS_DIR=/app/plugins`로 샘플 플러그인을 로드한다. (`server/README.md` 참고)
