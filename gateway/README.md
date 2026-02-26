# 게이트웨이 애플리케이션(gateway_app)

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
- 각 클라이언트 세션은 backend(`server_app`) 연결(`BackendConnection`)과 1:1로 매칭되어 payload를 중계합니다.
- 브리지 경로(client -> backend)는 raw-byte enqueue 경로를 사용해 불필요한 임시 `std::vector<std::uint8_t>` 생성을 줄였습니다.
- 인증은 `auth::IAuthenticator` 를 구현해 확장할 수 있습니다 (`ALLOW_ANONYMOUS`, `AUTH_PROVIDER`, `AUTH_ENDPOINT`).
- Redis SessionDirectory(`gateway/session/<client_id>`)로 sticky routing을 수행하고, Redis Instance Registry의 `active_sessions`를 기준으로 least-connections 방식으로 backend를 선택합니다.

### 왜 Sticky + Least Connections를 함께 쓰는가
- **Sticky만 사용**하면 특정 backend로 세션이 고착되어 장기적으로 부하가 한쪽으로 쏠릴 수 있습니다.
- **Least Connections만 사용**하면 재접속 사용자가 매번 다른 backend로 이동해 세션 연속성이 떨어질 수 있습니다.
- 현재 방식은 "이미 유효한 바인딩은 재사용(Sticky)"하고, 바인딩이 없거나 만료된 경우에만 "가장 한가한 서버 선택(Least Connections)"을 적용해
  사용자 경험과 분산 효율을 동시에 맞춥니다.

### 인증(Auth) 확장 예시
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
| `SERVER_REGISTRY_PREFIX` | Instance Registry 키 접두사 (server_app과 동일) | `gateway/instances/` |
| `SERVER_REGISTRY_TTL` | Instance Registry TTL(초) | `30` |
| `METRICS_PORT` | `/metrics` HTTP 포트 | `6001` |
| `GATEWAY_BACKEND_CONNECT_TIMEOUT_MS` | backend connect timeout(ms) | `5000` |
| `GATEWAY_BACKEND_SEND_QUEUE_MAX_BYTES` | backend 전송 대기 큐 상한 바이트 | `262144` |
| `ALLOW_ANONYMOUS` | 익명 로그인 허용(1/0), `0`이면 토큰 없는/anonymous 로그인 거부 | `1` |
| `AUTH_PROVIDER` / `AUTH_ENDPOINT` | 외부 인증 연동(옵션) | 빈 값 |

주요 gateway 메트릭:
- `gateway_sessions_active`, `gateway_connections_total`
- `gateway_backend_resolve_fail_total`, `gateway_backend_connect_fail_total`, `gateway_backend_connect_timeout_total`
- `gateway_backend_write_error_total`, `gateway_backend_send_queue_overflow_total`

자세한 옵션은 `docs/configuration.md` 와 `docs/ops/gateway-and-lb.md` 를 참고하세요.

## 빌드
```powershell
scripts/build.ps1 -Config Debug -Target gateway_app
```

## 실행 (권장: Linux/Docker)
로컬/운영 환경에서는 `gateway_app`을 **HAProxy 뒤**에 두고 Linux 런타임에서 실행하는 형태를 표준으로 둔다.

```powershell
scripts/deploy_docker.ps1 -Action up -Detached -Build
```

예시는 `docker/stack/docker-compose.yml`, `docker/stack/haproxy/haproxy.cfg` 와 `docs/ops/gateway-and-lb.md` 를 참고하세요.
