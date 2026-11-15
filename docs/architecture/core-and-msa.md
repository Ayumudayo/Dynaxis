# 필수 코어와 MSA 채팅 서버의 경계

프로젝트의 목표는 재사용 가능한 C++ 서버 코어와, 그 위에 조립한 MSA 채팅 서버다. 채팅은 첫 번째 실제 앱으로 유지한다. 다른 용도의 앱에서도 연결·실행·종료와 서비스 기반을 재사용할 수 있도록 업무 규칙의 소유권을 구분한다.

| 계층 | 책임 | 현재 구현 |
|---|---|---|
| 필수 코어 | TCP I/O, 연결/버퍼 수명, framing, handler routing, 작업 큐/스케줄러, 종료, 기본 관측성 | `core`, 최소 `server_core` 구성 |
| MSA 서비스 기반 | gateway, discovery/라우팅, 인증 연동 경계, 서비스 간 전달, Redis/PG adapter, write-behind, readiness/운영 | `gateway`, `infra`, `server` 조립 코드, `tools/wb_*` |
| 채팅 앱 | 사용자/룸, 메시지/귓속말, presence/history, 제재와 설정 규칙 | `server/chat`, `server/config/runtime_settings.hpp` |

의존 방향은 채팅 앱 → MSA 서비스 기반/코어, MSA 서비스 기반 → 코어다. 공통으로 사용하는 채팅 설정도 채팅 도메인이 소유한다. admin은 해당 계약을 소비하며, 코어가 채팅의 스팸/제재 규칙을 소유하지 않는다.

## 두 빌드 구성

- 기본 전체 구성: `BUILD_CORE_EXTENSIONS=ON`. 기존 `server_core` 전체 공개 API와 MSA 앱/도구를 유지한다. Redis, Postgres, gateway, write-behind를 함께 검증한다.
- 최소 구성: `BUILD_CORE_EXTENSIONS=OFF`. 앱/도구/GUI를 끈 core-engine 프리셋으로 빌드한다. Boost 외에 LuaJIT, sol2, Protobuf, OpenSSL, LZ4, Redis, Postgres를 요구하지 않는다. GTest 단위 검증은 선택 사항이고 TCP/설치 smoke는 GTest 없이 실행할 수 있다.

최소 설치는 구현이 제공되는 runtime 헤더만 설치하며 CMake package는 `server_core_HAS_EXTENSIONS=OFF`를 노출한다. 전체 API를 쓰는 소비자는 이 값을 확인하거나 기본 전체 package를 사용해야 한다. 최소/전체 package는 같은 prefix에 겹쳐 설치하지 않는다.

`core-only`는 제품을 줄이는 목적이 아니라 코어 독립성을 검증하는 구성이다. 실제 제품의 완료 기준에는 gateway를 거친 채팅, 노드 간 전달, 영속화와 의존성 복구가 포함된다.

## 선택 기능과 호환성

FPS는 `FPS_ENABLED=1`일 때만 객체, opcode route와 tick loop를 구성한다. 기본값 `0`인 MSA 채팅 프로세스에서는 실행하지 않는다. 기존 RUDP/FPS 검증용 env 예시는 이를 명시적으로 켠다.

기존 worlds, provider, Lua/plugin, RUDP API는 전체 구성에서 보존한다. 이 기능을 삭제하거나 새로운 Stable API를 늘리는 작업보다, 현재 MSA 채팅의 실행/장애 경계를 검증하는 것을 우선한다. legacy Dispatcher/Session과 전역 ServiceRegistry 소비자의 통합, 관리 앱의 책임 분리는 이후 migration 대상이다. 현재 이들이 모두 제거되었다고 보지 않는다.

## 실패 경계

- `TransportRouter(queue)`와 `Dispatcher(queue)`는 주입한 queue에서만 worker 요청을 실행합니다. 명시적 null/full/stopped queue는 `SERVER_BUSY`로 거절하며 다른 runtime의 전역 queue를 사용하지 않습니다. 기본 생성자는 기존 Registry 호환 경로이므로 새 앱은 명시적 생성자를 사용합니다.
- `Connection` 송신 예산은 executor에 대기하는 payload와 쓰기 큐, 진행 중 write를 포함한다. 넘친 요청은 적재하지 않고 오류를 통지한 뒤 연결을 닫는다.
- `TaskScheduler::poll()`은 작업/validator 예외가 나면 해당 작업만 취소하고 batch의 나머지를 계속 실행한다. 성공한 callback만 완료 수로 계산한다.
- `MemoryPool`은 중복 반환을 무시하고 constructor 크기 overflow를 거절한다. 반환한 포인터를 다시 사용하는 것은 금지한다.
- soak는 기본적으로 요청 실패를 허용하지 않는다. 필요한 실험만 `SOAK_MAX_ERROR_RATE`에 0..1 범위의 허용 비율을 명시한다.

`Connection`, `MemoryPool`, `BufferManager`, `TransportRouter`, `Dispatcher`의 private layout이 변경됐으므로 3.6.1 소비자는 다시 빌드한다. wire framing과 기본 MSA 채팅 메시지는 변경하지 않는다.

실제 server bootstrap과 관측 HTTP는 명시적 instance 의존성을 사용한다. 인증·재접속·readiness·종료 계약은 [인증된 MSA 채팅 실행](../ops/authenticated-chat.md)을 따른다. 관리 콘솔 확장은 핵심 기능 검증 뒤로 미룬다.
