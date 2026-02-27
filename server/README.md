# 서버 애플리케이션

`server_app`은 Knights 프로젝트의 메인 채팅 서버 애플리케이션입니다.
`core` 라이브러리를 기반으로 구축되었으며, 클라이언트 연결 관리, 채팅 로직 처리, 데이터 저장(DB/Redis), 그리고 인스턴스 레지스트리 등록 등의 역할을 수행합니다.

## 아키텍처

서버는 크게 **Bootstrap**, **Router**, **Service** 계층으로 구성됩니다.

1.  **Bootstrap (`src/app/bootstrap.cpp`)**:
    - 서버 초기화의 진입점입니다.
    - 환경 변수 파싱.
    - `io_context`, `JobQueue`, `ThreadManager` 등 코어 컴포넌트 초기화.
    - DB/Redis 연결 풀 생성.
    - `core::net::SessionListener`를 시작하여 클라이언트 연결을 수락하고 `core::net::Session`으로 전달.

2.  **Router (`src/app/router.cpp`)**:
    - `Dispatcher`에 Opcode별 핸들러를 등록합니다.
    - 예: `MSG_LOGIN` -> `ChatService::on_login`

3.  **ChatService (`src/chat/`)**:
    - 핵심 비즈니스 로직을 담당합니다.
    - **State Management**: 현재 접속한 유저, 활성화된 방, 인증 상태 등을 메모리(`State` 구조체)에서 관리합니다.
    - **Storage Interaction**:
        - **Redis**: 실시간 메시지 캐싱, Pub/Sub(분산 채팅), Presence(접속 현황), Write-behind(이벤트 스트림).
        - **PostgreSQL**: 영구적인 데이터 저장(유저 정보, 채팅 기록 등). Write-behind 워커에 의해 비동기적으로 저장될 수도 있습니다.

## 주요 기능

- **Opcode 라우팅**: 패킷의 Opcode에 따라 적절한 핸들러로 분기합니다.
- **Snapshot + Redis Cache**: 방 입장 시 최근 메시지를 Redis(LIST/LRU)에서 우선 조회하여 빠르게 로딩하고, 부족하면 DB에서 가져옵니다.
- **Refresh Fanout Dedup**: join/leave 처리에서 `lobby` 대상 중복 `broadcast_refresh`를 제거해 불필요한 fanout 신호를 줄입니다.
- **RoomUsers Lock Scope 개선**: `send_room_users`는 lock 구간에서 권한/멤버십만 확인하고, Redis 조회(`SMEMBERS`)는 lock 밖에서 수행해 경쟁 구간을 줄입니다.
- **Chat Hot-path 로그 노이즈 절감**: `CHAT_SEND` 본문 로그를 제거하고, pub/sub publish 카운트 로그는 샘플링(1024건마다) + `debug` 레벨로 축소합니다. whisper 상태 로그도 `debug`로 내려 운영 로그 볼륨을 낮춥니다.
- **Write-behind**: `WRITE_BEHIND_ENABLED=1` 설정 시, 중요한 이벤트(채팅 등)를 Redis Streams에 먼저 기록하고, 별도의 워커(`wb_worker`)가 이를 DB에 반영하여 쓰기 성능을 최적화합니다.
- **Instance Registry**: 서버 시작 시 자신의 주소(Host/Port)를 Redis에 등록하고 주기적으로 Heartbeat를 갱신합니다. 로드 밸런서(`gateway`)는 이를 통해 활성 서버 목록을 파악합니다.
- **Metrics**: `/metrics` 엔드포인트(HTTP)를 통해 Prometheus 호환 지표를 노출합니다.

## 빌드 및 실행

### 빌드
프로젝트 루트에서 빌드 스크립트를 사용하여 빌드합니다. (내부적으로 CMake Presets 사용)

```powershell
# 디버그(Debug) 빌드
pwsh scripts/build.ps1 -Config Debug -Target server_app

# 릴리스(Release) 빌드 (RelWithDebInfo)
pwsh scripts/build.ps1 -Config RelWithDebInfo -Target server_app
```

### 실행 (권장: Linux/Docker)
서버 스택 런타임은 Linux(예: Docker)로 통일하는 것을 권장한다.

```powershell
scripts/deploy_docker.ps1 -Action up -Detached -Build
```

### (옵션) Windows 단일 프로세스 실행
Windows에서 빌드된 실행 파일은 `build-windows/server/Debug/server_app.exe` 경로에 생성된다.

```powershell
.\build-windows\server\Debug\server_app.exe 5000
```

## 환경 변수 설정

서버는 OS 환경 변수를 통해 동작을 제어할 수 있습니다.
로컬에서는 `.env.example`를 복사해 `.env`를 만든 뒤, 실행 스크립트/쉘에서 로드해 사용할 수 있습니다.

| 변수명 | 설명 | 기본값/예시 |
| --- | --- | --- |
| `PORT` | 서버가 수신 대기할 포트 | `5000` |
| `DB_URI` | PostgreSQL 연결 문자열 (필수) | `postgresql://user:pass@localhost:5432/knights` |
| `REDIS_URI` | Redis 연결 문자열 (선택) | `tcp://127.0.0.1:6379` |
| `WRITE_BEHIND_ENABLED` | Write-behind 패턴 사용 여부 (`1`: 사용, `0`: 미사용) | `1` |
| `USE_REDIS_PUBSUB` | Redis Pub/Sub을 이용한 분산 채팅 활성화 여부 | `0` |
| `SERVER_ADVERTISE_HOST` | 레지스트리에 등록할 호스트 주소 (게이트웨이가 접근 가능한 주소) | `127.0.0.1` |
| `SERVER_ADVERTISE_PORT` | 레지스트리에 등록할 포트(옵션) | `5000` |
| `SERVER_REGISTRY_PREFIX` | Instance Registry 키 접두사 | `gateway/instances/` |
| `SERVER_REGISTRY_TTL` | Instance Registry TTL(초) | `30` |
| `METRICS_PORT` | 메트릭 수집을 위한 HTTP 포트 | `9090` |
| `CHAT_HOOK_PLUGINS_DIR` | (실험, 권장) 플러그인 디렉터리(모든 `.so/.dll`을 파일명 순으로 로드) | `/app/plugins` |
| `CHAT_HOOK_PLUGIN_PATHS` | (실험) 플러그인 경로 목록(순서 고정, 구분자 `;` 또는 `,`) | `/app/plugins/10_chat_hook_sample.so;/app/plugins/20_chat_hook_tag.so` |
| `CHAT_HOOK_PLUGIN_PATH` | (실험, 레거시) 단일 플러그인(.so/.dll) 경로 | `/app/plugins/10_chat_hook_sample.so` |
| `CHAT_HOOK_CACHE_DIR` | 플러그인 캐시 디렉터리(원본을 cache-copy 후 로드) | `/tmp/chat_hook_cache` |
| `CHAT_HOOK_LOCK_PATH` | (옵션) lock/sentinel 파일 경로(존재 시 reload 스킵, 단일 플러그인 모드에만 적용) | `<plugin_stem>_LOCK` |
| `CHAT_HOOK_RELOAD_INTERVAL_MS` | reload 폴링 주기(ms) | `500` |
| `LOG_BUFFER_CAPACITY` | 메모리 내 로그 버퍼 크기 | `256` |
| `CHAT_JOB_QUEUE_MAX` | 서버 로직 작업 큐 최대 길이(트래픽 스파이크 시 백프레셔/메모리 보호) | `8192` |
| `CHAT_DB_JOB_QUEUE_MAX` | DB 작업 큐 최대 길이(DB 지연 시 백프레셔/메모리 보호) | `4096` |
| `KNIGHTS_TRACING_ENABLED` | 경량 tracing context + span 로그 활성화 (`1`/`0`) | `0` |
| `KNIGHTS_TRACING_SAMPLE_PERCENT` | tracing 샘플링 비율(0~100) | `100` |

## 채팅 훅(Chat Hook) 플러그인 (실험)

`server_app`은 `MSG_CHAT_SEND` 경로에 hot-reload 가능한 플러그인 훅을 붙일 수 있습니다.

- ABI: `server/include/server/chat/chat_hook_plugin_abi.hpp` (C ABI v1, entrypoint `chat_hook_api_v1()`)
- 멀티 플러그인: 파일명 순서(예: `10_*.so`, `20_*.so`)로 순차 적용; `kReplaceText`는 다음 플러그인에 반영됨
- Docker 샘플 플러그인:
  - `/app/plugins/10_chat_hook_sample.so`
  - `/app/plugins/20_chat_hook_tag.so`
  - `/app/plugins/staging/10_chat_hook_sample_v2.so` (swap 용)
- Docker 스택 기본 설정: `docker/stack/docker-compose.yml`에서 `CHAT_HOOK_PLUGINS_DIR=/app/plugins`

핫 리로드 예시:

```bash
# 잠금 파일(lock/sentinel, 선택)
docker exec knights-stack-server-1-1 touch /app/plugins/10_chat_hook_sample_LOCK

# 바이너리 교체(swap)
docker exec knights-stack-server-1-1 cp /app/plugins/staging/10_chat_hook_sample_v2.so /app/plugins/10_chat_hook_sample.so

# 잠금 해제(unlock)
docker exec knights-stack-server-1-1 rm -f /app/plugins/10_chat_hook_sample_LOCK
```

## 디렉터리 구조

```
server/
├─ include/server/
│  ├─ app/      # Bootstrap, Router
│  ├─ chat/     # ChatService 및 핸들러 선언
│  ├─ state/    # Instance Registry 관련
│  └─ storage/  # DB/Redis 어댑터
└─ src/
   ├─ app/      # main.cpp, bootstrap.cpp 구현
   ├─ chat/     # 비즈니스 로직 구현 (핸들러 포함)
   ├─ state/    # 레지스트리 백엔드 구현
   └─ storage/  # 저장소 구현체
```
