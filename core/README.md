# server_core 라이브러리

Knights의 `core/` 모듈은 공용 C++20 런타임 라이브러리(`server_core`)를 제공하여 모든 서버 구성요소가 공유하는 네트워크, 동시성, 설정, 스토리지 유틸리티를 담당합니다. Hive/Connection 기반 비동기 I/O 루프와 TaskScheduler, DbWorkerPool 같은 인프라가 이 계층에서 구현되어 있으며, 상위 애플리케이션(`server_app`, `gateway_app`, `load_balancer_app`, `dev_chat_cli`)이 이를 링크하여 동작합니다.

## 디렉터리 구조
- `include/server/core/net/` — Hive, Listener, Session, Connection 등 네트워크 런타임
- `include/server/core/concurrent/` — `JobQueue`, `ThreadManager`, `TaskScheduler` 등 동시성 유틸리티
- `include/server/core/storage/` — 추상 `IConnectionPool`, `DbWorkerPool` 등 백엔드 비동기 작업 지원
- `include/server/core/util/` — `log`, `paths`, `service_registry`, `crash_handler` 등 공용 도구
- `include/server/core/config/` — `.env` 로더와 옵션 파서
- `include/server/core/memory/` — `MemoryPool` 기반 고정 블록 할당기
```text
core/
├─ include/
│  └─ server/
│     └─ core/
│        ├─ concurrent/
│        ├─ config/
│        ├─ memory/
│        ├─ metrics/
│        ├─ net/
│        ├─ protocol/
│        ├─ state/
│        ├─ storage/
│        └─ util/
├─ src/
└─ tests/ (planned)
```

- `src/` — 위 헤더의 구현부
- `tests/` (루트) — 향후 `server_core` 유닛 테스트가 배치될 예정

## 주요 기능
- **네트워크 런타임**: Boost.Asio 기반 `Hive + Listener + Connection` 패턴으로 단일 스레드 이벤트 루프를 노출하며, Gateway·LoadBalancer·Server 모두 동일한 추상화를 사용합니다.
- **동시성 도구**: `LockedQueue`, `LockedWaitQueue`, `TaskScheduler`로 지연/반복 작업과 백그라운드 Job을 안전하게 처리합니다.
- **스토리지 워커 풀**: `DbWorkerPool`이 PostgreSQL/Redis 작업을 비동기 처리하고 메트릭, 종료 시 플러시를 제공합니다.
- **Redis Streams 유틸리티**: `server/core/storage/redis`가 write-behind용 Streams/XADD API를 제공하여 `wb_worker`와 연동됩니다.
- **환경 구성**: `.env` 및 실행 파일 인접 디렉터리의 설정을 로드하여 프로세스별 구성 값을 주입합니다.
- **유틸리티 집합**: 구조화된 `log` 버퍼, CrashHandler, ServiceRegistry, 경로 유틸로 공통 부팅 로직을 단순화합니다.

## 빌드 및 사용
```powershell
# 루트에서 CMake configure 후
cmake --build build-msvc --target server_core
```
다른 타깃은 `target_link_libraries(<target> PRIVATE server_core)` 형태로 의존성을 추가합니다. 헤더는 `target_include_directories`에서 `core/include` 경로를 노출합니다.

## 추가 참고 자료
- 전체 아키텍처 개요: `docs/server-architecture.md`
- 엔진화 로드맵 및 Sapphire 기반 활용 아이디어: `docs/sapphire_core_insights.md`

기능 확장 시에는 `.env` 기반 구성, Hive 패턴, TaskScheduler/DbWorkerPool 계약을 준수하여 모듈 간 일관성을 유지하세요.
