# 운영 아키텍처 다이어그램 (상세)

README의 다이어그램은 개요 전달을 위한 최소 표현이다.
이 문서는 운영/장애 대응에서 자주 보는 제어면(control plane), 관측면(observability), UDP 카나리 전환 흐름을 상세히 보여준다.

## 1) 런타임 + 제어면(운영)

```mermaid
flowchart LR
    subgraph DataPlane["DATA PLANE"]
        Client["Client"]
        HA["HAProxy<br/>TCP L4"]
        GW["gateway_app"]
        SV["server_app cluster"]
        Redis[("Redis<br/>Registry / Session / PubSub / Streams")]
        WB["wb_worker"]
        PG[("PostgreSQL")]

        Client -->|TCP| HA -->|TCP| GW -->|Route| SV
        GW -. discovery/sticky .-> Redis
        SV <-->|fanout Pub/Sub| Redis
        SV -. XADD session_events .-> Redis -->|XREADGROUP| WB -->|Batch Insert| PG
    end

    subgraph ControlPlane["CONTROL PLANE"]
        Browser["Operator Browser"]
        Admin["admin_app"]
        Prom["Prometheus"]
        Graf["Grafana"]

        Browser -->|/admin| Admin
        Admin -->|/api/v1/*| Redis
        Admin -->|/metrics,/readyz| GW
        Admin -->|/metrics,/readyz| SV
        Admin -->|WB_WORKER_METRICS_URL| WB

        Prom -->|scrape| GW
        Prom -->|scrape| SV
        Prom -->|scrape| WB
        Prom -->|HTTP API| Admin
        Graf -->|query| Prom
    end
```

핵심 의도:
- 데이터면(게임 트래픽)과 제어면(운영 API/UI)을 분리해 장애 대응 시 영향 반경을 줄인다.
- `admin_app`은 운영자 단일 진입점이며, Redis/서비스 메트릭/Prometheus를 집계한다.

## 2) UDP 카나리 전환/롤백

```mermaid
flowchart TB
    CanaryEnv["docker/stack/.env.udp-canary.example"] --> DeployCanary["deploy_docker.ps1 -Observability -EnvFile canary"]
    DeployCanary --> GW1["gateway-1<br/>UDP ON"]
    DeployCanary --> GW2["gateway-2<br/>UDP OFF"]

    GW1 --> M1["gateway_udp_enabled 1"]
    GW2 --> M2["gateway_udp_enabled 0"]
    M1 --> Prom["Prometheus"]
    M2 --> Prom

    Prom --> Alert["UDP 품질/보안 알림<br/>loss, jitter, bind abuse"]
    Alert --> Decision{"알림 발생?"}

    Decision -- 아니오 --> Expand["점진 확장<br/>gateway-2 UDP ON"]
    Decision -- 예 --> RollbackEnv["docker/stack/.env.udp-rollback.example"]
    RollbackEnv --> DeployRollback["deploy_docker.ps1 -EnvFile rollback"]
    DeployRollback --> TCPOnly["모든 gateway_udp_enabled 0"]
    TCPOnly --> Smoke["TCP smoke: verify_pong.py"]
```

관련 문서:
- `docs/ops/udp-rollout-rollback.md`
- `docs/ops/observability.md`
- `docs/ops/admin-console.md`
