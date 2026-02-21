# 전송 프로토콜(UDP) 전환 점검표(rollout/rollback)

이 문서는 UDP 수신(ingress) 카나리(canary) 오픈과 TCP-only 롤백을 운영자가 즉시 실행할 수 있도록 정리한다.

## 1. 준비

- compose 기반 리허설: `docker/stack/.env.udp-canary.example`, `docker/stack/.env.udp-rollback.example`
- gateway build flag 확인: `/metrics`에서 `gateway_udp_ingress_feature_enabled 1`
- 관측 스택(권장): `pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build -Observability`

## 2. 카나리 롤아웃 (gateway-1만)

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build -Observability -EnvFile docker/stack/.env.udp-canary.example
```

검증 포인트:
- `http://127.0.0.1:36001/metrics` -> `gateway_udp_enabled 1`
- `http://127.0.0.1:36002/metrics` -> `gateway_udp_enabled 0`
- `gateway_transport_delivery_forward_total{transport="udp",...}` 계열이 수집되는지 확인

## 3. 점진 확장

카나리 안정화 후 `.env` 오버라이드에서 `GATEWAY2_UDP_LISTEN=0.0.0.0:7000`을 설정해 확장한다.

확장 조건(최소):
- `GatewayUdpEstimatedLossHigh`, `GatewayUdpJitterHigh` 알람 없음
- `gateway_udp_bind_rate_limit_reject_total` 급증 없음
- TCP smoke(`python tests/python/verify_pong.py`) 통과

## 4. 즉시 롤백 (TCP-only)

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Observability -EnvFile docker/stack/.env.udp-rollback.example
```

롤백 검증:
- 모든 gateway에서 `gateway_udp_enabled 0`
- TCP smoke 통과: `python tests/python/verify_pong.py`
- 기존 세션/로그인 흐름이 유지되는지 확인(필요 시 `python tests/python/test_load_balancing.py` 추가)

## 5. 10분 리허설 실행

```powershell
pwsh scripts/rehearse_udp_rollout_rollback.ps1

# 이미지 재빌드 없이 재실행
pwsh scripts/rehearse_udp_rollout_rollback.ps1 -NoBuild
```

완료 기준:
- 카나리 오픈 -> 롤백까지 10분 이내
- 롤백 후 TCP 스모크(smoke) 성공
- 사후 기록에 원인/대응/재시도 조건 기재

## 6. 사후 분석 및 재시도 조건

필수 기록:
- 발생 지표: loss/jitter/replay/bind abuse
- 최초 대응 시각, rollback 완료 시각
- edge 차단 여부, bind 정책 변경값

재시도 조건:
- 이전 장애 원인에 대한 수정 적용
- 최소 24시간 알람 안정 상태 확인
- 카나리 범위를 1개 gateway에서 다시 시작
