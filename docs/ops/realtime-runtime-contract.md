# 실시간 런타임(Realtime Runtime) 계약

이 문서는 현재 `main`에 병합된 실시간 지향 전송/런타임 기반 계약을 기록한다.
지금 이 기반은 주로 FPS 입력 workload를 통해 검증되며, 공개 엔진 계약의 기준 표면은 `server/core/realtime/**`다.

## 기준 표면

- 기준 공개 헤더:
  - `server/core/realtime/direct_bind.hpp`
  - `server/core/realtime/direct_delivery.hpp`
  - `server/core/realtime/simulation_phase.hpp`
  - `server/core/realtime/transport_quality.hpp`
  - `server/core/realtime/transport_policy.hpp`
  - `server/core/realtime/runtime.hpp`
- 기준 네임스페이스: `server::core::realtime`

## 현재 범위

- direct UDP/RUDP 증명은 이제 attach-only 가시성을 넘어 direct `MSG_PING`과 direct `MSG_FPS_INPUT` 유입까지 포함한다.
- 서버는 authoritative 2D actor transform을 위한 공개 `server/core/realtime/runtime.hpp` fixed-step runtime을 가진다.
- 현재 복제(replication)는 신뢰형 TCP snapshot과 gameplay 주파수의 delta 전달을 함께 사용한다.
- 거친 관심 범위 필터링과 actor history 보존은 게임 규칙이 아니라 엔진 기반(substrate) 기능으로 구현한다.

## 소유권 경계

### 전송 유입

- `gateway_app`은 승인된 증명/게임 기반 메시지에 대해서만 direct UDP/RUDP 유입을 허용한다.
- `MSG_PING`과 `MSG_FPS_INPUT`은 정책/환경 변수가 허용할 때 direct UDP/RUDP로 들어올 수 있다.
- 지원하지 않는 workload는 계속 TCP 전용이며, 조용히 성능 저하로 넘어가지 말고 시나리오 검증에서 실패해야 한다.

### fixed-step runtime

- realtime runtime은 자체 fixed-step 타이머/accumulator를 사용하며, `TaskScheduler`를 authoritative tick으로 재활용하지 않는다.
- runtime이 소유하는 것:
  - 세션별 최신 staged input
  - 첫 FPS input 시 actor 생성
  - authoritative actor transform 전진
  - 거친 관심 범위 선택
  - actor별 history 보존
  - authoritative tick 내부 phase의 결정론적 관측 어휘

### 복제 경계

- authoritative 상태는 의도적으로 최소만 유지한다.
  - `actor_id`
  - `x_mm`
  - `y_mm`
  - `yaw_mdeg`
  - `last_applied_input_seq`
  - `server_tick`
- 신뢰형 재동기화(resync)는 `MSG_FPS_STATE_SNAPSHOT`을 통해 TCP에 남긴다.
- gameplay 주파수 상태 업데이트는 `MSG_FPS_STATE_DELTA`를 사용한다.
  - UDP 바인딩 세션은 direct UDP delta를 받는다.
  - 이미 성립한 RUDP 세션은 direct RUDP delta를 받는다.
  - rollout 비활성 / canary miss 세션은 UDP bind가 끝난 뒤 direct UDP에 남는다.
  - 바인딩되지 않은 세션과 RUDP fallback이 고정된 세션은 기존 TCP bridge 경로로 돌아간다.
  - RUDP가 선택됐지만 아직 완전히 성립하지 않은 세션은 handshake가 끝나거나 fallback이 고정될 때까지 임시로 UDP direct delivery를 사용한다.

## 관심 범위와 history 규칙

- 관심 범위 관리는 세밀한 relevance logic이 아니라 거친 셀 기반 선택이다.
- 가시성 변화가 생기면 더 복잡한 부분 복구 프로토콜을 새로 만들지 않고 새 snapshot을 큐에 넣는다.
- 더러운 visible actor 수가 구성된 delta budget을 넘으면, runtime은 그 viewer에 대해 delta fanout 대신 snapshot 복구로 되돌아간다.
- actor history는 "해당 tick 이하에서 가장 최근 샘플"을 찾기 위한 bounded ring buffer로 유지한다.
- lag compensation, rewind hit validation, combat, shooter gameplay rule은 범위 밖이다.
- simulation phase observer는 관측 계약일 뿐이며, respawn/combat/objective 같은 gameplay manager를 core에 승격하는 근거가 아니다.

## 현재 검증 표면

- 결정론적 전송 증명:
  - `tools/loadgen/scenarios/udp_ping_only.json`
  - `tools/loadgen/scenarios/rudp_ping_only.json`
  - `tools/loadgen/scenarios/udp_fps_input_only.json`
  - `tools/loadgen/scenarios/rudp_fps_input_only.json`
- mixed soak 증명:
  - `tools/loadgen/scenarios/mixed_direct_udp_ping_soak.json`
  - `tools/loadgen/scenarios/mixed_direct_rudp_ping_soak.json`
  - `tools/loadgen/scenarios/mixed_direct_udp_fps_soak.json`
  - `tools/loadgen/scenarios/mixed_direct_rudp_fps_soak.json`
- stack/runtime 검증:
  - `CorePublicApiRealtimeCapabilitySmoke`
  - `tests/python/verify_fps_state_transport.py`
  - `tests/python/verify_fps_rudp_transport.py --scenario attach`
  - `tests/python/verify_fps_rudp_transport.py --scenario off`
  - `tests/python/verify_fps_rudp_transport.py --scenario rollout-fallback`
  - `tests/python/verify_fps_rudp_transport.py --scenario protocol-fallback`
  - `tests/python/verify_fps_rudp_transport.py --scenario udp-quality-impairment`
  - `tests/python/verify_fps_rudp_transport.py --scenario restart`
  - `tests/python/verify_fps_rudp_transport_matrix.py --scenario phase2-acceptance`
  - `tests/python/verify_fps_rudp_transport_matrix.py`
  - 결정론적 손상 direct-UDP 증명은 이제 `gateway_udp_loss_estimated_total`, `gateway_udp_jitter_ms_last`, `gateway_udp_reorder_drop_total`, `gateway_udp_duplicate_drop_total`를 통해 sequence gap, duplicate, reorder, jitter 신호를 함께 확인한다.
  - `tests/server/test_fps_runtime.cpp`
  - `tests/core/test_direct_egress_route.cpp`
  - `tests/core/test_rudp_engine.cpp`

## Phase 2 수락 경계

- 현재 Phase 2 수락 증거는 아래와 같다.
  - `CorePublicApiRealtimeCapabilitySmoke`를 통한 공개 소비자 realtime capability 증명
  - `CoreInstalledPackageConsumer`를 통한 installed-consumer runtime 증명
  - `tests/python/verify_fps_rudp_transport_matrix.py --scenario phase2-acceptance`를 통한 direct UDP/RUDP fallback/restart/impaired-network 증명
- 남은 더 큰 전송 공백은 더 이상 Phase 2 기반 증명이 아니다.
  이는 더 풍부한 정량/네트워크 shaping 증거, 예를 들어 더 넓은 OS-level `netem` rehearsal 같은 후속 release-evidence 작업에 속한다.
- 현재 OS-level `netem` 경로는 `python tests/python/verify_fps_netem_rehearsal.py --scenario fps-pair`를 통한 수동 ops 전용 rehearsal이며,
  받아들인 기준선이나 `ci-hardening`에는 포함하지 않는다.
- Phase 5 정량 기준선과 권장 실행기/로그 경로/잠정 전송 예산은 `docs/ops/quantified-release-evidence.md`에 기록한다.
- 첫 FPS direct-path soak 보고서:
  - `build/loadgen/mixed_direct_udp_fps_soak.20260318-010307Z.host.json`
  - `build/loadgen/mixed_direct_rudp_fps_soak.20260318-010307Z.host.json`

## 명시적 비목표

- 무기/발사/전투 규칙 없음
- lag-compensated hit validation 없음
- 현재 기반보다 더 넓은 snapshot/delta 프로토콜 패밀리 없음
- gameplay-state continuity 또는 world-migration 의미 없음
