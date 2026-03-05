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
| `GATEWAY_BACKEND_CIRCUIT_BREAKER_ENABLED` | backend circuit breaker 활성화(1/0) | `1` |
| `GATEWAY_BACKEND_CIRCUIT_FAIL_THRESHOLD` | circuit open 전환 연속 실패 임계치 | `5` |
| `GATEWAY_BACKEND_CIRCUIT_OPEN_MS` | circuit open 유지 시간(ms) | `10000` |
| `GATEWAY_BACKEND_CONNECT_RETRY_BUDGET_PER_MIN` | backend connect 재시도 예산(분당) | `120` |
| `GATEWAY_BACKEND_CONNECT_RETRY_BACKOFF_MS` | backend connect 재시도 백오프 시작값(ms) | `200` |
| `GATEWAY_BACKEND_CONNECT_RETRY_BACKOFF_MAX_MS` | backend connect 재시도 백오프 상한(ms) | `2000` |
| `GATEWAY_INGRESS_TOKENS_PER_SEC` | ingress 토큰 버킷 초당 토큰 | `200` |
| `GATEWAY_INGRESS_BURST_TOKENS` | ingress 토큰 버킷 burst 용량 | `400` |
| `GATEWAY_INGRESS_MAX_ACTIVE_SESSIONS` | 동시 backend 세션 상한 | `50000` |
| `GATEWAY_UDP_LISTEN` | UDP ingress listen 주소(`host:port`) | (unset) |
| `GATEWAY_UDP_BIND_SECRET` | UDP bind ticket 서명 키 | (unset) |
| `GATEWAY_UDP_BIND_TTL_MS` | UDP bind ticket TTL(ms) | `5000` |
| `GATEWAY_UDP_BIND_FAIL_WINDOW_MS` | UDP bind 실패 누적 윈도우(ms) | `10000` |
| `GATEWAY_UDP_BIND_FAIL_LIMIT` | UDP bind 실패 허용 횟수(윈도우 내) | `5` |
| `GATEWAY_UDP_BIND_BLOCK_MS` | UDP bind endpoint block 시간(ms) | `60000` |
| `GATEWAY_UDP_BIND_RETRY_BACKOFF_MS` | 세션별 bind 재시도 backoff 시작값(ms) | `200` |
| `GATEWAY_UDP_BIND_RETRY_BACKOFF_MAX_MS` | 세션별 bind 재시도 backoff 상한(ms) | `2000` |
| `GATEWAY_UDP_BIND_RETRY_MAX_ATTEMPTS` | 세션별 bind 재시도 backoff 지수 증가 상한 attempt | `6` |
| `GATEWAY_UDP_OPCODE_ALLOWLIST` | UDP ingress 허용 opcode CSV(미설정/빈 값이면 `MSG_UDP_BIND_REQ`만 허용) | (unset) |
| `GATEWAY_RUDP_ENABLE` | RUDP adapter 활성화(1/0, 기본 OFF) | `0` |
| `GATEWAY_RUDP_CANARY_PERCENT` | 신규 세션 RUDP canary 비율(0~100) | `0` |
| `GATEWAY_RUDP_OPCODE_ALLOWLIST` | RUDP 허용 opcode CSV(빈 값이면 비활성) | (empty) |
| `ALLOW_ANONYMOUS` | 익명 로그인 허용(1/0), `0`이면 토큰 없는/anonymous 로그인 거부 | `1` |
| `AUTH_PROVIDER` / `AUTH_ENDPOINT` | 외부 인증 연동(옵션) | 빈 값 |

주요 gateway 메트릭:
- `gateway_sessions_active`, `gateway_connections_total`
- `gateway_backend_resolve_fail_total`, `gateway_backend_connect_fail_total`, `gateway_backend_connect_timeout_total`
- `gateway_backend_write_error_total`, `gateway_backend_send_queue_overflow_total`
- `gateway_backend_circuit_open_total`, `gateway_backend_circuit_reject_total`, `gateway_backend_circuit_open`
- `gateway_backend_connect_retry_total`, `gateway_backend_retry_budget_exhausted_total`
- `gateway_ingress_reject_not_ready_total`, `gateway_ingress_reject_rate_limit_total`, `gateway_ingress_reject_session_limit_total`, `gateway_ingress_reject_circuit_open_total`
- `gateway_udp_bind_*`, `gateway_udp_opcode_allowlist_*`
- `gateway_rudp_core_build_enabled`, `gateway_rudp_enabled`, `gateway_rudp_canary_percent`, `gateway_rudp_opcode_allowlist_size`
- `gateway_rudp_packets_total`, `gateway_rudp_packets_reject_total`, `gateway_rudp_inner_forward_total`, `gateway_rudp_fallback_total`

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
