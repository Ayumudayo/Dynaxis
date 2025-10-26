# Gateway & Load Balancer 아키텍처

프로젝트는 이제 별도 프로세스로 **Gateway ↔ LoadBalancer ↔ Server** 파이프라인을 운용한다. Gateway가 클라이언트 TCP 세션을 수용하고, gRPC 양방향 스트리밍으로 Load Balancer에 프레임을 전달하면 Load Balancer가 서버 백엔드(TCP)로 중계한다.

```
Client
  │  (TCP)
  ▼
gateway_app  ── gRPC(Stream) ──►  load_balancer_app  ── TCP ──►  server_app
  │                                   │
  └─ Redis(optional) ◄────── 상태/헬스 ┘
```

## 주요 동작 흐름
1. **클라이언트 → Gateway**  
   Gateway는 Boost.Asio 기반 `Hive` 위에서 TCP 세션을 생성하고, 최초 메시지를 익명 인증(토큰 미검증)으로 처리한다.
2. **Gateway → LoadBalancer**  
   세션마다 gRPC 스트림을 개설하고 `RouteMessage`(`CLIENT_HELLO`/`CLIENT_PAYLOAD`) 메시지를 전송한다. Gateway는 스트림에서 `SERVER_PAYLOAD`/`SERVER_CLOSE`/`SERVER_ERROR` 를 역으로 수신해 클라이언트에 write 한다.
3. **LoadBalancer → Server**  
   Load Balancer는 Redis 세션 디렉터리(`LB_SESSION_TTL`)와 Consistent Hash ring으로 고정 백엔드를 우선 배정하고, 매핑이 없을 때만 라운드로빈으로 백엔드를 선택한다. 장애가 반복되면 `LB_BACKEND_FAILURE_THRESHOLD`·`LB_BACKEND_COOLDOWN` 설정으로 해당 백엔드를 일정 시간 제외한다. 클라이언트에서 받은 바이트 시퀀스를 그대로 server_app에 중계하며, 서버에서 오는 응답도 gRPC 스트림을 통해 Gateway로 되돌린다.
4. **상태 보고**  
   Load Balancer는 Redis 기반 `gateway/instances/*` 키에 인스턴스 정보를 주기적으로 갱신(기본 5초)한다. Redis가 비활성화된 경우 인메모리 백엔드를 사용한다.

## 실행 방법 (로컬)
1. **server_app**  
   ```powershell
   # 1) Postgres/Redis가 필요하면 .env 구성 후 실행
   cmake --build build-msvc --target server_app
   .\build-msvc\server\Debug\server_app.exe
   ```
2. **load_balancer_app**  
   ```powershell
   $env:LB_BACKEND_ENDPOINTS="127.0.0.1:5000"
   $env:LB_GRPC_LISTEN="127.0.0.1:7001"
   $env:LB_INSTANCE_ID="lb-local"
   cmake --build build-msvc --target load_balancer_app
   .\build-msvc\load_balancer\Debug\load_balancer_app.exe
   ```
3. **gateway_app**  
   ```powershell
   $env:LB_GRPC_ENDPOINT="127.0.0.1:7001"
   $env:GATEWAY_LISTEN="0.0.0.0:6000"
   $env:GATEWAY_ID="gw-local"
   cmake --build build-msvc --target gateway_app
   .\build-msvc\gateway\Debug\gateway_app.exe
   ```
4. **클라이언트**  
   기존 devclient 또는 임시 TCP 클라이언트를 `6000`번 포트로 연결하면 서버까지 라우팅된다.

## 환경 변수 요약
### gateway_app
| 변수 | 설명 | 기본값 |
| --- | --- | --- |
| `GATEWAY_LISTEN` | 수신 주소와 포트 (`host:port`) | `0.0.0.0:6000` |
| `GATEWAY_ID` | Gateway 인스턴스 식별자 | `gateway-default` |
| `LB_GRPC_ENDPOINT` | Load Balancer gRPC 엔드포인트 | `127.0.0.1:7001` |

### load_balancer_app
| 변수 | 설명 | 기본값 |
| --- | --- | --- |
| `LB_GRPC_LISTEN` | gRPC 리스닝 주소 (`host:port`) | `127.0.0.1:7001` |
| `LB_BACKEND_ENDPOINTS` | 서버 백엔드 목록(콤마 구분) | `127.0.0.1:5000` |
| `LB_INSTANCE_ID` | 상태 레지스트리에 기록할 ID | `lb-<timestamp>` |
| `LB_REDIS_URI` / `REDIS_URI` | 상태 백엔드(Redis) 연결 문자열 | 비활성 시 메모리 |
| `LB_SESSION_TTL` | 세션→백엔드 매핑 TTL(초) | `45` |
| `LB_BACKEND_FAILURE_THRESHOLD` | 백엔드 연속 실패 허용 횟수 | `3` |
| `LB_BACKEND_COOLDOWN` | 실패 후 재시도 대기(초) | `5` |
| `LB_SESSION_TTL` | 세션→백엔드 매핑 TTL(초) | `45` |

### server_app (변경 없음)
| 변수 | 설명 |
| --- | --- |
| `SERVER_BIND_ADDR`, `SERVER_PORT` | 기존 TCP 리스너 설정 |
| `GATEWAY_ID` | Redis fan-out self-echo 방지 |
| `USE_REDIS_PUBSUB`, `WRITE_BEHIND_ENABLED` 등 | 기존 옵션 유지 |

## 운영 및 TODO
- **스케일링**: Load Balancer가 Consistent Hash ring + 라운드로빈 폴백으로 다중 백엔드를 분배한다. 향후 Redis 레지스트리를 참고해 실시간 용량/세션 수 기반 스케줄링을 적용한다.
- **헬스체크**: 현재 gRPC와 TCP 실패 시 스트림이 종료된다. HTTP 헬스 엔드포인트와 메트릭 노출이 필요하다.
- **인증**: Gateway는 여전히 `auth::NoopAuthenticator`를 사용한다. 인증 토큰 검증 및 세션 레지스트리 연동이 추후 과제로 남아 있다.
- **멀티 게이트웨이**: Redis presence/PubSub 설정(`USE_REDIS_PUBSUB=1`)으로 서버 간 브로드캐스트를 유지할 수 있다. gateway_app 다중 인스턴스 시 `GATEWAY_ID`는 반드시 고유하게 지정한다.

### 다중 인스턴스 점검
- `.env`에서 `LB_BACKEND_ENDPOINTS`에 두 개 이상의 server_app 포트를 명시하고, 각 server_app은 고유 `SERVER_PORT`/`METRICS_PORT`를 사용한다.
- `LB_SESSION_TTL`, `LB_BACKEND_FAILURE_THRESHOLD`, `LB_BACKEND_COOLDOWN`, `GATEWAY_ID`가 노드별로 일관되게 설정되어 있는지 확인한다.
- Redis Pub/Sub 브로드캐스트를 활용하려면 `USE_REDIS_PUBSUB=1`, `REDIS_CHANNEL_PREFIX`, `GATEWAY_ID`를 모든 인스턴스에서 동일하게 설정한다.
- Gateway를 통해 동일한 클라이언트 ID로 여러 번 접속해 `load_balancer.log`의 `backend=` 라우팅이 동일한지 확인한다.
- 서버 인스턴스 중 하나를 중지하면 TTL 만료 후 동일 클라이언트가 다른 인스턴스로 재배치되는지 검증한다.

