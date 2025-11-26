# 명명/네임스페이스 가이드

이 문서는 Knights 리포지터리 전체에 적용되는 네이밍 규칙과 예외를 정리한다. 목적은 ① 프로젝트명을 코드/바이너리 식별자로 사용하지 않고, ② 역할 기반 이름을 통해 모듈을 재사용 가능하게 유지하며, ③ 문서/코드/운영 자산 간 일관성을 보장하는 것이다.

## 1. 공통 원칙
- **역할 기반 이름 사용**: `gateway`, `load_balancer`, `chat`, `auth`, `presence` 등 기능을 설명하는 이름만 허용한다. `Knights`, `knights`, `kproj` 등 프로젝트명을 코드 내 식별자나 네임스페이스로 사용하는 것을 금지한다.
- **소문자 snake_case 파일명**: 헤더/소스/스크립트/문서는 모두 소문자 + `_` 조합을 사용한다. 예: `chat_service_core.cpp`, `gateway_app.hpp`.
- **UTF-8 문서, ASCII 심볼**: 문서는 한국어+UTF-8, 코드 심볼은 ASCII 이름만 사용한다.
- **네임스페이스 = 패키지 경로**: `server::core::net`, `server::gateway::auth`처럼 디렉터리 구조와 동일한 계층을 유지한다.

## 2. C++ 코드/파일 규칙
| 항목 | 규칙 | 예시 |
| --- | --- | --- |
| 파일명 | snake_case + 확장자(`.hpp`, `.cpp`) | `chat_router.hpp`, `chat_router.cpp` |
| 클래스/struct | PascalCase | `ChatService`, `GatewayConnection` |
| 함수/변수 | snake_case | `load_recent_messages`, `queued_bytes_` |
| constexpr/enums | SCREAMING_SNAKE_CASE | `DEFAULT_RECENT_LIMIT`, `enum class route_kind` |
| 멤버 접미사 | 포인터/스마트포인터는 `_`, `_ptr` 등을 사용하지 않고 의미 기반 이름 사용 | `std::shared_ptr<Session> session` |
| 콜백 | 비동기 콜백은 `do_read`, `do_write` 처럼 동작을 명시 | `do_accept()` |

헤더 상단은 `#pragma once` 로 통일하고, include 순서는 표준 라이브러리 → 서드파티 → 프로젝트 헤더 순으로 유지한다.

## 3. CMake 타깃/바이너리 네이밍
- **라이브러리**: `server_core`, `gateway_common`, `storage_pg`.
- **실행 파일**: `<role>_app` 또는 `<tool>`. 예: `server_app`, `gateway_app`, `load_balancer_app`, `wb_worker`, `dev_chat_cli`.
- **테스트**: `<module>_tests`, `<scenario>_tests`. 예: `state_instance_registry_tests`, `core_concurrency_tests`.
- **설치 산출물**: 동일 이름을 유지하며, `install(TARGETS server_core EXPORT server_coreTargets ...)` 형식으로 패키징한다.
- CMake 네임스페이스/패키지 export 이름도 `server_core::` 처럼 역할 기반으로 통일한다.

## 4. 디렉터리/모듈
```
core/include/server/core/...   # public headers
core/src/...                  # implementation
server/...                    # server_app (추후 services/gateway 로 이동 예정)
gateway/...                   # gateway_app
load_balancer/...             # load_balancer_app
devclient/...                 # dev_chat_cli
docs/...                      # 문서
tools/...                     # 코드 생성/유틸
scripts/...                   # 빌드/운영 스크립트
```
신규 모듈을 만들 때는 `include/<package>/` + `src/` 페어를 유지하고, 헤더 경로가 네임스페이스 구조와 일치하도록 한다.

## 5. 프로토콜/메시지/CLI
- **Protobuf/gRPC**: 패키지 `server.wire.v1`, 서비스명 `Gateway`, `Chat`, RPC `SendMessage`, `JoinRoom` 등 동사+명사 조합 사용.
- **바이너리 프로토콜 opcode**: `MSG_HELLO`, `MSG_CHAT_SEND`, `MSG_STATE_SNAPSHOT`. 접두 `MSG_`/`ERR_`/`ROUTE_KIND_`를 유지한다.
- **CLI 명령**: `/login`, `/join`, `/whisper`, `/leave`, `/rooms`, `/who`, `/refresh`. `<필수>`, `[옵션]`, `...` 표기로 도움말을 작성한다.
- **환경 변수**: 전부 대문자 + `_`, 역할을 명시(`SERVER_BIND_ADDR`, `LB_BACKEND_IDLE_TIMEOUT`, `RECENT_HISTORY_LIMIT`). 공통 접두(`LB_`, `WB_`, `GATEWAY_`)로 범위를 구분한다.

## 6. 데이터 계층
| 영역 | 규칙 | 예시 |
| --- | --- | --- |
| Postgres 테이블 | 복수형 snake_case | `users`, `rooms`, `memberships`, `session_events` |
| PK | `id` (UUID/bigserial) | `rooms.id`, `messages.id` |
| FK | `<entity>_id` | `messages.room_id`, `memberships.user_id` |
| 인덱스 | `idx_<table>_<columns>` | `idx_users_lower_name`, `idx_messages_room_created_at` |
| enum/도메인 | snake_case, 접두 `enum_`, `domain_` (필요 시) |

마이그레이션 파일은 `tools/migrations/000N_<summary>.sql` 형식을 사용한다.

## 7. Redis/캐시 키
- 구조: `<scope>:<id>:<suffix>` 또는 `<scope>:<id>` (소문자).
- 예시
  - `room:{room_id}:recent`
  - `msg:{message_id}`
  - `presence:user:{user_id}`
  - `gateway:instances:{server_id}`
- TTL이나 maxlen 설정이 필요한 키는 docs/db/redis-strategy.md 에 표로 정리한다.

## 8. 관측/로그/메트릭
- **메트릭 이름**: Prometheus 관례(`snake_case + _total/_seconds/_ms`). 예:
  - `lb_backend_idle_close_total`
  - `chat_recent_cache_hit_total`
  - `wb_commit_latency_ms`
- **로그 태그**: `[gateway]`, `[server]` 등 역할 기반 prefix 사용. 민감 정보는 절대 포함하지 않는다.
- **알람/대시보드**: Grafana 패널명은 기능 중심("Gateway gRPC Reconnect", "LB Idle Close Rate")으로 작성한다.

## 9. 문서/산출물
- 문서 파일명은 소문자 snake_case (`docs/ops/distributed_routing_draft.md`, `docs/chat/recent-history.md`).
- README는 `<project>/README.md` 형식을 유지하고, 최상위 제목은 역할 중심(`server_core`, `gateway_app`)으로 맞춘다.

## 10. 명명 체크리스트
- [ ] 새 코드/문서에 `knights`, `Knights` 문자열이 등장하지 않는다.
- [ ] 파일/디렉터리는 snake_case, 네임스페이스는 디렉터리 구조와 일치한다.
- [ ] 환경 변수/메트릭/Redis 키는 접두사로 범위가 드러난다.
- [ ] CLI/프로토콜 명령 표기는 `/command`, `MSG_*`/`ERR_*` 규칙을 따른다.
- [ ] 문서/README는 역할 중심 제목을 사용한다.

모듈을 추가하거나 리팩터링할 때는 위 표를 기준으로 self-review 하여 규칙 위반을 조기에 수정한다.
