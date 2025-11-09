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

## 사용 예시
`cpp
#include "server/core/net/hive.hpp"
#include "server/core/net/listener.hpp"

int main() {
  boost::asio::io_context io;
  auto hive = std::make_shared<server::core::net::Hive>(io);
  server::core::net::Listener listener(*hive, 6000,
    [](std::shared_ptr<server::core::net::Hive> h) {
      return std::make_shared<server::core::net::Session>(std::move(h));
    });
  listener.start();
  io.run();
}
`
CMake:
`cmake
target_link_libraries(my_app PRIVATE server_core)
target_include_directories(my_app PRIVATE /core/include)
`

## 주요 기능
- **네트워크 추상화**: Hive/Listener/Session 조합으로 비동기 TCP 서버를 손쉽게 구성.
- **작업 큐/스케줄러**: JobQueue, TaskScheduler, DbWorkerPool 로 I/O와 DB 작업 분리.
- **Storage 인터페이스**: IConnectionPool, IRedisClient 로 다른 DB/Redis 구현을 손쉽게 대체.
- **Metrics/Logging**: metrics::Counter/Gauge SPI, structured log 헬퍼 제공.
- **환경/유틸**: .env 로딩, ServiceRegistry, crash handler, paths 유틸리티.

> NOTE: 현재 ServiceRegistry는 편의를 위해 환경 변수에 포인터를 저장하지만, 멀티 프로세스 시나리오에서는 의도치 않은 동작을 유발할 수 있으므로 추후 대체 예정입니다.

## 빌드
`powershell
cmake --build build-msvc --target server_core
`

## 앞으로의 작업
- Hive/Session 개선과 연결된 ECS/멀티샤드 아키텍처는 docs/roadmap.md 에서 추적합니다.
- TaskScheduler/DbWorkerPool 단위 테스트 강화
- Prometheus exporter 와 logfmt 통합을 확대 적용
