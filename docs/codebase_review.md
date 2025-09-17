# 코드베이스 검토 보고서

## 1. 문서 및 계획 점검
- `docs/roadmap.md`는 DB 안정화 → Presence → Write-behind → 분산 브로드캐스트 순으로 우선순위를 명확히 제시하며, 각 단계의 DoD와 리스크를 구체적으로 기록하고 있습니다. WIP로 표기된 Pub/Sub 브리지, Write-behind 단계는 현재 구현과의 차이를 지속적으로 갱신할 필요가 있습니다.
- `docs/server-architecture.md`, `docs/repo-structure.md`는 서비스 디렉터리 분리(`services/gateway/` 등)를 전제로 설명하지만, 실제 리포는 아직 `server/`, `devclient/` 구조입니다. 계획을 유지하려면 리팩터링 진행 현황을 문서에 갱신하거나, 현 구조에 맞춘 보완 지침을 추가하는 편이 좋습니다.
- `docs/protocol.md`는 패킷 헤더와 `MSG_HELLO` 레이아웃, 예약 msg_id를 최신 상태(v1.1)로 유지하고 있으며 코드 구현(`core/src/net/session.cpp`)과 일치합니다. capability 추가 시 즉시 문서에 이력이 반영되도록 현 프로세스를 유지하세요.
- `docs/build.md`, `docs/naming-conventions.md`는 vcpkg·CMake 설정과 명명 규칙을 명확히 안내합니다. `scripts/build.ps1`의 주요 플래그나 예시 명령을 추가하면 신입 기여자에게 더 실용적일 것입니다.

## 2. 코드 분석 주요 개선 사항

### 2.1 해결된 핵심 이슈
- Redis Pub/Sub 브로드캐스트 포맷이 pub/sub 양쪽에서 어긋나 있었습니다(`server/src/app/bootstrap.cpp:165`, `server/src/chat/handlers_chat.cpp:235`). `ChatService`가 `gw=<id>\n<payload>` envelope로 메시지를 발행하도록 변경해 self-echo 필터링이 의도대로 동작합니다. 다중 게이트웨이 환경에서는 `GATEWAY_ID`를 인스턴스별로 고유하게 설정해야 합니다.
- 세션 송신 큐 길이 관리가 부족해 `queued_bytes_`가 write 완료 후에도 감소하지 않았습니다. `core/src/net/session.cpp:198`에서 write 콜백이 성공할 때 `queued_bytes_`를 줄여 큐 길이가 실제 송신 상태와 일치합니다.
- `ThreadManager`의 `stopped_` 플래그가 비원자형이어서 레이스가 발생했습니다. `core/include/server/core/concurrent/thread_manager.hpp:22`에 `std::atomic<bool>`을 도입하고 `Stop()`에서 compare_exchange를 사용해 중복 호출과 레이스를 제거했습니다.
- `core/include/server/core/protocol/frame.hpp`에 `<cstring>`을 포함시켜 헤더 단독 사용 시 `std::memcpy` 선언이 누락되지 않도록 했습니다.

### 2.2 추가 권장 사항
- Redis Pub/Sub envelope 생성 로직을 헬퍼 함수로 추상화하면 동일 포맷을 재사용하고 후속 자체 테스트를 작성하기 수월합니다.
- 문서 계획(`docs/roadmap.md`)과 실제 구현 간 차이를 정기적으로 검토해 리팩터링 단계(서비스 디렉터리 분리 등)의 진척을 명시하세요.

## 3. Context7 기반 외부 API 사용 검증
| API | 사용 위치 | 정의 확인 | 비고 |
| --- | --- | --- | --- |
| `pqxx::connection`, `pqxx::work` | `server/src/storage/postgres/connection_pool.cpp:271-288` | Context7 `/jtv/libpqxx` Getting Started 스니펫과 동일한 커넥션/트랜잭션 패턴을 사용합니다. | 권장 패턴과 일치함을 확인했습니다. |
| `ftxui::ScreenInteractive::Fullscreen`, `ScreenInteractive::PostEvent` | `devclient/src/main.cpp:35-60` | Context7 `/arthursonzogni/ftxui` Screen 문서가 Fullscreen 생성과 `PostEvent(Event::Custom)` 패턴을 명시합니다. | 이벤트 루프 강제 갱신 방식이 문서 예시와 동일합니다. |
| Redis++ (`sw::redis`) Streams/Subscriber | `server/src/storage/redis/client.cpp:18-171` | Context7에 `redis-plus-plus` C++ 문서가 없어 버전 일치는 미확인입니다. | 예외 처리 및 XADD/XREADGROUP 사용 패턴은 일반적인 구현과 일치하지만, upstream 문서로 버전 호환성을 확인해야 합니다. |

## 4. 후속 조치 제안
1. Redis Streams/Write-behind, Pub/Sub 브리지에 대한 통합 테스트와 운영 모니터링 지표를 `docs/db/write-behind.md`와 연계해 구체화하세요.
2. 서비스 구조 리팩터링 진행 상황을 `docs/server-architecture.md`, `docs/repo-structure.md` 등에 주기적으로 반영하여 계획 대비 실제 구현 상태를 명확히 하세요.
3. Pub/Sub envelope 헬퍼와 관련 단위 테스트를 추가해 멀티 게이트웨이 환경에서의 회귀 위험을 줄이는 것이 좋습니다.
