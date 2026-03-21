# 세션 연속성(Session Continuity) 계약

이 문서는 현재 `main`에 병합된 세션 연속성 계약을 기록한다.
초기 브랜치 시작 메모와 tranche 종료 메모는 이 문서로 흡수되었고, 지금은 이 파일이 살아 있는 기준 문서다.

## 범위

- continuity는 기존 chat/control 스택 위에서 동작하는 인증된 논리 세션을 다룬다.
- 영속화되는 continuity 표면은 의도적으로 좁게 유지한다.
  - 실제 사용자 식별자
  - 논리 세션 ID
  - resume lease 만료 시각
  - 마지막 가벼운 방/위치 정보
- gameplay 상태 복구, zone migration, 실시간 FPS transport continuity는 범위 밖이다.

## 소유권

### 서버

- `server_app`이 continuity lease 발급과 검증을 소유한다.
- 새로운 인증 로그인은 아래 값을 발급한다.
  - `logical_session_id`
  - `resume_token`
  - `resume_expires_unix_ms`
  - `resumed`
- resume 로그인은 `resume:<token>` 형식을 사용한다.
- 유효한 영속 lease가 있으면 실제 사용자 식별자와 마지막 continuity 방을 복구한다.
- 유효하지 않거나 오래된 resume token은 `UNAUTHORIZED`로 거절한다.

### 게이트웨이

- `gateway_app`은 resume 시 클레임된 재접속 사용자 이름을 신뢰하지 않는다.
- 대신 원시 resume token을 `resume-hash:<sha256(token)>` 형태로 해시해 sticky reconnect 선택 키로 사용한다.
- 로그인 응답이 성공하면 gateway는 이 alias와 최소 locator 힌트를 `SessionDirectory`에 저장한다.
- locator 힌트에는 아래가 들어간다.
  - `role`
  - `game_mode`
  - `region`
  - `shard`
  - `backend_instance_id`
- 정확한 alias binding이 사라졌더라도, locator 힌트가 전역 least-load fallback 이전에 재접속 후보를 같은 shard 경계로 좁혀 준다.

### 저장소

- continuity lease의 소유 저장 경로는 기존 `sessions` repository를 그대로 사용한다.
- 마지막 가벼운 방/위치는 Redis continuity 키에도 함께 반영한다.
- join, leave, login은 room snapshot TTL과 lease window를 함께 갱신한다.
- gateway는 같은 lease 모양 TTL을 가진 sibling Redis 키에 resume locator 힌트를 저장한다.

## 결정 규칙

### 새로운 로그인

- `resume:` 접두사가 없다.
- 일반 인증 경로를 탄다.
- continuity가 활성화되어 있고 로그인에 영속 사용자 식별자가 있으면 새 continuity lease를 발급한다.

### resume 로그인

- gateway는 해시된 resume alias로 라우팅한다.
- server는 영속 continuity lease를 검증한다.
- 검증이 성공하면:
  - 논리 사용자 식별자와 마지막 continuity 방을 복구한다.
  - `resumed=true`를 반환한다.
- 검증이 실패하면:
  - 로그인 자체를 거절한다.
  - 새 로그인으로 조용히 낮춰 처리하지 않는다.

## 재시작 기대치

- gateway 재시작:
  - 클라이언트는 다른 살아 있는 gateway로 재접속할 수 있다.
  - resume alias는 `SessionDirectory`를 통해 계속 남는다.
  - 정확한 alias binding이 사라져도 locator 메타데이터가 같은 shard 경계 쪽으로 재접속 후보를 좁혀 준다.
- server 재시작:
  - continuity lease는 lease TTL 안에서 계속 유효하다.
  - backend가 돌아오면 영속 continuity 상태를 기준으로 resume이 결정론적으로 성공해야 한다.

## 현재 검증 대상

- `python tests/python/verify_session_continuity.py`
- `python tests/python/verify_continuity_recovery_matrix.py --scenario phase5-recovery-baseline`
- `python tests/python/verify_session_continuity_restart.py --scenario gateway-restart`
- `python tests/python/verify_session_continuity_restart.py --scenario server-restart`
- `python tests/python/verify_session_continuity_restart.py --scenario locator-fallback`
- Phase 5 증거를 개별 시나리오 수동 실행이 아니라 하나의 반복 가능한 시퀀스로 모아야 할 때는
  `verify_continuity_recovery_matrix.py --scenario phase5-recovery-baseline`를 우선 entrypoint로 사용한다.

## 하위 관계

- world residency/lifecycle 작업은 이 계약 위에 올라가지만, 이 계약을 대체하지는 않는다.
- continuity는 더 큰 MMORPG runtime 계약에서도 여전히 기본 복구 계층이다.
