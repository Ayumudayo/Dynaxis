# load_balancer_app

`load_balancer/` 모듈은 Gateway에서 들어오는 gRPC 스트림을 서버 인스턴스로 라우팅하는 `load_balancer_app` 실행 파일을 제공합니다. Gateway와의 통신은 `gateway_lb.proto` 기반 양방향 스트림으로 이루어지며, backend 서버에는 TCP로 연결하여 payload를 그대로 전달합니다. Redis 또는 임시 인메모리 상태 저장소를 사용해 헬스체크 및 인스턴스 메타데이터를 관리할 수 있도록 설계되었습니다.

## 디렉터리 구조
- `include/load_balancer/load_balancer_app.hpp` — gRPC 서비스, 백엔드 선택 로직, 상태 저장소 인터페이스 정의
- `src/load_balancer_app.cpp` — 환경 로드, gRPC 서버 부팅, heartbeat 발행, 라우팅 구현
- `src/main.cpp` — 엔트리 포인트
```text
load_balancer/
├─ include/
│  └─ load_balancer/
│     └─ load_balancer_app.hpp
├─ src/
│  ├─ load_balancer_app.cpp
│  └─ main.cpp
```


## 주요 환경 변수
| 변수 | 설명 | 기본값 |
| --- | --- | --- |
| `LB_GRPC_LISTEN` | gRPC 수신 주소:포트 | `127.0.0.1:7001` |
| `LB_BACKEND_ENDPOINTS` | 콤마 구분 backend TCP 주소 목록 | `127.0.0.1:5000` |
| `LB_REDIS_URI` | (옵션) Redis 기반 인스턴스 레지스트리 URI | 설정 시 Redis 사용 |
| `LB_INSTANCE_ID` | Load Balancer 인스턴스 ID | `lb-local` |
| `PRESENCE_TTL_SEC` | Redis backend TTL (server_app와 공유) | `30` |

`.env`는 실행 파일과 같은 디렉터리 또는 리포지터리 루트에서 자동으로 로드됩니다.

## 빌드 & 실행
```powershell
cmake --build build-msvc --target load_balancer_app
.\build-msvc\load_balancer\Debug\load_balancer_app.exe
```
Gateway와 동일한 gRPC 포트를 공유하므로, 실제 배포에서는 고유 포트를 지정하고 `LB_BACKEND_ENDPOINTS`에 여러 서버 인스턴스를 나열해 라운드로빈 라우팅을 활용합니다.

## 런타임 동작
- Gateway와의 `Stream` RPC마다 backend TCP 소켓을 열고, 클라이언트 ↔ 서버 방향 payload를 그대로 릴레이합니다.
- 주기적 heartbeat를 `server_state::InstanceRegistry`에 기록하여 Gateway가 사용할 상태를 유지합니다.
- 종료 시 gRPC 서버, Hive, heartbeat 타이머를 순서대로 정리하여 세션 누수를 방지합니다.

향후 상태 공유를 Redis로 확장하거나 Consul, etcd 등으로 교체할 경우 `create_backend()` 내부 구현을 교체하면 됩니다. 세부 설계는 `docs/server-architecture.md`의 Load Balancer 섹션과 `docs/core-design.md`의 엔진화 TODO를 참고하세요.
