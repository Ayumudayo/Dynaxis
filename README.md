# Knights Chat Stack

**Knights**는 본 프로젝트의 정식 이름이 아닙니다.
임시로 대강 붙인 이름입니다.

**Knights**는 C++20로 작성된 고성능 분산 채팅 시스템입니다. 마이크로서비스 아키텍처를 채택하여 확장성을 보장하며, Redis와 PostgreSQL을 활용한 견고한 데이터 처리 파이프라인을 갖추고 있습니다.

![Overall Architecture](Overall Architecture.png)

## 🚀 프로젝트 개요 (Overview)

이 프로젝트는 대규모 트래픽을 처리할 수 있는 채팅 서버 스택을 구현하는 것을 목표로 합니다. 최신 C++ 표준(C++20)과 고성능 비동기 네트워크 라이브러리(Boost.Asio)를 기반으로 하며, 다음과 같은 핵심 가치를 추구합니다.

-   **High Performance**: Lock-free 알고리즘과 비동기 I/O를 적극 활용하여 처리량을 극대화합니다.
-   **Reliability**: 메시지 유실 없는 시스템을 위해 Write-Behind 패턴과 Dead Letter Queue(DLQ)를 구현했습니다.
-   **Scalability**: Gateway, Load Balancer, Server로 역할을 분리하여 수평 확장이 용이합니다.
-   **Observability**: 모든 컴포넌트는 Prometheus 메트릭을 노출하여 실시간 모니터링이 가능합니다.

## 🏗️ 아키텍처 (Architecture)

시스템은 크게 4가지 주요 컴포넌트로 구성됩니다.

1.  **Gateway (`gateway/`)**:
    -   클라이언트의 TCP 연결을 수용하는 진입점입니다.
    -   인증(Authentication), 세션 관리, Heartbeat 처리를 담당합니다.
    -   Load Balancer와 gRPC로 통신하여 트래픽을 백엔드 서버로 전달합니다.

2.  **Load Balancer (`load_balancer/`)**:
    -   Gateway와 Server 사이의 중계 역할을 합니다.
    -   **Consistent Hashing**을 사용하여 유저를 특정 서버에 분배합니다.
    -   **Sticky Session**을 지원하여 재접속 시에도 동일한 서버로 연결되도록 보장합니다.

3.  **Server (`server/`)**:
    -   실제 채팅 로직을 처리하는 핵심 서버입니다.
    -   방(Room) 관리, 메시지 브로드캐스팅, Redis Pub/Sub 연동을 수행합니다.
    -   **Write-Behind** 패턴을 통해 채팅 로그를 비동기로 DB에 저장합니다.

4.  **Core (`core/`)**:
    -   모든 프로젝트에서 공유하는 정적 라이브러리입니다.
    -   네트워크(Session, Listener), 동시성(JobQueue, ThreadManager), 메모리 관리(MemoryPool) 등의 공통 기능을 제공합니다.

## ✨ 주요 기능 (Key Features)

-   **Modern C++20**: Concept, Coroutine(일부), Module(준비 중) 등 최신 문법 활용.
-   **Redis Streams & Pub/Sub**: 분산 환경에서의 메시지 큐 및 실시간 이벤트 전파.
-   **PostgreSQL Storage**: 채팅 기록 및 유저 정보의 영구 저장.
-   **Fault Tolerance**:
    -   Gateway/Server 장애 시 자동 재접속 및 세션 복구.
    -   DB 쓰기 실패 시 Redis DLQ로 이동 후 `wb_worker`가 재처리.
-   **DevClient**: FTXUI 기반의 TUI 클라이언트로 개발 및 테스트 용이성 확보.

## 📂 서브 프로젝트 (Sub-projects)

| 프로젝트 | 경로 | 설명 |
| :--- | :--- | :--- |
| **Core** | [`core/`](core/README.md) | 네트워크, 스레딩, 로깅 등 공용 라이브러리 |
| **Server** | [`server/`](server/README.md) | 채팅 비즈니스 로직 및 데이터 처리 |
| **Gateway** | [`gateway/`](gateway/README.md) | 클라이언트 연결 및 인증 담당 프론트엔드 |
| **Load Balancer** | [`load_balancer/`](load_balancer/README.md) | 트래픽 분산 및 세션 라우팅 |
| **DevClient** | [`devclient/`](devclient/README.md) | 개발자용 TUI 채팅 클라이언트 |
| **Tools** | [`tools/`](tools/README.md) | Write-Behind 워커, 마이그레이션 도구 등 |

## 🛠️ 시작하기 (Getting Started)

### 필수 요구 사항 (Prerequisites)

-   **OS**: Windows 10/11 (Linux 지원 예정)
-   **Compiler**: MSVC 19.3x+ (Visual Studio 2022), Clang 14+, GCC 11+
-   **Build System**: CMake 3.20+
-   **Dependency Manager**: vcpkg
-   **Infrastructure**:
    -   Redis 6.0+
    -   PostgreSQL 13+

### 환경 설정 (Configuration)

프로젝트 루트의 `.env` 파일을 설정해야 합니다. (`.env.example` 참고)

```ini
# Database
DB_URI=postgresql://user:pass@localhost:5432/knights
REDIS_URI=redis://localhost:6379

# Network
SERVER_PORT=5000
GATEWAY_PORT=6000
LB_GRPC_PORT=7001

# Monitoring
METRICS_PORT=9090
```

### 빌드 및 실행 (Build & Run)

PowerShell 스크립트를 통해 간편하게 전체 스택을 빌드하고 실행할 수 있습니다.

**1. 빌드**

```powershell
# 전체 프로젝트 빌드 (Debug)
scripts/build.ps1 -Config Debug
```

**2. 실행**

각 컴포넌트를 개별 터미널에서 실행하거나, `run_all.ps1`을 사용하세요.

```powershell
# 서버 실행
.\build-msvc\server\Debug\server_app.exe 5000

# 로드 밸런서 실행
.\build-msvc\load_balancer\Debug\load_balancer_app.exe

# 게이트웨이 실행
.\build-msvc\gateway\Debug\gateway_app.exe

# 클라이언트 실행
.\build-msvc\devclient\Debug\dev_chat_cli.exe
```

## 🧪 테스트 (Testing)

**단위 테스트 (Unit Tests)**

```powershell
cmake --build build-msvc --target chat_history_tests
ctest --test-dir build-msvc/tests
```

**통합 스모크 테스트 (Smoke Test)**

서버, 게이트웨이, 로드밸런서, 클라이언트를 모두 띄우고 메시지 송수신을 검증합니다.

```powershell
scripts/run_all.ps1 -Config Debug -WithClient -Smoke
```

## 📚 문서 (Documentation)

더 깊이 있는 기술적인 내용은 `docs/` 디렉토리에서 확인할 수 있습니다.

-   [**Repository Structure**](docs/repo-structure.md): 프로젝트 구조 설명
-   [**Server Architecture**](docs/server-architecture.md): 서버 상세 아키텍처
-   [**Redis Strategy**](docs/db/redis-strategy.md): Redis 활용 전략 (Streams, Pub/Sub)
-   [**Write-Behind Pattern**](docs/db/write-behind.md): 쓰기 지연 처리 패턴 상세
-   [**Observability**](docs/ops/observability.md): 모니터링 및 로깅 가이드
