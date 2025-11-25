# server_core

`core`는 Knights 프로젝트 전역에서 공유하는 C++20 정적 라이브러리(Static Library)입니다.
Boost.Asio 기반의 고성능 네트워크 계층(Hive/Session/Listener), 멀티스레딩 지원(JobQueue, TaskScheduler), 메모리 관리(MemoryPool), 그리고 로깅 및 유틸리티를 제공합니다.

## 주요 기능

### 1. 네트워크 (Network)
- **비동기 I/O**: Boost.Asio `io_context`와 `strand`를 활용한 Lock-Free에 가까운 동시성 모델.
- **세션 관리**: `Session` 클래스를 통해 TCP 연결의 생명주기를 관리하며, **Heartbeat(MSG_PING)** 및 **Gather-Write** 최적화를 지원합니다.
- **패킷 처리**: `Dispatcher`를 통해 수신된 패킷을 적절한 핸들러로 라우팅합니다.

### 2. 동시성 (Concurrency)
- **JobQueue**: 특정 컨텍스트(예: 방, 유저) 내에서 순차적 실행을 보장하는 작업 큐.
- **TaskScheduler**: 지연된 작업이나 주기적인 작업을 스케줄링.
- **ThreadManager**: 워커 스레드 풀을 관리하고 `io_context`를 구동.

### 3. 메모리 (Memory)
- **MemoryPool**: 고정 크기 블록 할당을 통해 메모리 파편화를 방지하고 할당/해제 속도를 최적화.
- **BufferManager**: 네트워크 송수신 버퍼를 풀링하여 관리.

### 4. 유틸리티 (Utility)
- **Async Logger**: 백그라운드 스레드를 이용한 비동기 로깅으로 메인 로직 스레드의 블로킹 방지.
- **Config**: `.env` 파일 로딩 및 옵션 관리.

## 디렉터리 구조

```
core/
├─ include/server/core/
│  ├─ concurrent/  # JobQueue, ThreadManager, TaskScheduler
│  ├─ config/      # dotenv, options
│  ├─ memory/      # BufferManager, MemoryPool
│  ├─ metrics/     # Runtime metrics (Counter/Gauge)
│  ├─ net/         # Hive, Listener, Session, Connection, Dispatcher
│  ├─ protocol/    # Packet definition, codec
│  ├─ state/       # SharedState (global server state)
│  ├─ storage/     # DB/Redis interfaces
│  └─ util/        # Log, CrashHandler, ServiceRegistry
└─ src/            # 구현체 (.cpp)
```

## 빌드 방법 (Build)

이 프로젝트는 **CMake**와 **Visual Studio 2022**를 지원하며, `vcpkg`를 통해 의존성을 관리합니다.

### 필수 요구 사항
- Visual Studio 2022 (C++ Desktop Development 워크로드)
- CMake 3.20 이상
- Git

### 빌드 명령
프로젝트 루트에서 다음 명령을 실행하여 빌드할 수 있습니다.

```powershell
# VS2022 Debug 빌드 (Presets 사용)
cmake --build --preset windows-vs2022-debug

# 또는 Release 빌드
cmake --build --preset windows-vs2022-relwithdebinfo
```

## 사용 예시 (Example)

서버를 구동하는 기본적인 코드는 다음과 같습니다. (의사 코드)

```cpp
#include "server/core/net/listener.hpp"
#include "server/core/concurrent/thread_manager.hpp"

int main() {
    // 1. 설정 및 의존성 초기화
    auto options = std::make_shared<SessionOptions>();
    auto shared_state = std::make_shared<SharedState>();
    BufferManager buffer_manager;
    Dispatcher dispatcher;

    // 2. 스레드 매니저 시작 (io_context 구동)
    ThreadManager thread_manager(4); // 4 threads

    // 3. 리스너 설정 및 시작
    // Listener는 내부적으로 Session을 생성하고 Dispatcher와 연결합니다.
    Listener listener(thread_manager.get_io_context(), 
                      6000, // Port
                      dispatcher, 
                      buffer_manager, 
                      options, 
                      shared_state);
    
    listener.start();

    // 4. 서버 실행 대기
    thread_manager.join();
}
```

## 최신 변경 사항 (Recent Changes)
- **Async Logging**: `log::info`, `log::error` 등이 이제 비동기로 동작하여 성능 저하를 최소화합니다.
- **Heartbeat**: `Session`은 설정된 주기마다 자동으로 `MSG_PING`을 전송하여 연결 상태를 확인합니다.
- **Gather-Write**: 여러 개의 작은 패킷을 보낼 때 `gather-write`를 사용하여 시스템 콜 비용을 줄였습니다.
