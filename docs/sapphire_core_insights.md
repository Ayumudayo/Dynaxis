# 코어 인프라에서 활용 가능한 Sapphire 패턴

## 1. 서비스 로케이터와 모듈 간 공유 레지스트리
- **핵심 개념**: `Common::Service` 템플릿이 타입을 키로 삼아 `shared_ptr`를 저장하고, 모든 DLL/실행 파일이 동일 슬롯을 참조한다. (`Sapphire/src/common/Service.h:33`, `Sapphire/src/common/Service.h:70`, `Sapphire/src/common/ServiceRegistry.cpp:18`)
- **왜 좋은가**: 크로스 모듈 환경에서도 전역 싱글턴 충돌을 피하면서 안전하게 객체 생애주기를 관리한다. 타입 정보에서 `class`/`struct` 접두를 제거해 MSVC 빌드에서도 키가 일치한다. (`Sapphire/src/common/Service.h:73`)
- **Knights 적용 방안**: 현재 `server/core`가 의존성 주입을 수동으로 넘겨주는 부분(예: `run_server`에서 여러 매니저를 생성 후 람다로 캡처)을 `Common::Service` 스타일 레지스트리로 대체하면, 향후 모듈화된 툴(별도 CLI, 테스트 바이너리)에서도 동일 로거/커넥션 풀을 손쉽게 공유할 수 있다. 특히 Windows에서 DLL 플러그인 확장을 고려한다면 환경 변수에 레지스트리 포인터를 저장하는 기법이 즉시 재사용 가능하다. (`Sapphire/src/common/ServiceRegistry.cpp:19`)

## 2. 설정 로딩과 기본값 복제 규약
- **핵심 개념**: `ConfigMgr`가 실행 파일 위치 기준 `config/*.ini`를 읽고, 없으면 `.default` 파일을 복사한다. 형식화된 `getValue` 템플릿으로 정수·실수·문자열·불리언을 타입 안전하게 꺼낸다. (`Sapphire/src/common/Config/ConfigMgr.h:16`, `Sapphire/src/common/Config/ConfigMgr.h:25`)
- **왜 좋은가**: 개발 초기에는 샘플 설정으로 부트스트랩하고, 배포 환경에서는 값 오버라이드만 해도 된다. 타입별 분기 덕분에 잘못된 형 변환을 컴파일 단계에서 차단한다.
- **Knights 적용 방안**: 현재 `.env` 기반 초기화에 추가로, 복수 실행 바이너리를 위한 공통 설정 팩토리를 만들 때 `ConfigMgr`를 참고하면 된다. 예를 들어 테스트 툴이 `config/*.default`를 자동 복사하도록 하면 온보딩/CI 설정 누락을 줄일 수 있다.

## 3. ASIO 기반 네트워크 스켈레톤
- **핵심 개념**: `Hive`가 `io_service::work` 생애주기를 관리하고, `Acceptor`/`Connection`이 strand에서 읽기·쓰기·에러 처리를 표준화한다. (`Sapphire/src/common/Network/Hive.cpp:9`, `Sapphire/src/common/Network/Acceptor.cpp:43`, `Sapphire/src/common/Network/Connection.cpp:26`)
- **추가 관찰**: `Hive::stop()`은 shutdown 플래그를 원자적으로 설정한 뒤 `work_ptr`을 해제하고 no-op를 post하여 블로킹된 `run()`을 깨운다. 덕분에 단일 쓰레드에서 Hive를 돌리다가도 안전하게 중단·재시작(`reset()`)이 가능하다. (`Sapphire/src/common/Network/Hive.cpp:39`)
- **왜 좋은가**: 접속 수락부터 세션 종료까지 공통 코드로 묶여 재사용성이 높고, `strand`로 직렬화돼 동시성 버그를 줄인다. `m_work_ptr.reset()` + 더미 post로 `run()`을 안전하게 빠져나오는 패턴도 재활용 가치가 있다. (`Sapphire/src/common/Network/Hive.cpp:39`)
- **Knights 적용 방안**: Knights도 `core::net::SessionListener`/`core::net::Session`(legacy base: `core::Acceptor`/`Session`)을 쓰지만, 별도 경량 서비스(예: 상태 수집기, 내부 RPC) 작성 시 위 스켈레톤을 참고하면 수신 버퍼 크기·큐 관리 등 기본기 구현 시간을 대폭 줄일 수 있다. 특히 `Connection::dispatchRecv`/`dispatchSend` 큐 구조를 가져오면 사용자 정의 프레임을 쉽게 삽입 가능하다. (`Sapphire/src/common/Network/Connection.cpp:129`)
- **활용 예시**: Sapphire는 월드 서버에서 Hive를 생성해 전용 네트워크 스레드를 돌리고(`Sapphire/src/world/WorldServer.cpp:371`), 로비 서버는 `make_Hive()`로 공유 Hive를 만들어 여러 acceptor와 세션을 묶는다. 이렇게 “1 Hive = 1 io_context + work” 관례를 지키면 Knights에서도 멀티 서비스 구성이 쉬워진다.
- **장기 계획**: 현재 Knights는 단일 io_context 구성으로 충분해 Hive 패턴을 즉시 도입하지 않지만, 향후 게이트웨이‒로드 밸런서‒다중 서버 인스턴스 구조로 확장될 때 서비스별 `io_context + work guard` 번들을 독립 운용해야 하므로, Sapphire식 Hive/Connection 레이어를 재검토하고 기반 클래스를 미리 일반화해 둘 예정이다.

## 4. 복합 프레임 조립·파싱 파이프라인
- **핵심 개념**: `PacketContainer`가 여러 IPC 세그먼트를 하나의 버퍼로 정렬하고, `GamePacketParser`가 헤더/세그먼트 길이를 검증하면서 큐에 쌓는다. (`Sapphire/src/common/Network/PacketContainer.cpp:33`, `Sapphire/src/common/Network/GamePacketParser.cpp:9`)
- **왜 좋은가**: 불완전/과대 프레임을 조기에 차단하고, align·padding을 자동 처리해 핸들러는 순수 payload만 다룬다. 최대 크기 제한으로 DoS를 방어하고, 재사용 가능한 헬퍼(`checkHeader`, `checkSegmentHeader`)를 제공한다. (`Sapphire/src/common/Network/GamePacketParser.cpp:111`)
- **Knights 적용 방안**: 현재 `server/core/protocol`은 고정 헤더 기반이지만, 향후 멀티 메시지 프레임(예: 배치 전송)을 추가할 때 Sapphire 구조를 그대로 이식하면 프레임 혼합 전송을 안정적으로 지원할 수 있다. 또한 `PacketContainer::setTargetActor` 패턴은 Knights 메시지 브로드캐스트 시 다중 수신자 ID를 주입하는 데 응용 가능하다. (`Sapphire/src/common/Network/PacketContainer.cpp:53`)

## 5. DB 워커 풀과 큐잉 전략
- **핵심 개념**: `DbWorkerPool`이 비동기 실행용 큐(`LockedWaitQueue`)와 동기 연결 풀을 나눠 관리하고, keep-alive·escapeString까지 제공한다. (`Sapphire/src/common/Database/DbWorkerPool.h:34`, `Sapphire/src/common/Database/DbWorkerPool.h:85`, `Sapphire/src/common/Util/LockedQueue.h:66`)
- **왜 좋은가**: 장기 실행 SQL을 비동기 스레드로 넘겨 게임 루프 블로킹을 피하고, 준비된 문장 인덱스를 타입으로 강제해 잘못된 prepared statement 호출을 예방한다. 큐에서 `push_reset`을 제공해 `shared_ptr` 레퍼런스를 깨끗하게 비운다. (`Sapphire/src/common/Util/LockedQueue.h:112`)
- **Knights 적용 방안**: Knights는 Postgres/Redis 전용 인터페이스가 있으므로, Sapphire의 큐잉 구조를 래핑해 `JobQueue`와 결합하면 백그라운드 DB 작업(로그 적재, 감사 이벤트)을 안정적으로 분리할 수 있다. 또한 keep-alive 호출 패턴은 장기 연결형 Postgres 세션에도 유용하다. (`Sapphire/src/world/WorldServer.cpp:497`)

## 6. 경량 지연 작업 스케줄러
- **핵심 개념**: `TaskMgr`는 현재 tick에서 실행 여부만 검사하고, 지연 큐를 `std::queue`로 유지해 이후 tick에서 합류시킨다. (`Sapphire/src/world/Manager/TaskMgr.cpp:10`)
- **왜 좋은가**: 복잡한 우선순위 큐 없이도 지연 실행과 재큐잉을 단순하게 구현하고, 작업 객체가 `onQueue`/`execute`로 생애주기를 통제한다.
- **Knights 적용 방안**: Knights의 `JobQueue`는 워커 스레드용이므로, 메인 io_context 안에서 돌려야 하는 경량 타이머(예: 정기 health ping, 메트릭 샘플링)에 `TaskMgr` 구조를 적용하면 구현이 단순해진다. 필요시 `JobQueue`로 전달하는 어댑터 작업을 추가하면 코어 루프와 백엔드 작업을 명확히 구분할 수 있다.

## 7. 스레드 안전 큐 헬퍼
- **핵심 개념**: `LockedQueue`가 단순한 뮤텍스 보호 큐와 `push_swap`/`push_reset` 헬퍼를 제공해 shared_ptr 참조를 제어한다. (`Sapphire/src/common/Util/LockedQueue.h:83`)
- **왜 좋은가**: push 이후 입력 객체를 기본 상태로 되돌려 메모리 누수를 막고, 큐가 비었을 때 기본값을 반환하도록 설계돼 소비측에서 예외 처리를 줄인다.
- **Knights 적용 방안**: Knights의 네트워크/스토리지 경계에서 생산자-소비자 큐가 필요할 때(예: Redis 스트림 변환 버퍼)에 바로 도입 가능하다. 특히 `push_reset` 패턴은 protobuf `shared_ptr` 재사용에 적합하다.

## 8. 안전한 Crash 핸들링과 최소 로깅
- **핵심 개념**: `Util::CrashHandler`가 플랫폼별로 신호를 잡아 스택 트레이스를 덤프하고, signal-safe `safe_logf`로 stderr/OutputDebugString에 직접 기록한다. (`Sapphire/src/common/Util/CrashHandler.cpp:33`, `Sapphire/src/common/Util/CrashHandler.cpp:103`)
- **왜 좋은가**: 서드파티 로거(예: spdlog)를 signal 핸들러에서 호출하지 않으면서도 크래시 원인을 텍스트로 남길 수 있다. Windows에서는 `DbgHelp`를 통해 심볼 정보까지 추출한다. (`Sapphire/src/common/Util/CrashHandler.cpp:171`)
- **Knights 적용 방안**: Knights도 코어 바이너리마다 `CrashHandler`를 전역으로 배치하면 CI/현장 환경에서 예외 스택을 확보할 수 있다. 특히 Windows 지원이 필요한 경우 별도 구현 없이 바로 활용 가능하다.

## 9. 버퍼 캡처 가능한 Logger 추상화
- **핵심 개념**: `Logger`가 spdlog의 async logger를 감싸고, `buffer_sink`를 통해 최근 로그를 메모리에 보관한다. 동시 초기화 레이스를 `std::call_once`와 내부 `ensure_initialized`로 막는다. (`Sapphire/src/common/Logging/Logger.cpp:54`, `Sapphire/src/common/Logging/Logger.h:20`)
- **왜 좋은가**: UI/콘솔이 필요로 하는 최근 로그를 별도 저장소 없이 가져올 수 있고, 로그 시스템이 초기화되기 전에 호출돼도 안전하게 stdout sink를 생성한다. 또한 로그 레벨을 동적으로 바꿔도 중앙에서 처리된다. (`Sapphire/src/common/Logging/Logger.cpp:102`)
- **Knights 적용 방안**: Knights의 코어 로그 래퍼를 이 패턴으로 교체하면 실시간 진단 UI나 gRPC/HTTP `/logs` 엔드포인트에서 최근 로그 버퍼를 쉽게 노출할 수 있다. 비동기 로거 초기화와 레이스 방지 코드는 그대로 가져다 쓸 수 있다.

## 10. 실행 파일 경로 유틸리티
- **핵심 개념**: `Util::executablePath`/`executableDir`가 플랫폼별 API를 통해 실행 파일 위치를 계산한 뒤 `weakly_canonical`로 정규화한다. (`Sapphire/src/common/Util/Paths.cpp:5`)
- **왜 좋은가**: config/data 디렉터리를 실행 파일 상대 경로로 탐색할 수 있어 설치 경로가 달라도 안정적으로 자원을 찾는다.
- **Knights 적용 방안**: Knights의 CLI/서비스가 리포지터리 상대 경로를 추론할 때 `.env`가 없어도 동작하도록, 이 유틸을 도입해 `config/`, `docs/` 기본 경로를 설정할 수 있다.

---

### 요약된 적용 전략
1. **공통 인프라 등록 계층**을 도입해 멀티 바이너리 환경에서도 동일 자원을 공유한다.
2. **설정/부트스트랩 패턴**을 통일해 신규 도구나 테스트 러너가 기본값을 자동 복제하도록 한다.
3. **비동기 네트워크·프레임 파이프라인**을 재사용해 새로운 프로토콜 구현 시 기본기 시간을 절약한다.
4. **DB 작업과 스케줄링**을 큐 기반으로 분리해 코어 루프의 응답성을 유지한다.
5. **경량 락 큐** 도입으로 shared_ptr 생애주기 문제를 예방하면서 생산자-소비자 구조를 단순화한다.
6. **Hive 스타일 네트워크 계층**은 장기적으로 게이트웨이/다중 인스턴스 구성이 필요해질 때를 목표로 기반을 다져 두고, 현재 구조에서는 일반화 범위를 점진적으로 확대한다.
