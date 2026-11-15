# 장애와 복구

Redis 장애는 discovery, 신규 routing, Pub/Sub, presence와 room cache를 함께 영향받게 한다. DB/Redis readiness와 실제 노드 간 전달을 따로 확인한다. Redis 복구 후 subscriber는 재연결을 계속 시도한다. Pub/Sub의 장애 중 메시지는 재전송되지 않는다.

Postgres 장애에서는 server와 worker readiness가 내려가며 worker는 backoff 후 재연결한다. Streams pending 이벤트가 복구 후 적재되는지 확인한다. 실패 이벤트의 ACK/DLQ 정책은 [write-behind](../db/write-behind.md)를 따른다.

[관측](observability.md)과 [실패·복구 검증](../tests.md)을 함께 사용한다.
