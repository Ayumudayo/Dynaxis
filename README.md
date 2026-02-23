# Knights Chat Stack

**Knights**는 본 프로젝트의 정식 이름이 아닙니다.
임시로 대강 붙인 이름입니다.

**Knights**는 AI 에이전트를 사용해 C++20로 작성된 고성능 분산 채팅 시스템입니다. 마이크로서비스 아키텍처를 채택하여 확장성을 보장하며, Redis와 PostgreSQL을 활용한 견고한 데이터 처리 파이프라인을 갖추고 있습니다.

## 🚀 프로젝트 개요

이 프로젝트는 대규모 트래픽을 처리할 수 있는 채팅 서버 스택을 구현하는 것을 목표로 합니다. 최신 C++ 표준(C++20)과 고성능 비동기 네트워크 라이브러리(Boost.Asio)를 기반으로 하며, 다음과 같은 핵심 가치를 추구합니다.

-   **고성능(High Performance)**: Lock-free 알고리즘과 비동기 I/O를 적극 활용하여 처리량을 극대화합니다.
-   **신뢰성(Reliability)**: 메시지 유실 없는 시스템을 위해 Write-Behind 패턴과 Dead Letter Queue(DLQ)를 구현했습니다.
-   **확장성(Scalability)**: (외부) TCP 로드밸런서(예: HAProxy) + Gateway + Server로 역할을 분리하여 수평 확장이 용이합니다.
-   **관측성(Observability)**: 모든 컴포넌트는 Prometheus 메트릭을 노출하여 실시간 모니터링이 가능합니다.

## 🏗️ 아키텍처

시스템은 크게 5가지 주요 컴포넌트로 구성됩니다.

1.  **Edge Load Balancer (예: HAProxy)**:
    -   외부 TCP(L4) 로드밸런서로, 다수의 `gateway_app` 인스턴스로 클라이언트 연결을 분산합니다.
    -   애플리케이션 프로토콜(opcode)은 해석하지 않습니다.

2.  **Gateway (`gateway/`)**:
    -   클라이언트의 TCP 연결을 수용하는 진입점입니다.
    -   인증(Authentication), 세션 관리, Heartbeat 처리를 담당합니다.
    -   **Service Discovery**: Redis를 통해 서버 인스턴스를 찾아, **Least Connections** 방식으로 트래픽을 분산합니다.
    -   **Session Stickiness**: 재접속 시 이전 세션 정보를 바탕으로 동일한 서버로 라우팅을 시도합니다.

3.  **Server (`server/`)**:
    -   실제 채팅 로직을 처리하는 핵심 서버입니다.
    -   방(Room) 관리, 메시지 브로드캐스팅, Redis Pub/Sub 연동을 수행합니다.
    -   **Write-Behind** 패턴을 위해 Redis Streams에 이벤트를 적재합니다.

4.  **Write-Behind Worker (`wb_worker`)**:
    -   Redis Streams -> Postgres 비동기 적재를 담당합니다.
    -   스택 구동 시 함께 실행되며 `/metrics`를 노출할 수 있습니다.

5.  **Core (`core/`)**:
    -   모든 프로젝트에서 공유하는 정적 라이브러리입니다.
    -   네트워크(Session/TransportConnection, SessionListener/TransportListener), 동시성(JobQueue, ThreadManager), 메모리 관리(MemoryPool) 등의 공통 기능을 제공합니다.

## 아키텍처 다이어그램
```mermaid
flowchart TB
    %% ------------------------------
    %% Styles & Definitions
    %% ------------------------------
    classDef client fill:#333,stroke:#fff,stroke-width:2px,color:#fff
    classDef gateway fill:#1a237e,stroke:#7986cb,stroke-width:2px,color:#fff,rx:5
    classDef server fill:#004d40,stroke:#4db6ac,stroke-width:2px,color:#fff,rx:5
    classDef redis fill:#b71c1c,stroke:#e57373,stroke-width:2px,color:#fff,shape:cylinder
    classDef db fill:#3e2723,stroke:#a1887f,stroke-width:2px,color:#fff,shape:cylinder
    classDef component fill:#455a64,stroke:#90a4ae,stroke-width:2px,color:#fff,rx:5
    classDef network fill:#f5f5f5,stroke:#e0e0e0,stroke-width:2px,stroke-dasharray: 5 5,color:#616161

    %% ------------------------------
    %% Node Structure
    %% ------------------------------
    
    subgraph Clients ["USERS"]
        ClientApp["Client Application"]:::client
    end

    subgraph AccessLayer ["ACCESS LAYER"]
        direction TB
        EdgeLB["Edge LB<br/>(HAProxy, TCP)"]:::component
        Gateway["Gateway Server<br/>(Session & Routing)"]:::gateway
    end

    subgraph ServiceLayer ["SERVICE LAYER"]
        direction TB
        subgraph Cluster ["Game Server Cluster"]
            direction LR
            S1["Server 1"]:::server
            S2["Server 2"]:::server
            Sn["Server N"]:::server
        end
    end

    subgraph StateLayer ["STATE & DATA LAYER"]
        direction TB
        RedisPrimary[("Redis (Hot Data)<br/>Session/PubSub")]:::redis
        
        subgraph Async ["Async Persistence"]
            direction TB
            WbWorker["Write-Behind Worker"]:::component
            Postgres[("PostgreSQL<br/>(Cold Data)")]:::db
        end
    end

    %% ------------------------------
    %% Data Flow
    %% ------------------------------

    %% 1. Connection
    ClientApp ===|"1. TCP Connect"| EdgeLB
    EdgeLB ===|"1. TCP Forward"| Gateway

    %% 2. Routing (Least Connections)
    Gateway ===|"2. Route (Least Conn)"| S1
    Gateway ===|"2. Route"| S2
    Gateway ===|"2. Route"| Sn

    %% 3. Synchronization (Pub/Sub & Heartbeat)
    S1 <-->|"3. Sync/PubSub"| RedisPrimary
    S2 <-->|"3. Sync/PubSub"| RedisPrimary
    Sn <-->|"3. Sync/PubSub"| RedisPrimary

    %% 4. Discovery
    Gateway -.->|"Discovery"| RedisPrimary

    %% 5. Persistence
    RedisPrimary -.->|"Stream (Events)"| WbWorker
    WbWorker -->|"Batch Insert"| Postgres

    %% ------------------------------
    %% Layout Hints
    %% ------------------------------
    style Clients fill:#fff,stroke:none
    style AccessLayer fill:#e8eaf6,stroke:#c5cae9
    style ServiceLayer fill:#e0f2f1,stroke:#b2dfdb
    style StateLayer fill:#ffebee,stroke:#ffcdd2
```

운영/장애 대응 관점의 상세 다이어그램은 `docs/ops/architecture-diagrams.md`를 참고하세요.

## 왜 이 구조인가?

### 1) 왜 HAProxy(L4) + Gateway를 같이 쓰는가
- HAProxy는 TCP 분산에는 매우 강하지만, 애플리케이션 프레임(opcode) 해석이나 인증/세션 스티키 정책은 담당하지 않는다.
- `gateway_app`은 첫 로그인 프레임을 해석해 인증하고, Redis 기반 sticky routing까지 수행한다.
- 즉, **HAProxy는 네트워크 분산**, **Gateway는 애플리케이션 연결 제어**를 맡아 역할 충돌 없이 수평 확장을 가능하게 한다.

### 2) 왜 Redis Pub/Sub이 필요한가
- 사용자들은 여러 `server_app` 인스턴스에 분산되어 붙기 때문에, 한 서버에서 보낸 룸 메시지를 다른 서버 사용자에게도 전달해야 한다.
- 이때 Redis Pub/Sub을 서버 간 백플레인으로 사용하면 노드 수가 늘어나도 브로드캐스트 경로를 단순하게 유지할 수 있다.

### 3) 왜 Write-Behind(Streams -> Worker -> Postgres)인가
- 채팅 요청 경로에서 DB 쓰기를 직접 수행하면 지연과 실패 전파가 커져 실시간 응답성이 급격히 떨어진다.
- 서버는 Redis Streams에 이벤트를 먼저 기록하고, `wb_worker`가 배치로 Postgres에 반영해 hot path 부하를 분리한다.
- 이 구조는 성능(낮은 지연)과 신뢰성(재처리/DLQ)을 동시에 맞추기 위한 절충안이다.

## ✨ 주요 기능

-   **Modern C++20**: Concept, Coroutine(일부), Module(준비 중) 등 최신 문법 활용.
-   **Redis Streams & Pub/Sub**: 분산 환경에서의 메시지 큐 및 실시간 이벤트 전파.
-   **PostgreSQL Storage**: 채팅 기록 및 유저 정보의 영구 저장.
-   **Fault Tolerance**:
    -   Gateway/Server 장애 시 자동 재접속 및 세션 복구.
    -   DB 쓰기 실패 시 Redis DLQ로 이동 후 `wb_worker`가 재처리.
-   **Client GUI**: Dear ImGui 기반의 그래픽 클라이언트로 직관적인 사용성.

## 📂 서브 프로젝트

| 프로젝트 | 경로 | 설명 |
| :--- | :--- | :--- |
| **Core** | [`core/`](core/README.md) | 네트워크, 스레딩, 로깅 등 공용 라이브러리 |
| **Server** | [`server/`](server/README.md) | 채팅 비즈니스 로직 및 데이터 처리 |
| **Gateway** | [`gateway/`](gateway/README.md) | 클라이언트 연결 및 인증 담당 프론트엔드 |
| **Client GUI** | [`client_gui/`](client_gui/README.md) | Dear ImGui 기반 그래픽 채팅 클라이언트 |
| **Tools** | [`tools/`](tools/README.md) | Write-Behind 워커, 마이그레이션 도구 등 |

## 🛠️ 시작하기

### 필수 요구 사항

-   **OS**: Windows 10/11 (개발) + Linux(Docker) 런타임(검증/운영)
-   **Compiler**: MSVC 19.3x+ (Visual Studio 2022), Clang 14+, GCC 11+
-   **Build System**: CMake 3.20+
-   **Dependency Manager**: vcpkg
-   **Infrastructure**:
    -   Redis 6.0+
    -   PostgreSQL 13+

### 환경 설정

애플리케이션은 **OS 환경 변수**를 읽어 설정됩니다.
로컬 개발에서는 `.env.example`를 복사해 `.env`를 만들고, 스크립트들이 이를 로드하도록 사용할 수 있습니다.
(코드 자체에는 `.env` 자동 로더가 없습니다.)

```ini
# 코어(Core)
DB_URI=postgresql://knights:password@127.0.0.1:5432/knights_db
REDIS_URI=tcp://127.0.0.1:6379

# 서버 앱(server_app)
PORT=5000
SERVER_ADVERTISE_HOST=127.0.0.1
SERVER_REGISTRY_PREFIX=gateway/instances/

# 게이트웨이 앱(gateway_app)
GATEWAY_LISTEN=0.0.0.0:6000
GATEWAY_ID=gateway-default

# 메트릭(Metrics)
# 프로세스별 METRICS_PORT 설정입니다. 터미널/스크립트마다 다른 값을 사용하세요.
# 예시: METRICS_PORT=9100
```

### 빌드 및 실행

개발은 Windows에서 하되, 서버 스택(HAProxy/gateway/server/worker)은 **Linux 런타임(Docker Desktop의 Linux containers)** 으로 실행하는 흐름을 표준으로 둡니다.

**1. 빌드**

```powershell
# 전체 프로젝트 빌드 (Debug)
scripts/build.ps1 -Config Debug
```

(옵션) clangd/`compile_commands.json` 생성 (LSP/코드 인텔리전스용):

```powershell
pwsh scripts/configure_windows_ninja.ps1 -CopyCompileCommands
```

`compile_commands.json`는 `.gitignore`에 포함되어 있으므로 로컬에서만 유지하면 됩니다.

**2. 서버 스택 실행 (권장: Docker/Stack)**

```powershell
# 전체 스택 기동(HAProxy 포함, 검증용 표본)
scripts/deploy_docker.ps1 -Action up -Detached -Build
```

접속:
- 게임 트래픽: `127.0.0.1:6000` (HAProxy)
- HAProxy stats: `http://127.0.0.1:8404/`

중지:

```powershell
scripts/deploy_docker.ps1 -Action down
```

**3. 클라이언트 실행**

```powershell
.\build-windows\client_gui\Debug\client_gui.exe
```

## 🧪 테스트

**단위 테스트 (Unit Tests)**

```powershell
cmake --build build-windows --target chat_history_tests
ctest --preset windows-test
```

**통합 스모크 테스트 (Smoke Test)**

Docker 스택(`docker/stack`)을 띄운 뒤 클라이언트로 메시지 송수신을 검증합니다.

```powershell
scripts/deploy_docker.ps1 -Action up -Detached -Build

# (선택) 로그 보기
scripts/deploy_docker.ps1 -Action logs
```

## 📚 문서

더 깊이 있는 기술적인 내용은 `docs/` 디렉토리에서 확인할 수 있습니다.

-   [**Repository Structure**](docs/repo-structure.md): 프로젝트 구조 설명
-   [**Naming & Commenting Conventions**](docs/naming-conventions.md): 네이밍/네임스페이스 + 한국어 Doxygen 주석 규칙
-   [**Server Architecture**](docs/server-architecture.md): 서버 상세 아키텍처
-   [**Redis Strategy**](docs/db/redis-strategy.md): Redis 활용 전략 (Streams, Pub/Sub)
-   [**Write-Behind Pattern**](docs/db/write-behind.md): 쓰기 지연 처리 패턴 상세
-   [**Observability**](docs/ops/observability.md): 모니터링 및 로깅 가이드
-   [**Ops Architecture Diagrams**](docs/ops/architecture-diagrams.md): 제어면/UDP 전환 상세 다이어그램
