# gateway_app

TCP 연결의 로그인 credential을 검증하고 Redis registry의 정상 backend를 선택해 바이트를 중계한다. sticky routing, 부하 기반 선택, resume alias/locator를 지원한다. backend timeout·송신 큐·retry budget·circuit breaker로 장애 전파를 제한한다.

[인증](../docs/ops/authenticated-chat.md) · [운영](../docs/ops/gateway-and-lb.md)
