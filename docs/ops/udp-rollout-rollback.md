# 전송 프로토콜(UDP) 전환 점검표

이 문서는 UDP 수신(ingress) 카나리(canary) 오픈과 TCP 전용 롤백을 운영자가 즉시 실행할 수 있도록 정리한다. 목표는 명령만 나열하는 것이 아니라, "언제 열고", "무엇을 보고", "어느 조건에서 바로 되돌려야 하는가"를 한 장에서 읽히게 하는 것이다.

이런 점검표가 필요한 이유는 UDP 계층의 실패가 보통 연결 자체보다 품질 저하 형태로 먼저 나타나기 때문이다. 즉, 완전히 끊어지기 전에도 loss, jitter, bind abuse 같은 신호가 먼저 악화될 수 있다. 그래서 rollout과 rollback 기준을 미리 고정해 두는 편이 운영에 훨씬 안전하다.

## 1. 준비

- compose 기반 리허설: `docker/stack/.env.udp-canary.example`, `docker/stack/.env.udp-rollback.example`
- gateway 빌드 플래그 확인: `/metrics`에서 `gateway_udp_ingress_feature_enabled 1`
- 관측 스택 권장 기동: `pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build -Observability`

준비 단계를 먼저 두는 이유는 UDP rollout이 단순 환경 변수 변경이 아니기 때문이다. 빌드, 설정, 관측 스택이 모두 준비되어 있어야 "정말 기능이 꺼져 있었는지", "지금 새로 열린 것인지"를 구분할 수 있다.

## 2. 카나리 롤아웃 (gateway-1만)

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build -Observability -EnvFile docker/stack/.env.udp-canary.example
```

검증 포인트:

- `http://127.0.0.1:36001/metrics` -> `gateway_udp_enabled 1`
- `http://127.0.0.1:36002/metrics` -> `gateway_udp_enabled 0`
- `gateway_transport_delivery_forward_total{transport="udp",...}` 계열이 수집되는지 확인

한 gateway만 먼저 여는 이유는 문제를 좁은 반경에서 확인하기 위해서다. 두 대를 동시에 열면 증상은 더 빨리 보일 수 있어도, 어느 인스턴스나 어떤 경로에서 문제가 시작됐는지 추적하기 어려워진다.

## 3. 점진 확장

카나리 안정화 뒤 `.env` override에서 `GATEWAY2_UDP_LISTEN=0.0.0.0:7000`을 설정해 확장한다.

확장 조건(최소):

- `GatewayUdpEstimatedLossHigh`, `GatewayUdpJitterHigh` 알람 없음
- `gateway_udp_bind_rate_limit_reject_total` 급증 없음
- TCP 스모크(`python tests/python/verify_pong.py`) 통과

확장 전에 TCP 스모크를 다시 확인하는 이유는 UDP를 열었다고 해서 TCP 기본 경로가 건드려지지 않는다고 가정하면 안 되기 때문이다. 게이트웨이 설정과 리소스 사용량 변화는 기존 경로에도 영향을 줄 수 있다.

## 4. 즉시 롤백 (TCP 전용)

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Observability -EnvFile docker/stack/.env.udp-rollback.example
```

롤백 검증:

- 모든 gateway에서 `gateway_udp_enabled 0`
- TCP 스모크 통과: `python tests/python/verify_pong.py`
- 기존 세션과 로그인 흐름이 유지되는지 확인(필요 시 `python tests/python/test_load_balancing.py` 추가)

롤백 검증을 따로 적는 이유는 "설정을 되돌렸다"와 "서비스가 정상 상태로 복귀했다"가 같은 뜻이 아니기 때문이다. 운영자는 설정값만이 아니라 실제 접속 경로가 이전 상태로 돌아왔는지도 확인해야 한다.

## 5. 10분 리허설 실행

```powershell
pwsh scripts/rehearse_udp_rollout_rollback.ps1

# 이미지 재빌드 없이 재실행
pwsh scripts/rehearse_udp_rollout_rollback.ps1 -NoBuild
```

완료 기준:

- 카나리 오픈에서 롤백까지 10분 이내
- 롤백 뒤 TCP 스모크 성공
- 사후 기록에 원인, 대응, 재시도 조건 기재

리허설 시간을 제한하는 이유는 실제 장애에서 가장 비싼 것은 "무엇을 해야 할지 몰라서 멈춰 있는 시간"이기 때문이다. 10분 안에 반복할 수 없는 절차는 실전에서도 흔들릴 가능성이 높다.

## 6. 사후 분석과 재시도 조건

필수 기록:

- 발생 지표: loss, jitter, replay, bind abuse
- 최초 대응 시각, rollback 완료 시각
- edge 차단 여부, bind 정책 변경값

재시도 조건:

- 이전 장애 원인에 대한 수정 적용
- 최소 24시간 알람 안정 상태 확인
- 카나리 범위를 1개 gateway에서 다시 시작

재시도를 더 좁은 범위에서 다시 시작하는 이유는, 한 번 문제를 낸 설정이나 코드가 "어쩌다 한 번"이었는지 확신할 수 없기 때문이다. 운영에서는 항상 가장 작은 반경부터 다시 증명하는 편이 안전하다.
