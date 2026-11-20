# 로컬 MSA 채팅 스택

`pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build`로 실행한다. HAProxy 6000 → gateway 2개 → server 2개와 Redis/Postgres/worker/migrator를 구성한다. startup topology manifest는 server 인스턴스와 포트를 정의한다.

웹 관리 콘솔(`admin`, v1 조회·v2 채팅 운영·v3 비동기 재시작/설정 제어, 기본 `ADMIN_READ_ONLY=1`)은 기본 호스트 포트 `39200`에서 제공되며, v3 호스트 연동 작업은 [호스트 실행 어댑터](../../tools/admin_host_adapter.py)로 처리한다. 선택 observability는 `-Observability`로 실행한다. [웹 관리 콘솔](../../docs/architecture/web-admin-console.md), [인증](../../docs/ops/authenticated-chat.md), [검증](../../docs/tests.md).
