# Deployment Guide

이 문서는 Dev(Compose)와 Prod(Kubernetes/AWS) 환경을 모두 아우르는 배포 절차를 정리한다. “왜 이 단계를 수행하는가?” 까지 설명해 운영자가 그대로 따라할 수 있도록 작성했다.

## 1. 공통 준비물
| 항목 | Dev | Stage/Prod |
| --- | --- | --- |
| OS | Windows 11 + PowerShell 7 / WSL2 Ubuntu | Amazon Linux 2 / Ubuntu 22.04 |
| 필수 도구 | CMake 3.20+, vcpkg, Docker Desktop | kubectl, helm, terraform, aws cli |
| 외부 서비스 | Redis 6+, PostgreSQL 13+ | Amazon ElastiCache, Amazon RDS |
| 필수 Secrets | `DB_URI`, `REDIS_URI`, `JWT_SECRET` | AWS Secrets Manager 또는 KMS |

`.env` 예시는 `docs/configuration.md` 를 참고한다. 환경별로 `.env.server`, `.env.gateway`, `.env.lb` 를 분리해 ConfigMap/Secret 에 주입하는 것을 권장한다.

## 2. Dev 환경 (Docker Compose)
### 2.1 실행 방법
```bash
# Windows PowerShell
scripts/run_all.ps1 -Config Debug -WithClient

# WSL/Linux
bash scripts/run_all.sh -c Debug -b build-linux -p 5000
```
위 스크립트는 Postgres 컨테이너를 올린 뒤 `server_app`, `load_balancer_app`, `gateway_app`, `dev_chat_cli` 까지 한 번에 기동한다. Redis는 기본적으로 외부 인스턴스를 바라보므로, 로컬 Redis 를 쓰고 싶다면 `.env` 에 `REDIS_URI=redis://redis:6379` 를 지정하고 compose 파일에 redis 서비스를 추가한다.

### 2.2 Compose 파일 핵심
- `depends_on.condition=service_healthy` 로 Postgres 준비 여부를 보장
- `volumes: pgdata` 로 데이터 유지
- 서버 컨테이너에 `METRICS_PORT=9090` 노출 후 `docker-compose logs -f server` 로 확인

### 2.3 Dev 검증 체크리스트
1. `curl http://localhost:9090/metrics` 200 OK
2. devclient 로 `/login dev` → `/chat hello` 성공
3. `scripts/smoke_wb.ps1` 실행 시 write-behind ok

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
helm upgrade --install load-balancer charts/load-balancer -f values/prod.yaml
helm upgrade --install gateway charts/gateway -f values/prod.yaml
```
각 차트에는 Deployment(+HPA), Service, ConfigMap(.env), Secret(DB/Redis), ServiceMonitor(/metrics), PodDisruptionBudget가 포함돼야 한다.

### 3.3 배포 검증 순서
1. `kubectl rollout status deploy/server-app`
2. `kubectl port-forward svc/server-metrics 9090:9090` 후 `/metrics` 확인
3. devclient 혹은 e2e 테스트로 `/login` → `/join lobby` → `/chat` 시나리오 수행
4. Grafana 대시보드에서 Active Sessions, LB Idle Close Rate, wb_pending 을 5분간 모니터링

### 3.4 롤백
```bash
helm rollback server-app <REV>
helm rollback load-balancer <REV>
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
