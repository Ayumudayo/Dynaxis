# Deployment Guide

이 문서는 Dev(Compose)와 Prod(Kubernetes/AWS) 환경을 모두 아우르는 배포 절차를 정리한다. “왜 이 단계를 수행하는가?” 까지 설명해 운영자가 그대로 따라할 수 있도록 작성했다.

## 1. 공통 준비물
| 항목 | Dev | Stage/Prod |
| --- | --- | --- |
| OS | Windows 11 + PowerShell 7 / WSL2 Ubuntu | Amazon Linux 2 / Ubuntu 22.04 |
| 필수 도구 | CMake 3.20+, vcpkg, Docker Desktop | kubectl, helm, terraform, aws cli |
| 외부 서비스 | Redis 6+, PostgreSQL 13+ | Amazon ElastiCache, Amazon RDS |
| 필수 Secrets | `DB_URI`, `REDIS_URI`, `JWT_SECRET` | AWS Secrets Manager 또는 KMS |

`.env` 예시는 `docs/configuration.md` 를 참고한다. 환경별로 `.env.server`, `.env.gateway` 를 분리해 ConfigMap/Secret 에 주입하는 것을 권장한다. (HAProxy는 별도 설정 파일로 관리)

## 2. Dev 환경 (Docker Compose)
### 2.1 권장: 전체 스택 Docker (HAProxy 포함)
Windows에서 개발하더라도 서버 스택 런타임은 **Linux 컨테이너**로 통일한다.

```powershell
# Windows PowerShell
scripts/deploy_docker.ps1 -Action up -Stack -Detached -Build
```

```bash
# WSL/Linux
docker build -f Dockerfile.base -t knights-base .
docker compose -f docker/stack/docker-compose.yml up -d --build
```

### 2.2 Docker Infra만(선택)
Redis/Postgres만 Compose로 띄우고, 애플리케이션은 WSL/Linux(또는 원격 Linux)에서 실행할 수도 있다.

```powershell
scripts/deploy_docker.ps1 -Action up -Infra -Detached
```

### 2.4 Compose 파일 핵심
- `depends_on.condition=service_healthy` 로 Postgres 준비 여부를 보장
- `volumes: pgdata` 로 데이터 유지
- 서버/게이트웨이에서 `/metrics`를 노출하고 Prometheus/Grafana를 붙일 수 있다.

### 2.5 Dev 검증 체크리스트
1. HAProxy 엔드포인트로 접속: `127.0.0.1:6000`
2. server/gateway metrics 확인(옵션): `/metrics` 200 OK
3. `WRITE_BEHIND_ENABLED=1` 설정 시 `wb_worker`가 backlog를 처리하는지 확인

## 3. Prod 환경 (AWS + Kubernetes)
### 3.1 인프라 (Terraform)
1. `terraform init && terraform apply` 로 VPC/Subnet/SecurityGroup/ElastiCache/RDS/SecretsManager 생성
2. 출력물: `redis_endpoint`, `postgres_endpoint`, `server_advertise_host`
3. SecretsManager 에 DB/Redis URI 저장 → Helm values 에 reference

### 3.2 애플리케이션 배포 (Helm)
```bash
helm upgrade --install server-app charts/server-app -f values/prod.yaml \
  --set env.DB_URI=secret://knights/db_uri \
  --set env.REDIS_URI=secret://knights/redis_uri
helm upgrade --install gateway charts/gateway -f values/prod.yaml
```
각 차트에는 Deployment(+HPA), Service, ConfigMap(환경 변수), Secret(DB/Redis), ServiceMonitor(/metrics), PodDisruptionBudget가 포함돼야 한다.

Edge Load Balancer는 클러스터 외부(예: HAProxy, AWS NLB)에서 `gateway` Service로 TCP 트래픽을 분산한다.

### 3.3 배포 검증 순서
1. `kubectl rollout status deploy/server-app`
2. `kubectl port-forward svc/server-metrics 9090:9090` 후 `/metrics` 확인
3. devclient 혹은 e2e 테스트로 `/login` → `/join lobby` → `/chat` 시나리오 수행
4. Grafana 대시보드에서 Active Sessions, write-behind 지표 등을 5분간 모니터링

### 3.4 롤백
```bash
helm rollback server-app <REV>
helm rollback gateway <REV>
```
롤백 후에도 `/metrics` 와 runbook 체크리스트를 반드시 수행한다.

## 4. 배포 전략 요약
| 상황 | 전략 |
| --- | --- |
| 소규모 수정 | rolling update (Deployment) + readinessProbe 조절 |
| 대규모 schema 변경 | ① migration ② server_app 배포 ③ 워커 배포 순으로 진행 |
| 긴급 Hotfix | Helm Chart 의 `canary` values 로 1대만 교체 후 smoke -> 전체 롤아웃 |

## 5. 참고 자료
- `docs/ops/prewarm.md` – 새 인스턴스 pre-warm
- `docs/ops/runbook.md` – 알람 대응 순서
- `docs/ops/fallback-and-alerts.md` – 장애 시 fallback
