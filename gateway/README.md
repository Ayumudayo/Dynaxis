# gateway_app

`gateway/` 는 TCP 클라이언트 연결을 수용하는 프런트 엔드(Edge Gateway)입니다.

운영 환경에서는 보통 **외부 TCP(L4) 로드밸런서(예: HAProxy)** 뒤에 여러 `gateway_app` 인스턴스를 두고 수평 확장합니다.
`gateway_app`은 Redis Instance Registry를 조회해 backend(`server_app`)를 선택하고, 1:1 TCP 브리지를 구성합니다(Sticky + Least Connections).

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
- Boost.Asio Hive 위에서 다수의 TCP 세션을 처리합니다.
- 각 클라이언트 세션은 backend(`server_app`) 연결(`BackendSession`)과 1:1로 매칭되어 payload를 중계합니다.
- 인증은 `auth::IAuthenticator` 를 구현해 확장할 수 있습니다 (`ALLOW_ANONYMOUS`, `AUTH_PROVIDER`, `AUTH_ENDPOINT`).
- Redis SessionDirectory(`gateway/session/<client_id>`)로 sticky routing을 수행하고, Redis Instance Registry의 `active_sessions`를 기준으로 least-connections 방식으로 backend를 선택합니다.

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
| `REDIS_URI` | Redis URI (Instance Registry/SessionDirectory) | `tcp://127.0.0.1:6379` |
| `SERVER_REGISTRY_PREFIX` | Instance Registry key prefix (server_app과 동일) | `gateway/instances/` |
| `SERVER_REGISTRY_TTL` | Instance Registry TTL seconds | `30` |
| `METRICS_PORT` | `/metrics` HTTP 포트 | `6001` |
| `ALLOW_ANONYMOUS` | 익명 로그인 허용(1/0) | `1` |
| `AUTH_PROVIDER` / `AUTH_ENDPOINT` | 외부 인증 연동(옵션) | 빈 값 |

자세한 옵션은 `docs/configuration.md` 와 `docs/ops/gateway-and-lb.md` 를 참고하세요.

## 빌드 & 실행
```powershell
scripts/build.ps1 -Config Debug -Target gateway_app
.\build-windows\gateway\Debug\gateway_app.exe
```

멀티 인스턴스로 운영할 경우 외부 TCP 로드밸런서(예: HAProxy)에서 여러 `gateway_app`으로 분산시킵니다.
예시는 `docs/ops/gateway-and-lb.md` 를 참고하세요.
