# 로컬 MSA 채팅 스택

`pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build`로 실행한다. HAProxy 6000 → gateway 2개 → server 2개와 Redis/Postgres/worker/migrator를 구성한다. startup topology manifest는 server 인스턴스와 포트를 정의한다.

관리 콘솔은 포함하지 않는다. 선택 observability는 `-Observability`로 실행한다. [인증](../../docs/ops/authenticated-chat.md), [검증](../../docs/tests.md).
