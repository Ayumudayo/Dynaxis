# gateway_app

`gateway/` 모듈은 클라이언트 TCP 연결을 수용하고 gRPC 스트림을 통해 Load Balancer와 바인딩하는 `gateway_app` 실행 파일을 제공합니다. Gateway는 Hive 기반 Listener/Connection을 사용하여 세션을 관리하고, 각 클라이언트별로 Load Balancer gRPC 스트림을 유지하면서 서버 인스턴스로 패킷을 전달합니다.

## 디렉터리 구조
- `include/gateway/gateway_app.hpp` — Gateway 런타임과 gRPC 세션 관리자 정의
- `include/gateway/gateway_connection.hpp` — 개별 TCP 연결 처리 및 wire 프로토콜 디코더
- `include/gateway/auth/` — 인증 훅(현재는 스텁)
- `src/gateway_app.cpp` — 환경 로드, Hive 초기화, gRPC 세션 관리 구현
- `src/gateway_connection.cpp` — 클라이언트 I/O 처리, Load Balancer 스트림 연계
- `src/main.cpp` — 엔트리 포인트
```text
gateway/
├─ include/
│  └─ gateway/
│     ├─ auth/
│     ├─ gateway_app.hpp
│     └─ gateway_connection.hpp
├─ src/
│  ├─ gateway_app.cpp
│  ├─ gateway_connection.cpp
│  └─ main.cpp
```


## 주요 환경 변수
| 변수 | 설명 | 기본값 |
| --- | --- | --- |
| `GATEWAY_LISTEN` | Gateway가 수신할 주소:포트 | `0.0.0.0:6000` |
| `GATEWAY_ID` | Gateway 인스턴스 ID (Presence, 로깅 용도) | `gateway-default` |
| `LB_GRPC_ENDPOINT` | 연결할 Load Balancer gRPC 주소 | `127.0.0.1:7001` |

필요 시 `.env` 파일을 실행 파일 옆 또는 리포지터리 루트에 배치하면 자동으로 로드됩니다.

## 빌드 & 실행
```powershell
cmake --build build-msvc --target gateway_app
.\build-msvc\gateway\Debug\gateway_app.exe
```
Gateway를 실행하기 전에 Load Balancer(`load_balancer_app`)가 gRPC 포트를 열고 있어야 하며, `LB_GRPC_ENDPOINT`가 일치해야 합니다.

## 런타임 동작
- 새 TCP 세션이 생성되면 `GatewayConnection`이 wire 프로토콜을 파싱하고, 최초 HELLO 후 Load Balancer `Stream` RPC를 열어 경로를 협상합니다.
- 클라이언트 종료 시 `/leave` 및 graceful shutdown을 수행하여 INFO 레벨 로그만 남기도록 설계되어 있습니다.
- 향후 인증 로직은 `include/gateway/auth/` 하위 구현을 확장하여 삽입할 수 있습니다.

세션 라우팅/에러 플로우는 `docs/server-architecture.md`의 Gateway 섹션과 `proto/gateway_lb.proto` 정의를 참고하세요.
