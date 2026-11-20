# 관측

각 gateway/server/worker는 `/metrics`, `/healthz`, `/readyz`를 제공한다. health는 프로세스 상태, readiness는 선언된 DB/Redis 의존성과 서비스 준비 상태를 나타낸다. DB/Redis 장애에서 readiness가 내려가고 복구 후 올라가는지 종단 검사한다.

`pwsh scripts/run_full_stack_observability.ps1`로 Prometheus/Grafana/exporter를 함께 실행한다. 기본 host 포트는 Prometheus 39090, Grafana 33000이다. `scripts/check_observability.ps1`과 `scripts/check_prometheus_rules.ps1`로 대상을 확인한다.

필수 관측은 active sessions, queue depth, 송신 거절, backend 연결/timeout/retry, dependency readiness, worker pending/flush/error/drop, session resume 성공/실패, build info다. idle latency quantile은 NaN일 수 있다. 메시지 내용이나 access/resume credential은 진단 로그에 기록하지 않는다.
