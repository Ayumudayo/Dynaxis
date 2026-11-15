# 채팅 세션 재접속

logical_session_id는 TCP 연결 ID와 별개다. Postgres sessions에 resume 자격의 hash, 사용자 UUID, 만료/폐기 상태를 보관한다. Redis에는 마지막 방과 gateway의 backend/locator hint를 TTL로 저장한다.

signed 모드는 유효한 access credential과 resume credential을 모두 요구한다. backend는 lease 사용자와 검증된 principal이 같은지 확인한다. resume token만으로 다른 계정의 상태를 복구할 수 없다.

gateway는 유효한 sticky backend를 우선하고, 없으면 locator의 서비스 속성 및 현재 부하를 기준으로 정상 server를 선택한다. backend가 달라도 DB lease와 Redis 마지막 방으로 상태를 복원한다. 마지막 방 정보가 없으면 lobby로 복구하며 room owner 권한을 새로 부여하지 않는다.

`python tests/python/verify_continuity_recovery_matrix.py --no-build`는 gateway/server 재시작, locator fallback, 한 server가 중지된 상태의 failover를 실제 로컬 스택에서 검사한다. 검사 중 스택을 재기동하므로 운영 환경에서 실행하지 않는다.
