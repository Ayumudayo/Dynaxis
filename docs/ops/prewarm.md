# Pre-warm 절차 (상세)

새로운 서버/게이트웨이/로드밸런서 인스턴스를 투입할 때, 실제 트래픽을 받기 전에 미리 워밍업하는 절차를 정의한다.

## 1. 체크리스트
| 항목 | 내용 |
| --- | --- |
| 이미지 | 최신 태그인지, 취약점 스캔 완료인지 확인 |
| 구성 | `.env.server`, `.env.gateway`, `.env.lb` 각자 유효성 검사 |
| 데이터 | Schema migration 선적용 (`migrations_runner migrate`) |
| 모니터링 | `/metrics`, 로그 파이프라인 연결 상태 확인 |

## 2. Pre-warm 단계
1. **베이스 Pod 기동** – Deployment replica=1, readinessProbe 비활성화 상태로 시작
2. **캐시 로딩**
   - `server_app --prewarm` → 최근 스냅샷 + Redis 키 생성
   - `load_balancer_app --prewarm` (TODO) → Consistent Hash 링과 Sticky Cache warm-up
3. **헬스체크** – `/healthz`, `/metrics` 확인, `chat_session_active`=0 로그 확인
4. **프록시 연결 테스트** – Gateway ↔ Load Balancer ↔ server_app 로 테스트 클라이언트 연결
5. **readinessProbe 활성화** – Helm values 또는 `kubectl patch` 로 readiness 를 다시 true
6. **HPA/Service 합류** – 스케일 아웃 시 기존 replica 의 1/3 간격으로 순차 배포

## 3. 운영 자동화 스크립트
```bash
#!/usr/bin/env bash
set -euo pipefail
APP=server-app
helm upgrade --install $APP charts/$APP -f values/prod.yaml --set prewarm=true
kubectl rollout status deploy/$APP
for pod in $(kubectl get pod -l app=$APP -o name); do
  kubectl exec "$pod" -- /app/server_app --prewarm
  kubectl exec "$pod" -- curl -sf http://localhost:9090/metrics >/dev/null
  kubectl patch "$pod" -p '{"metadata":{"labels":{"prewarm":"done"}}}'
done
```
`prewarm=true` 플래그는 readinessProbe 를 일시적으로 끄고, 완료 후 다시 켜도록 Helm 템플릿에서 처리한다.

## 4. 롤백 시 주의
- Pre-warm 도중 배포를 중단하면 readiness 가 off 인 Pod 가 남을 수 있으므로 `kubectl delete pod -l prewarm=init` 로 정리
- Sticky session 캐시는 오래된 backend 를 가리킬 수 있으므로 Redis `gateway/session/*` 키를 스캔해 TTL 을 줄인다.

## 5. 참고
- `docs/ops/deployment.md`
- `docs/ops/runbook.md`
