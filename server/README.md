# server_app

채팅 로그인·방·메시지·귓속말·history·presence·권한·재접속을 소유한다. Redis fanout과 Postgres repository를 조립하고 gateway의 인증 결과와 별개로 access credential을 검증한다.

[설정](../docs/configuration.md) · [인증](../docs/ops/authenticated-chat.md) · [재접속](../docs/ops/session-continuity-contract.md)
