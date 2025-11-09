# Gateway & Load Balancer 운영 가이드

## 1. 구성 개요
- **Gateway**  
  - TCP/WebSocket 클라이언트를 수용하고 gRPC 스트림을 통해 Load Balancer와 통신  
  - 인증(`auth::IAuthenticator`) 후 `LbSession` 을 생성
- **Load Balancer**  
  - Consistent Hash + Sticky Session( Redis ) 조합으로 backend 선택  
  - Gateway와 gRPC 스트림, Backend와 TCP 소켓 사이에서 데이터 프락시 역할

## 2. 주요 환경 변수
| 변수 | 설명 |
| --- | --- |
| `GATEWAY_LISTEN` | 게이트웨이 수신 주소 (기본 `0.0.0.0:6000`) |
| `LB_GRPC_ENDPOINT` | 게이트웨이가 연결할 LB 주소 |
| `LB_BACKEND_ENDPOINTS` | 정적 backend 목록 (`host:port,host:port` 형식) |
| `LB_DYNAMIC_BACKENDS` | `1` 로 설정 시 registry 기반 동적 재구성 활성화 |
| `LB_BACKEND_IDLE_TIMEOUT` | backend 프락시 유휴 시간 제한(초) |

## 3. 동작 흐름
1. 클라이언트가 Gateway에 접속 → HELLO/인증 → `open_lb_session()`  
2. Gateway는 LB에 gRPC 스트림을 열고 `ROUTE_KIND_CLIENT_HELLO` 전송  
3. LB는 backend를 할당하고 TCP 연결을 생성  
4. 데이터가 양방향으로 전달되며, heartbeat는 `ROUTE_KIND_HEARTBEAT` 로 유지  
5. 연결 종료 시 Gateway는 `ROUTE_KIND_CLIENT_CLOSE` 를 보내고 세션을 정리

## 4. 장애 대응
- **gRPC 스트림 오류** – Gateway는 재연결 시도를 로그와 함께 남기며, LB는 `mark_backend_failure()` 로 backend를 격리  
- **Backend 유휴** – `LB_BACKEND_IDLE_TIMEOUT` 을 초과하면 LB가 강제 종료 후 `lb_backend_idle_close_total` 지표를 갱신  
- **Redis 불가** – Sticky session을 새로 계산하고, 사용 중인 backend에 fallback된다.

## 5. 모니터링
- Gateway 로그: `metric=publish_total`, `metric=subscribe_total`, `metric=self_echo_drop_total`  
- Load Balancer 로그: `metric=lb_backend_idle_close_total`, backend 성공/실패 카운터  
- Grafana 패널 “LB Idle Close Rate” 를 통해 5분당 idle 종료 횟수를 확인

## 6. 배포 팁
- Gateway/LB 모두 readiness/liveness probe 를 설정, 재시작 한계를 명확히 한다.  
  ```yaml
  livenessProbe:
    tcpSocket:
      port: 6000
    initialDelaySeconds: 5
    periodSeconds: 10
  ```
- ConfigMap/Secret 을 분리해 `.env.gateway`, `.env.lb` 를 관리하고, Helm values 로 주입한다.  
- Prometheus `ServiceMonitor` 나 AWS NLB Health Check 를 통해 상태를 추적한다.

## 7. 참고 문서
- `docs/ops/distributed_routing_draft.md` – 분산 라우팅 설계 초안  
-. `docs/ops/fallback-and-alerts.md` – 알람/대응 전략  
- `docs/ops/observability.md` – 지표/대시보드
