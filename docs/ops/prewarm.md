# Pre-warm 절차

신규 서버를 배포하거나 대규모 재시작이 필요할 때 초기 트래픽을 안전하게 받아들이기 위한 준비 절차를 정리한다.

## 1. 사전 준비
1. `.env.server` 확인 – `DB_URI`, `REDIS_URI`, `METRICS_PORT`  
2. schema migration 수행: `./migrations_runner migrate`
3. Redis 캐시 초기화가 필요한 경우(`flushall` 금지) 스크립트로 필요한 키만 삭제

## 2. Pre-warm 단계
| 단계 | 내용 | 확인 |
| --- | --- | --- |
| 1 | 서버 프로세스 기동 (readiness off) | `/healthz` 200 |
| 2 | `server_app --prewarm` 모드로 snapshot + Redis 채널을 미리 로드 | 로그 `prewarm complete` 확인 |
| 3 | `/metrics` 확인, `chat_session_active`=0, 오류 없음 | curl |
| 4 | 로드밸런서에서 새 backend를 링에 추가 | `lb_hash_rebuild_total` 증가 |
| 5 | 게이트웨이 라우팅 테스트 (smoke) | devclient `connect → /rooms` |
| 6 | readiness on, HPA/Service에 편입 | `kubectl rollout status` |

## 3. 자동화 스크립트 예시
```bash
#!/usr/bin/env bash
set -euo pipefail

kubectl rollout restart deploy/server-app
kubectl rollout status deploy/server-app

for pod in $(kubectl get pod -l app=server-app -o jsonpath='{.items[*].metadata.name}'); do
  kubectl exec "$pod" -- /app/server_app --prewarm
done
```

## 4. 주의 사항
- Pre-warm 중에는 외부에서 트래픽이 유입되지 않도록 게이트웨이에서 backend를 disable 한다.  
- Redis Pub/Sub 채널을 미리 구독해 두면 첫 메시지 손실을 막을 수 있다.  
- runbook에 해당 절차를 기록하고 배포 자동화와 연계한다.
