# load_balancer_app

`load_balancer/` 는 gateway_app 과 gRPC 스트림을 주고받으면서 backend TCP 세션을 대신 유지하는 컴포넌트입니다. Consistent Hash, Redis 기반 sticky session, backend health 관리, idle close 등을 제공합니다.

## 구성
```
load_balancer/
├─ include/load_balancer/
│  ├─ load_balancer_app.hpp
│  └─ session_directory.hpp
└─ src/
   ├─ load_balancer_app.cpp
   ├─ session_directory.cpp
   └─ main.cpp
```

## 특징
- **Consistent Hash**: `LB_BACKEND_ENDPOINTS` + Instance Registry(`gateway/instances/*`)를 조합해 링을 구성합니다.
- **Sticky Session**: Redis `gateway/session/<client>` 키를 TTL 기반으로 관리하며, 로컬 캐시에도 만료 시각을 저장합니다.
- **Idle 감시**: `LB_BACKEND_IDLE_TIMEOUT` 초 이상 데이터가 흐르지 않으면 TCP를 강제로 끊고 `lb_backend_idle_close_total` 로그를 남깁니다.
- **동적 백엔드**: `LB_DYNAMIC_BACKENDS=1`이면 registry 스냅샷을 주기적으로 갱신합니다.

## 환경 변수 요약
| 이름 | 설명 | 기본값 |
| --- | --- | --- |
| `LB_GRPC_LISTEN` | gRPC listen 주소 | `127.0.0.1:7001` |
| `LB_BACKEND_ENDPOINTS` | `host:port` 목록 (정적) | `127.0.0.1:5000` |
| `LB_REDIS_URI` | Redis URI (없으면 sticky 비활성) | 빈 값 |
| `LB_SESSION_TTL` | sticky TTL(초) | `45` |
| `LB_BACKEND_FAILURE_THRESHOLD` | 실패 허용 횟수 | `3` |
| `LB_BACKEND_COOLDOWN` | 실패 후 재시도 대기(초) | `5` |
| `LB_BACKEND_IDLE_TIMEOUT` | backend 유휴 종료(초) | `30` |

## 빌드 & 실행
```powershell
cmake --build build-msvc --target load_balancer_app
.uild-msvc\load_balancer\Debug\load_balancer_app.exe
```
Gateway 와 server_app 이 정상 동작 중이어야 하며, Redis/Registry 설정은 `docs/ops/gateway-and-lb.md` 를 참고하세요.
