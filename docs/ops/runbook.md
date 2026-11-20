# 로컬 운영 확인

1. `pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build`로 실행한다.
2. server 39091/39092, gateway 36001/36002, worker 39093의 `/readyz`를 확인한다.
3. `verify_pong.py`, `verify_chat.py`, `verify_whisper_cross_instance.py`로 전달을 검사한다.
4. 장애 시 readiness와 backend/worker 오류 메트릭을 함께 확인한다. pending 이벤트는 DB 복구 후 적재되어야 한다.
5. 종료는 `pwsh scripts/deploy_docker.ps1 -Action down`을 사용한다. 데이터 volume 삭제는 일반 종료에 포함하지 않는다.

[인증 설정](authenticated-chat.md)과 [실패·복구 검사](../tests.md)를 따른다.
