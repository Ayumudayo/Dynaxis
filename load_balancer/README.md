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

## 동작 흐름
1. Gateway 가 `Stream` RPC 를 열고 HELLO 메시지를 보냅니다.
2. LB는 sticky 캐시/Consistent Hash를 참고해 backend 를 선택합니다.
3. Backend TCP 소켓을 연 뒤 Gateway ↔ Backend 사이를 프락시합니다.
4. 데이터가 일정 시간 이상 흐르지 않으면 `LB_BACKEND_IDLE_TIMEOUT` 만큼 기다렸다가 세션을 정리하고 `metric=lb_backend_idle_close_total` 로그를 남깁니다.

```
[Gateway] ==gRPC==> [LB Stream] --TCP--> [Backend(server_app)]
                  ↘ Redis SessionDirectory (gateway/session/<client>)
```

## 특징
- **Dynamic Backend Discovery**: `LB_DYNAMIC_BACKENDS=1` 이면 Instance Registry(`gateway/instances/*`) 를 주기적으로 읽어 링을 갱신합니다.
- **Consistent Hash + Failover**: 실패 횟수가 `LB_BACKEND_FAILURE_THRESHOLD` 를 넘으면 `LB_BACKEND_COOLDOWN` 동안 링에서 제외합니다.
- **Sticky Session**: Redis 에 client_id → backend_id 를 TTL 기반으로 저장하고, 로컬 캐시에도 만료 시각을 기록합니다.
- **Idle 감시**: `LB_BACKEND_IDLE_TIMEOUT` 초 이상 활동이 없으면 backend 소켓을 강제 종료합니다.

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
| `LB_BACKEND_REFRESH_INTERVAL` | Registry 스냅샷 주기 | `5` |

## Metrics / 로그
- `metric=lb_backend_idle_close_total value=<n>` : idle 강제 종료 횟수(로그 기반). Prometheus에서 `sum(increase(lb_backend_idle_close_total[5m]))` 을 감시하세요.
- 기타 지표는 `/metrics` HTTP 엔드포인트(향후 TODO)에서 노출 예정입니다.

## 빌드 & 실행
```powershell
cmake --build build-msvc --target load_balancer_app
.uild-msvc\load_balancer\Debug\load_balancer_app.exe
```
Gateway 와 server_app 이 정상 동작 중이어야 하며, Redis/Registry 설정은 `docs/ops/gateway-and-lb.md` 를 참고하세요.
