# 배포 전략 — Dev(Compose)와 Prod(Kubernetes/매니지드)

본 문서는 개발/테스트 환경과 실제 운영 환경에서의 배포 방식을 제안합니다.

## 환경 옵션 개요
- Dev/로컬: Docker Compose로 Postgres(+선택 Redis), server_app, 워커를 일괄 기동
- 하이브리드(현재 사례): 매니지드 Redis + 로컬 Postgres
- Prod 권장: 매니지드 Redis/PG + Kubernetes(Helm)로 server/worker 배포, IaC로 인프라 정의

## Dev: Docker Compose(권장)
- 장점: 재현성, 온보딩, 버전 고정, one‑command up/down, 헬스체크/의존 순서 제어
- 현재 Redis가 매니지드인 경우, Compose 스택에 Redis는 포함하지 않고 `REDIS_URI`만 주입

### docker-compose.dev.yml(예시)
```yaml
version: "3.9"
services:
  postgres:
    image: postgres:15
    environment:
      POSTGRES_PASSWORD: example
      POSTGRES_USER: app
      POSTGRES_DB: appdb
    ports:
      - "5432:5432"
    healthcheck:
      test: ["CMD-SHELL", "pg_isready -U app"]
      interval: 5s
      timeout: 3s
      retries: 20
    volumes:
      - pgdata:/var/lib/postgresql/data

  server:
    image: local/server_app:dev  # 또는 빌드 산출물 마운트
    build: .
    command: ["/app/server_app", "5000"]
    env_file: [".env"]
    environment:
      DB_URI: postgres://app:example@postgres:5432/appdb
      # REDIS_URI는 매니지드/외부 인스턴스 주소 사용
    depends_on:
      postgres:
        condition: service_healthy
    ports:
      - "5000:5000"

  wb_worker:
    image: local/wb_worker:dev
    build: .
    command: ["/app/wb_worker"]
    env_file: [".env"]
    environment:
      DB_URI: postgres://app:example@postgres:5432/appdb
    depends_on:
      postgres:
        condition: service_healthy

  wb_dlq_replayer:
    image: local/wb_dlq_replayer:dev
    build: .
    command: ["/app/wb_dlq_replayer"]
    env_file: [".env"]
    environment:
      DB_URI: postgres://app:example@postgres:5432/appdb
    depends_on:
      postgres:
        condition: service_healthy

volumes:
  pgdata: {}
```

메모
- 로컬 바이너리를 쓰는 경우 bind mount로 `build-msvc/server/Debug/server_app.exe` 등을 컨테이너에 마운트하거나, 멀티스테이지 Dockerfile로 빌드 이미지를 생성합니다.
- REDIS_URI는 매니지드(예: `rediss://…`) 주소를 그대로 사용합니다.
- 보안: dev 스택은 공개 네트워크에 노출하지 않습니다.

## Prod: 매니지드 + Kubernetes(권장)
- 데이터 계층: 매니지드 Redis(예: ElastiCache) / 매니지드 Postgres(예: RDS)
- 워크로드: Kubernetes + Helm 차트
  - server_app: Deployment(+HPA), Service, ConfigMap(.env), Secret(DB/Redis URI)
  - wb_worker/wb_dlq_replayer: Deployment, 가용성/동시성 파라미터 조정
  - 관측: ServiceMonitor(/metrics), 로그 수집(예: Fluent Bit)
- 배포: 롤링 업데이트 + 드레인(runbook 참고), readinessProbe/livenessProbe
- IaC: Terraform로 VPC/Subnet/SG/SecretManager/ElastiCache/RDS 등 관리

## 현재 구성(하이브리드) 운영 팁
- 매니지드 Redis + 로컬 Postgres 시
  - 네트워크 지연/방화벽 확인(SSL 필요 시 rediss 스킴)
  - 로컬 Postgres 백업/업데이트 주기 관리
  - `/metrics` 및 최소 지표 로그로 병목 파악

## 관련 문서
- 빠른 시작: `docs/getting-started.md`
- 관측/메트릭: `docs/ops/observability.md`
- 드레인/알람: `docs/ops/runbook.md`
- Redis 전략: `docs/db/redis-strategy.md`
- Write-behind: `docs/db/write-behind.md`
