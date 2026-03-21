# MMORPG World Residency 및 Ownership 계약

이 문서는 현재 `main`에 병합된 world admission, residency, owner 경계, lifecycle-policy 계약을 기록한다.
이전 브랜치 시작 메모, tranche 종료 메모, world-lifecycle charter 메모는 모두 이 문서로 흡수되었다.

## 범위

- 내구성 있는 world residency는 continuity 기반 위에 올라간다.
- world 단위 owner 권한과 lifecycle policy는 현재 runtime 계약의 일부다.
- operator가 주도하는 live owner transfer commit도 현재 runtime 계약의 일부다.
- operator가 선언하는 cross-world migration envelope도 현재 runtime 계약의 일부다.
- local/proof stack의 startup 시점 topology 선택은 지원 표면에 포함된다.
- lease된 desired-topology pool로 유휴 실행 중 서버를 live reassignment하는 경로도 이제 지원 표면에 포함된다.
- 아래는 여전히 범위 밖이다.
  - elastic process spawn/retire
  - autoscaling
  - live zone migration
  - gameplay simulation state continuity
  - combat/world replication

## 소유권

### Gateway

- resume locator hint에는 `world_id`가 포함된다.
- `world_id`는 backend registry의 `world:<id>` tag 관례에서 읽는다.
- 정확한 resume alias binding이 사라져도, selector fallback이 같은 world/shard 경계 쪽으로 재접속을 좁힐 수 있다.

### Server

- 서버는 논리 세션의 durable world residency를 소유한다.
- 서버는 `SERVER_INSTANCE_ID`를 통해 현재 world owner 경계를 소유한다.
- room continuity는 world continuity와 owner continuity에 종속된다.
- world residency가 없거나 owner continuity가 없거나 불일치하면, 복구는 `default world + lobby`로 되돌아간다.

### Storage

- world residency는 별도의 continuity Redis key를 사용한다.
- world owner 권한은 `world_id` 기준의 전용 continuity Redis key를 사용한다.
- world lifecycle policy는 `dynaxis:continuity:world-policy:<world_id>`에 저장된다.
  - `draining=0|1`
  - `replacement_owner_instance_id=<instance_id>`
- world/owner/room 키는 같은 lease 모양 TTL 창을 공유한다.

## World lifecycle policy

### 제어면

- `admin_app`은 아래 경로로 world lifecycle policy를 노출한다.
  - `GET /api/v1/worlds`
  - `PUT /api/v1/worlds/{world_id}/policy`
  - `DELETE /api/v1/worlds/{world_id}/policy`
  - `GET /api/v1/worlds/{world_id}/drain`
  - `PUT /api/v1/worlds/{world_id}/drain`
  - `DELETE /api/v1/worlds/{world_id}/drain`
  - `GET /api/v1/worlds/{world_id}/transfer`
  - `PUT /api/v1/worlds/{world_id}/transfer`
  - `DELETE /api/v1/worlds/{world_id}/transfer`
  - `GET /api/v1/worlds/{world_id}/migration`
  - `PUT /api/v1/worlds/{world_id}/migration`
  - `DELETE /api/v1/worlds/{world_id}/migration`
- 제어면은 아래 질문에 답한다.
  - 지금 어떤 인스턴스가 해당 world를 소유하는가
  - 그 world가 draining 상태인가
  - replacement owner target이 선언되었는가
- 이름 붙은 owner transfer는 이제 아래를 할 수 있다.
  - lifecycle policy를 통해 replacement target을 선언
  - 새로운 로그인 부작용을 기다리지 않고 continuity world owner key를 replacement owner로 commit
  - owner 경계가 이동한 뒤 transfer policy 정리
- 이름 붙은 world drain은 이제 아래를 할 수 있다.
  - 같은 lifecycle policy primitive 위에 drain intent 선언
  - observed world inventory에서 남은 세션 수를 실시간으로 보고
  - fresh login 또는 owner-commit 부작용 없이 `draining_sessions`와 `drained`를 구분
  - 다음 closure action이 무엇인지 orchestration 상태로 노출
    - 대기 중인지
    - replacement target을 안정화 중인지
    - owner transfer를 commit해야 하는지
    - migration readiness를 기다리는지
    - policy를 지워도 되는지
- 이름 붙은 world migration은 이제 아래를 할 수 있다.
  - `source world -> target world owner` 의도를 desired topology와 lifecycle policy와 분리해 선언
  - core/runtime policy가 해석하지 않는 opaque app-defined payload reference를 운반
  - source world가 draining이고 target owner가 resumed backend와 일치할 때 continuity resume을 target world로 복구
- policy 쓰기는 권위 있는 Redis world-policy key에 직접 기록된다.

### 런타임 결정 규칙

- fresh admission은 새 backend 선택에서 draining owner를 제외한다. 단, 그 owner가 이미 선언된 replacement owner라면 예외다.
- resume 라우팅도 같은 draining-owner 제외 규칙을 적용한다.
- 서버 측 restore가 성립하려면 아래가 필요하다.
  - 영속 world residency 존재
  - 현재 backend와 owner continuity 일치
  - draining 상태라면, 선언된 replacement owner가 이미 현재 backend여야 함
- source world가 draining이어서 원래 world에 머무를 수 없을 때, migration envelope는 아래 조건을 만족하면 target world로 restore할 수 있다.
  - source world가 draining 상태
  - target owner가 현재 backend와 일치
  - 현재 backend가 이미 target world를 호스팅하거나, target world owner key가 현재 backend와 일치
- 현재 `server_app`은 generic envelope 위에 하나의 app-local payload seam을 더 둔다.
  - `payload_kind="chat-room-v1"`는 `payload_ref=<target room>`을 뜻한다.
  - migration restore가 성공하면 resumed room 경계는 generic `preserve_room` 대신 그 target room으로 이동한다.
- 위 조건을 만족하지 못하면 세션은 backend-safe 기본 world와 `lobby`로 되돌아간다.
- 현재 지원하는 drain closure 경로:
  - 이름 붙은 drain 선언
  - source owner의 active session이 0이 될 때까지 대기
  - 같은 world owner transfer commit 또는 migration readiness 충족
  - `drain.orchestration.next_action=clear_policy` 관측
  - drain policy를 명시적으로 정리

## 현재 topology 경계

- 현재 지원하는 topology 제어는 startup bootstrap + live runtime reassignment of idle running servers다.
- `docker/stack/topologies/*.json`은 local/proof 용 구체 startup manifest다.
- 기본 Docker topology는 계속 아래다.
  - `server-1 -> world:starter-a`
  - `server-2 -> world:starter-b`
- same-world proof topology는 별도 startup manifest로 유지한다.
- 현재 지원하는 proof 매핑:
  - 기본 topology: drain progress, drain-to-migration closure, cross-world migration handoff, app-local target-room migration handoff
  - same-world topology: 명시적 owner transfer commit, drain-to-owner-transfer closure
- background reconciliation과 자동 owner mutation은 현재 runtime 범위에 포함되지 않는다.
- 현재 runtime 범위에서는 Phase 3 acceptance를 사실상 종료된 것으로 본다.
  - runtime closure 경로가 반복 가능하게 증명되었다.
  - redeploy 없이 유휴 실행 중 서버를 desired pool로 다시 붙이는 live scale-out 경로 하나가 존재한다.
  - elastic spawn/retire와 autoscaling은 여전히 현재 지원 runtime 범위 밖이며, 승인된 Phase 3 계약에도 포함되지 않는다.

## Resume 규칙

- 새로운 로그인은 아래 우선순위로 `world_id`를 정한다.
  - `WORLD_ADMISSION_DEFAULT`가 있으면 그것
  - 없으면 `SERVER_TAGS`의 첫 `world:<id>` tag
  - 둘 다 없으면 `default`
- 새로운 로그인은 아래를 영속 저장한다.
  - world residency
  - world owner 권한
  - room continuity
- resume은 residency와 owner continuity가 유효하고, 필요한 draining policy도 이미 만족된 경우에만 `world_id`와 room을 복구한다.
- fallback reason은 계속 아래 네 가지를 사용한다.
  - `missing_world`
  - `missing_owner`
  - `owner_mismatch`
  - `draining_replacement_unhonored`

## 관측 신호

- 로그인 응답에는 `world_id`가 포함된다.
- 서버 메트릭은 world write/restore/fallback counter를 노출한다.
- 서버 메트릭은 app-local migration room handoff counter도 노출한다.
  - `chat_continuity_world_migration_payload_room_handoff_total`
  - `chat_continuity_world_migration_payload_room_handoff_fallback_total`
- runtime-assignment 문서가 존재하면 server registry heartbeat도 이를 반영하므로, `GET /api/v1/topology/observed`와 `GET /api/v1/topology/actuation`는 정상 heartbeat 창 안에서 live reassignment를 보여 준다.
- admin 메트릭은 world-drain write/clear counter를 노출한다.
- gateway 메트릭은 world-policy filter와 replacement-selection counter를 노출한다.
- admin 메트릭은 world-policy와 world-transfer write/clear/owner-commit counter도 노출한다.
- admin 메트릭은 world-migration write/clear counter도 노출한다.

## 현재 검증 대상

- `python tests/python/verify_stack_topology_generator.py`
- `python tests/python/verify_session_continuity.py`
- `python tests/python/verify_mmorpg_runtime_matrix.py --scenario phase3-acceptance`
- `python tests/python/verify_session_continuity_restart.py --scenario locator-fallback`
- `python tests/python/verify_session_continuity_restart.py --scenario world-residency-fallback`
- `python tests/python/verify_session_continuity_restart.py --scenario world-owner-fallback`
- `python tests/python/verify_session_continuity_restart.py --scenario world-owner-missing-fallback`
- `python tests/python/verify_session_continuity_restart.py --scenario world-drain-fallback`
- `python tests/python/verify_session_continuity_restart.py --scenario world-drain-reassignment`
- `python tests/python/verify_session_continuity_restart.py --scenario world-drain-progress`
- `python tests/python/verify_session_continuity_restart.py --scenario world-drain-transfer-closure`
- `python tests/python/verify_session_continuity_restart.py --scenario world-drain-migration-closure`
- `python tests/python/verify_session_continuity_restart.py --scenario world-owner-transfer-commit`
- `python tests/python/verify_session_continuity_restart.py --scenario world-migration-handoff`
- `python tests/python/verify_session_continuity_restart.py --scenario world-migration-target-room-handoff`
- `python tests/python/verify_admin_api.py`
- `python tests/python/verify_admin_auth.py`
- `python tests/python/verify_admin_read_only.py`
- 지원되는 Phase 3 runtime 경로 전체를 한 번에 exercise하려면, 개별 시나리오보다 `verify_mmorpg_runtime_matrix.py --scenario phase3-acceptance`를 우선 entrypoint로 사용한다.
- 정량 Phase 5 handoff/restart 증거의 해석 소유권은 `docs/ops/quantified-release-evidence.md`에 기록된다.

## 관련 계약

- desired topology, actuation, orchestration 계층 정의는 계속 `docs/ops/mmorpg-desired-topology-contract.md`를 따른다.
