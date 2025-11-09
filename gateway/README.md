# gateway_app

`gateway/` 는 TCP 클라이언트 연결을 수용하고 Load Balancer와 gRPC 스트림을 맺는 프런트 엔드입니다. 인증, 세션 생성, heartbeat, Pub/Sub self-echo 방지 등을 담당합니다.

## 구성
```
gateway/
├─ include/gateway/
│  ├─ auth/              # IAuthenticator, NoopAuthenticator
│  ├─ gateway_app.hpp    # 앱 수명주기/환경 변수 로딩
│  └─ gateway_connection.hpp # TCP 세션 처리
└─ src/
   ├─ gateway_app.cpp
   ├─ gateway_connection.cpp
   └─ main.cpp
```

## 특징
- Boost.Asio Hive 위에서 다수의 TCP 세션을 처리하며, 각 세션은 Load Balancer `Stream` RPC 와 1:1 매칭됩니다.
- 인증은 `auth::IAuthenticator` 를 구현해 확장할 수 있습니다 (`ALLOW_ANONYMOUS`, `AUTH_PROVIDER`, `AUTH_ENDPOINT`).
- Redis Presence 및 Pub/Sub self-echo를 막기 위해 `GATEWAY_ID` 를 로그에 남기고 heartbeat 시 Presence TTL을 갱신합니다.
- gRPC 스트림이 끊기면 `LB_RETRY_DELAY_MS` 간격으로 재연결을 시도하며, 3회 이상 실패 시 클라이언트에 `SESSION_MOVED` 알림을 전송하도록 설계되어 있습니다.

### Auth 확장 예시
```cpp
class TokenAuthenticator : public gateway::auth::IAuthenticator {
 public:
  gateway::auth::AuthResult authenticate(const AuthRequest& req) override {
    if (req.token == "secret") {
      return {true, req.client_id.empty() ? "token-user" : req.client_id, {}};
    }
    return {false, {}, "invalid token"};
  }
};
```
`gateway_app` 초기화 시 `set_authenticator(std::make_shared<TokenAuthenticator>())` 로 주입합니다.

## 환경 변수
| 이름 | 설명 | 기본값 |
| --- | --- | --- |
| `GATEWAY_LISTEN` | TCP 수신 주소:포트 | `0.0.0.0:6000` |
| `GATEWAY_ID` | 인스턴스 식별자 (Presence, 로그) | `gateway-default` |
| `LB_GRPC_ENDPOINT` | Load Balancer gRPC 주소 | `127.0.0.1:7001` |
| `LB_RETRY_DELAY_MS` | LB 재연결 대기시간 | `3000` |

자세한 옵션은 `docs/configuration.md` 와 `docs/ops/gateway-and-lb.md` 를 참고하세요.

## 빌드 & 실행
```powershell
cmake --build build-msvc --target gateway_app
.build-msvc\gateway\Debug\gateway_app.exe
```
Load Balancer가 먼저 올라 있어야 하며, `.env.gateway` 에 gRPC 엔드포인트를 정확히 설정해야 합니다.


