# server_core

core/ 는 Knights 전역에서 공유하는 C++20 static library 입니다. Boost.Asio 기반 네트워크 레이어(Hive/Session/Listener), JobQueue·TaskScheduler, Redis/DB 래퍼, dotenv/로깅 유틸리티 등을 한 번에 제공합니다.

## 디렉터리 구조
`
core/
├─ include/server/core/
│  ├─ concurrent/  # JobQueue, ThreadManager, TaskScheduler
│  ├─ config/      # dotenv, options
│  ├─ memory/      # BufferManager, MemoryPool
│  ├─ metrics/     # Counter/Gauge/Histogram SPI
│  ├─ net/         # Hive, Listener, Session, Connection
│  ├─ protocol/    # opcode, frame codec, errors
│  ├─ state/       # InstanceRegistry helpers
│  ├─ storage/     # DB/Redis 추상화
│  └─ util/        # log, crash_handler, paths, service_registry
└─ src/            # 구현체
`

## 핵심 기능
- **네트워크 추상화**: core::net::Hive, Listener, Session 조합으로 비동기 TCP 서버를 손쉽게 구성합니다.
- **작업 큐/스케줄러**: JobQueue, TaskScheduler, DbWorkerPool 로 I/O와 DB 작업을 분리합니다.
- **Storage 인터페이스**: IConnectionPool, IRedisClient 로 다른 DB/Redis 구현을 쉽게 교체할 수 있습니다.
- **환경/유틸**: .env 로딩, ServiceRegistry, crash handler, structured logging 제공.

## 빌드
`powershell
cmake --build build-msvc --target server_core
`
다른 타깃에서 사용하려면:
`cmake
target_link_libraries(my_app PRIVATE server_core)
target_include_directories(my_app PRIVATE /core/include)
`

## 앞으로의 작업
- Hive/Session 개선과 연결된 ECS/멀티샤드 아키텍처는 docs/roadmap.md 에서 추적합니다.
- TaskScheduler/DbWorkerPool 단위 테스트를 보강하고, Prometheus exporter 를 확대 적용할 계획입니다.
