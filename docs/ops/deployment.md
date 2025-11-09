# 배포 전략 – Dev(Compose)와 Prod(Kubernetes/AWS)

개발·테스트 환경에서 빠르게 반복하면서도 운영 배포 시 일관성을 유지하기 위해 두 가지 흐름을 사용한다. Compose는 로컬 복제 환경을, Kubernetes는 AWS 인프라에 가까운 구성을 제공한다.

## 환경 구분
| 환경 | 설명 |
| --- | --- |
| Dev | Docker Compose로 Postgres, (선택) Redis, `server_app`, write-behind 워커 등을 한 번에 기동 |
| Stage/DR | AWS 관리형 Redis, 자체 Postgres 또는 RDS를 혼합해 운영과 동일한 네트워크에서 검증 |
| Prod | AWS(VPC + ElastiCache + RDS) + Kubernetes/Helm + Terraform(IaC)로 완전 자동화 |

## Dev: Docker Compose
> 목적: 빠른 코드-테스트 반복, smoke 테스트, 클라이언트/서버 동시 기동

- Redis는 기본적으로 관리형 인스턴스를 바라본다. Compose에서 로컬 Redis를 쓰고 싶다면 `.env` 의 `REDIS_URI` 를 `redis://redis:6379` 형태로 지정한다.
- 서버/워커 이미지는 `Dockerfile.dev`를 통해 vcpkg 캐시를 활용해 빌드한다.
- 대표 구성:

```yaml
services:
  postgres:
    image: postgres:15
    environment:
      POSTGRES_USER: app
      POSTGRES_PASSWORD: example
      POSTGRES_DB: appdb
    ports: ["5432:5432"]
    healthcheck:
      test: ["CMD-SHELL", "pg_isready -U app"]
      interval: 5s
      timeout: 3s
      retries: 20
    volumes: [pgdata:/var/lib/postgresql/data]

  server:
    build: .
    command: ["/app/server_app", "5000"]
    env_file: [".env"]
    environment:
      DB_URI: postgres://app:example@postgres:5432/appdb
      REDIS_URI: ${REDIS_URI?set in .env}
    depends_on:
      postgres:
        condition: service_healthy
    ports: ["5000:5000"]

  wb_worker:
    build: .
    command: ["/app/wb_worker"]
    env_file: [".env"]
    environment:
      DB_URI: postgres://app:example@postgres:5432/appdb
    depends_on:
      postgres:
        condition: service_healthy
```

- Windows 개발자는 `scripts/run_all.ps1 -Config Debug -WithClient -Smoke` 를, WSL/Linux는 `scripts/run_all.sh -c Debug -b build-linux -p 5000` 을 사용해 전체 파이프라인을 한번에 확인할 수 있다.

## Prod: AWS + Kubernetes
1. **데이터 계층**  
   - Redis: Amazon ElastiCache (cluster 모드 off, TLS/rediss 권장)  
   - Postgres: Amazon RDS (multi-AZ, 자동 백업, pgBouncer 옵션)
2. **애플리케이션 계층**  
   - `server_app`, `wb_worker`, `wb_dlq_replayer`를 각각 Helm 차트로 배포 (Deployment + HPA)  
   - ConfigMap: `.env` 공통 값, Secret: DB/Redis 자격 증명  
   - ServiceMonitor 및 `/metrics` 엔드포인트를 통해 Prometheus 스크랩
3. **IaC**  
   - Terraform으로 VPC/Subnet/Security Group/Secrets/ElastiCache/RDS를 선언  
   - ArgoCD 혹은 Flux로 Helm 배포를 GitOps 형태로 관리
4. **배포 절차**  
   - `helm upgrade --install server-app charts/server-app -f values/prod.yaml`  
   - 배포 전후 runbook 체크(릴리스 알림, readiness/liveness, smoke 시나리오)

## 운영 체크리스트 요약
1. Dockerfile은 vcpkg 바이너리 캐시를 활용해 빌드 시간을 줄이고, 보안 스캔 통과 여부를 파악한다.  
2. Compose 환경에서도 TLS/Secrets를 동일하게 사용하는 것이 좋다.  
3. Helm 차트에는 다음 리소스가 포함되어야 한다: Deployment/HPA/Service/ConfigMap/Secret/ServiceMonitor.  
4. `/metrics` 와 `metric=*` 로그가 수집되는지 Prometheus·Fluent Bit 파이프라인을 통해 확인한다.  
5. Failover나 DR을 대비해 runbook (`docs/ops/runbook.md`) 의 단계별 체크 항목을 최신 상태로 유지한다.

## 참고 문서
- `docs/getting-started.md` – 로컬 개발 환경 준비  
- `docs/ops/observability.md` – 지표/로그/트레이싱  
- `docs/db/redis-strategy.md`, `docs/db/write-behind.md` – 데이터 계층 전략  
- `docs/ops/runbook.md` – 장애 대응 및 점검 프로세스
