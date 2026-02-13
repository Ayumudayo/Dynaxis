# Server App

`server_app`은 Knights 프로젝트의 메인 채팅 서버 애플리케이션입니다.
`core` 라이브러리를 기반으로 구축되었으며, 클라이언트 연결 관리, 채팅 로직 처리, 데이터 저장(DB/Redis), 그리고 인스턴스 레지스트리 등록 등의 역할을 수행합니다.

## 아키텍처 (Architecture)

서버는 크게 **Bootstrap**, **Router**, **Service** 계층으로 구성됩니다.

1.  **Bootstrap (`src/app/bootstrap.cpp`)**:
    - 서버 초기화의 진입점입니다.
    - 환경 변수 파싱.
    - `io_context`, `JobQueue`, `ThreadManager` 등 코어 컴포넌트 초기화.
    - DB/Redis 연결 풀 생성.
    - `Listener`를 시작하여 클라이언트 연결 수락.

2.  **Router (`src/app/router.cpp`)**:
    - `Dispatcher`에 Opcode별 핸들러를 등록합니다.
    - 예: `MSG_LOGIN` -> `ChatService::on_login`

3.  **ChatService (`src/chat/`)**:
    - 핵심 비즈니스 로직을 담당합니다.
    - **State Management**: 현재 접속한 유저, 활성화된 방, 인증 상태 등을 메모리(`State` 구조체)에서 관리합니다.
    - **Storage Interaction**:
        - **Redis**: 실시간 메시지 캐싱, Pub/Sub(분산 채팅), Presence(접속 현황), Write-behind(이벤트 스트림).
        - **PostgreSQL**: 영구적인 데이터 저장(유저 정보, 채팅 기록 등). Write-behind 워커에 의해 비동기적으로 저장될 수도 있습니다.

## 주요 기능 (Features)

- **Opcode 라우팅**: 패킷의 Opcode에 따라 적절한 핸들러로 분기합니다.
- **Snapshot + Redis Cache**: 방 입장 시 최근 메시지를 Redis(LIST/LRU)에서 우선 조회하여 빠르게 로딩하고, 부족하면 DB에서 가져옵니다.
- **Write-behind**: `WRITE_BEHIND_ENABLED=1` 설정 시, 중요한 이벤트(채팅 등)를 Redis Streams에 먼저 기록하고, 별도의 워커(`wb_worker`)가 이를 DB에 반영하여 쓰기 성능을 최적화합니다.
- **Instance Registry**: 서버 시작 시 자신의 주소(Host/Port)를 Redis에 등록하고 주기적으로 Heartbeat를 갱신합니다. 로드 밸런서(`gateway`)는 이를 통해 활성 서버 목록을 파악합니다.
- **Metrics**: `/metrics` 엔드포인트(HTTP)를 통해 Prometheus 호환 지표를 노출합니다.

## 빌드 및 실행 (Build & Run)

### 빌드
프로젝트 루트에서 CMake Presets를 사용하여 빌드합니다.

```powershell
# Debug 빌드
cmake --build --preset windows-vs2022-debug --target server_app

# Release 빌드
cmake --build --preset windows-vs2022-relwithdebinfo --target server_app
```

### 실행
실행 파일은 `build-windows/server/Debug/server_app.exe` 경로에 생성됩니다.
실행 시 포트 번호를 인자로 전달할 수 있습니다.

```powershell
.\build-windows\server\Debug\server_app.exe 5000
```

## 환경 변수 설정 (Configuration)

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
| `SERVER_REGISTRY_PREFIX` | Instance Registry key prefix | `gateway/instances/` |
| `SERVER_REGISTRY_TTL` | Instance Registry TTL seconds | `30` |
| `METRICS_PORT` | 메트릭 수집을 위한 HTTP 포트 | `9090` |
| `LOG_BUFFER_CAPACITY` | 메모리 내 로그 버퍼 크기 | `256` |

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
