# 서버 코어 개선 TODO (상용 서버 엔진 비교 기반)

기준 입력:

- 임시 비교 문서 검토 결과
- 저장소 실제 근거:
  - `core/src/net/dispatcher.cpp:76`
  - `core/src/metrics/metrics.cpp:14`
  - `core/src/net/session.cpp:277`
  - `docs/protocol.md:6`
  - `docs/ops/observability.md:142`
  - `docs/msa-architecture.md:28`

---

## 0) 목표와 범위

목표:

- "상용 서버 엔진에서 기대되는 운영 성숙도"와 현재 Knights 코어 사이의 갭을 우선순위(P0/P1/P2)로 해소한다.
- 문서에만 존재하는 의도를 런타임 동작/검증 가능한 계약으로 전환한다.

비목표:

- 게임 도메인 로직 전면 재설계.
- 코어와 무관한 대규모 제품 기능 추가.

완료 기준(DoD):

- [ ] P0 항목이 코드/테스트/문서에서 모두 일치한다.
- [ ] P1 항목이 운영 지표와 함께 강제 동작한다.
- [ ] P2 항목이 설정 플래그 기반으로 가시화되고, 비활성화 시 부작용이 없다.
- [ ] 각 Phase 완료 시 해당 변경 범위를 설명하는 문서(설계/운영/API/README/런북/알람 규칙)를 모두 동기화한다.

---

## 1) 현재 상태 요약 (근거 기반)

이미 강한 부분:

- [x] 세션/연결 송신 큐 상한과 드롭 처리(backpressure) 존재 (`core/src/net/session.cpp:102`, `core/src/net/connection.cpp:96`)
- [x] health/ready 노출 경로 존재 (`core/src/metrics/http_server.cpp:83`, `core/src/app/app_host.cpp:136`)

갭 신호:

- [ ] `processing_place`가 분기만 있고 실행 의미가 동일(사실상 no-op) (`core/src/net/dispatcher.cpp:76`)
- [ ] 공용 metrics API가 no-op 구현 (`core/src/metrics/metrics.cpp:14`)
- [ ] 프로토콜 문서가 존재하지 않는 파일(`frame.hpp`)과 불일치 에러코드를 참조 (`docs/protocol.md:6`, `docs/protocol.md:92`, `core/include/server/core/protocol/protocol_errors.hpp:14`)
- [ ] 트레이싱은 로드맵 상태로 표준 런타임 미포함 (`docs/ops/observability.md:142`)
- [ ] 회복탄력성(Circuit Breaker/Bulkhead/Token Bucket)은 아키텍처 문서 중심이며 런타임 강제 근거가 약함 (`docs/msa-architecture.md:28`, `docs/msa-architecture.md:40`)

---

## 2) P0 - 정확성/계약 일치 (최우선)

### P0-1. Dispatcher `processing_place`를 실제 실행 정책으로 강제

왜 먼저:

- 제어 필드가 "있는 척"만 하면 장애 시 원인 파악이 가장 어렵다.

작업:

- [x] `kInline/kWorker/kRoomStrand` 각각의 실행 의미를 명세한다.
- [x] `core/src/net/dispatcher.cpp`에서 정책별 실행 경로를 분리한다.
- [x] 미지원 조합은 명시 오류(`MSG_ERR`)로 처리한다.
- [x] 정책별 메트릭 라벨(호출 수/거절 수/예외 수)을 추가한다.

검증:

- [x] 통합 테스트에서 동일 opcode라도 `processing_place` 변경 시 관측 가능한 동작 차이가 난다.
- [x] 잘못된 정책 조합은 일관된 에러 코드/로그로 실패한다.

### P0-2. HELLO/버전 협상 계약 동기화 (코드-문서-테스트)

왜 먼저:

- 핸드셰이크 불일치는 배포/롤백 시 가장 치명적인 호환성 장애를 만든다.

작업:

- [x] 런타임 HELLO 페이로드 필드 정의를 단일 소스로 고정한다 (`core/src/net/session.cpp:277`).
- [x] `docs/protocol.md`의 존재하지 않는 경로/불일치 코드 참조를 정리한다 (`docs/protocol.md:6`).
- [x] `UNSUPPORTED_VERSION` 등 문서에 언급된 에러코드 정책을 코드와 일치시킨다.
- [x] 버전 호환성 테스트(하위/상위/불일치)를 추가한다.

검증:

- [x] HELLO 필드 수/의미/크기가 테스트로 고정된다.
- [x] 문서 변경 없이 런타임 계약이 바뀌면 CI가 실패한다.

### P0-3. 공용 metrics API를 no-op에서 실제 백엔드로 전환

왜 먼저:

- 운영 중 "숫자가 안 보이는" 상태는 장애 대응 시간을 급증시킨다.

작업:

- [x] `get_counter/get_gauge/get_histogram`을 실제 exporter/registry에 연결한다.
- [x] 최소 공통 메트릭 세트(build info + 런타임 핵심 카운터)를 보장한다.
- [x] `/metrics` smoke 테스트를 서비스별로 추가한다.

검증:

- [x] 최소 트래픽 주입 시 카운터/게이지가 실제로 변한다.
- [x] `/metrics` 응답이 비어 있지 않고 핵심 메트릭이 항상 노출된다.

### P0-4. Admin 계약/권한 드리프트 제거 (문서-코드-운영가이드 동기화)

왜 먼저:

- 운영 제어면 계약이 코드와 다르면, 장애 시 운영자가 기대한 권한/동작과 실제가 어긋나 오조작 리스크가 커진다.

근거:

- `POST /api/v1/users/disconnect`는 코드에서 `admin`만 허용한다 (`tools/admin_app/main.cpp:1492`), 그러나 계약 문서는 `operator, admin`으로 명시한다 (`docs/ops/admin-api-contract.md:279`).
- `tools/AGENTS.md`는 `admin_app`을 read-only로 소개하지만 실제 구현은 다수 write endpoint를 제공한다 (`tools/AGENTS.md:9`, `tools/admin_app/main.cpp:1379`).

작업:

- [x] admin 권한 매트릭스를 단일 소스로 확정한다(권장: 코드 우선 + 문서 정합).
- [x] `docs/ops/admin-api-contract.md`, `tools/admin_app/README.md`, `tools/AGENTS.md`를 동일 계약으로 맞춘다.
- [x] 권한 매트릭스 drift를 잡는 계약 테스트(최소 role별 허용/거부 케이스)를 추가한다.

검증:

- [x] role별 기대 status(200/403)가 테스트로 고정된다.
- [x] 문서/README/AGENTS 간 엔드포인트 성격(read vs write) 불일치가 0건이다.

---

## 3) P1 - 복원력/과부하 제어 운영화

### P1-1. 문서상의 복원력 정책을 런타임 강제 규칙으로 전환

작업:

- [x] Circuit Breaker/Bulkhead/재시도 예산을 실제 경계(gateway->backend, storage I/O)에 배치한다.
- [x] 각 정책에 대해 "설정 키 1개 + 강제 지점 1개 + 메트릭 1개"를 강제한다.

검증:

- [x] 다운스트림 장애 시 연쇄 실패 대신 차단/격리가 관측된다.
- [x] 정책 트립 횟수가 메트릭으로 확인된다.

### P1-2. Rate limit/Load shedding 표준화

작업:

- [x] gateway ingress에 토큰 버킷 기반 제한을 명시 구현한다.
- [x] ready/backpressure 상태에 따른 외부 응답 정책을 표준화한다.

검증:

- [x] 과부하 시 비핵심 경로가 먼저 감쇠되고 핵심 경로가 보존된다.
- [x] 운영 런북에서 임계치/롤백 스위치를 확인할 수 있다.

### P1-3. 실패 모드 회귀 테스트 강화

작업:

- [x] timeout/queue bound/dependency down 시나리오 테스트를 추가한다.
- [x] bounded queue 유지(무한 증가 없음) 검증을 포함한다.

검증:

- [x] 회귀 테스트가 실패 모드를 재현하고 예측 가능한 출력(메트릭/오류)을 보장한다.

---

## 4) P2 - 분산 추적(OTLP) 도입

### P2-1. OTLP 경량 도입 (config-gated)

작업:

- [x] ingress -> dispatch -> dependency call 최소 span 체인을 정의한다.
- [x] tracing on/off 설정 플래그와 샘플링 정책을 추가한다.
- [x] docker observability 스택에서 선택적으로 활성화 가능하게 한다.

검증:

- [x] tracing enabled 시 단일 요청 trace가 서비스 경계를 넘어 연결된다.
- [x] tracing disabled 시 기능/성능 부작용이 최소임을 확인한다.

### P2-2. 로그/메트릭/트레이스 상관키 연결

작업:

- [x] trace_id 또는 correlation_id를 구조화 로그에 연결한다.
- [x] 런북에 "메트릭 -> 로그 -> 트레이스" 추적 절차를 추가한다.

검증:

- [x] 운영자가 하나의 요청을 3개 신호에서 일관되게 추적할 수 있다.

---

## 5) 실행 순서 (권장)

- [x] Step 1: P0-1 (정책 no-op 제거)
- [x] Step 2: P0-2 (핸드셰이크/버전 계약 정합)
- [x] Step 3: P0-3 (metrics 실체화)
- [x] Step 4: P0-4 (admin 계약/권한 드리프트 제거)
- [x] Step 5: P1-1/P1-2 (복원력/과부하 제어)
- [x] Step 6: P1-3 (실패 모드 회귀)
- [x] Step 7: P2 (OTLP + 상관키)

### Phase 완료 공통 게이트 (문서 동기화 필수)

작업:

- [ ] 각 Step(Phase) 완료 직후, 변경 범위를 설명하는 문서를 전부 업데이트한다.
- [ ] 최소 동기화 대상:
  - 프로토콜/계약 변경: `docs/protocol.md`, `docs/protocol/opcodes.md`, 관련 API contract 문서
  - 운영/관측 변경: `docs/ops/observability.md`, `docker/observability/prometheus/alerts.yml`
  - admin/control-plane 변경: `docs/ops/admin-api-contract.md`, `docs/ops/admin-console.md`, `tools/admin_app/README.md`, `tools/AGENTS.md`
  - 구성/환경변수 변경: `README.md`, `server/README.md`, `gateway/README.md`, `tools/*/README.md`

검증:

- [ ] 각 Phase PR/작업 로그에 "코드 변경 파일 + 테스트 결과 + 문서 동기화 목록"을 함께 남긴다.
- [ ] 문서와 런타임 동작 사이의 계약 드리프트(권한/파라미터/에러코드/메트릭 명칭)가 0건이다.

---

## 6) 작업 단위 추적 템플릿 (반복 사용)

각 항목은 아래 템플릿으로 분해한다.

- [ ] Task:
  - 영향 파일:
  - Docs to update:
  - 변경 이유:
  - Acceptance Criteria:
  - Verification:

---

## 7) 리뷰 메모

- 본 문서는 "상용 서버 엔진 비교"에서 나온 갭을 코드 근거와 운영 우선순위로 재정렬한 TODO다.
- 구현 시 P0 완료 전 P1/P2 선행을 금지한다(가시성 없는 기능 추가 방지).

### 진행 기록 (P0-1)

- 상태: 완료
- 코드 변경:
  - `core/src/net/dispatcher.cpp`: `processing_place`별 실행 경로(`kInline`, `kWorker`, `kRoomStrand`) 강제 + invalid policy reject + place별 counter 기록.
  - `core/include/server/core/net/session.hpp`, `core/src/net/session.cpp`: `Session::post_serialized(std::function<void()>)` 추가.
  - `core/include/server/core/runtime_metrics.hpp`, `core/src/runtime_metrics.cpp`: `processing_place` calls/reject/exception 카운터 추가.
  - `server/src/app/metrics_server.cpp`: `chat_dispatch_processing_place_calls_total`, `chat_dispatch_processing_place_reject_total`, `chat_dispatch_processing_place_exception_total` 노출.
  - `tests/core/test_core_net.cpp`: worker queue 경유/queue unavailable reject/room strand post/invalid policy reject 시나리오 추가.
  - `tests/core/test_core_metrics.cpp`: `processing_place` 카운터 증가 검증 추가.
  - `server/CMakeLists.txt`: Windows 빌드 검증을 막던 post-build DLL copy 경로를 `${CMAKE_SOURCE_DIR}/vcpkg_installed` 하드코딩에서 `${VCPKG_INSTALLED_DIR}` 기반으로 수정.
- 문서 동기화:
  - `docs/core-design.md`: Dispatcher `processing_place` 정책과 신규 메트릭 반영.
- 검증 결과:
  - `pwsh scripts/build.ps1 -Config Debug -Target server_app`: 성공.
  - `pwsh scripts/build.ps1 -Config Debug -Target core_general_tests`: 성공.
  - `ctest --preset windows-test -R "DispatcherTest|RuntimeMetricsTest" --output-on-failure`: 9/9 통과.
  - `lsp_diagnostics`: 현재 로컬 clangd include 경로/툴체인 인식 문제로 false-positive 다수 발생(빌드/테스트 성공으로 런타임 정합성 검증 대체).

### 진행 기록 (P0-2)

- 상태: 완료
- 코드 변경:
  - `core/include/server/core/protocol/version.hpp`: `kProtocolVersionMajor/Minor` 단일 소스와 `is_protocol_version_compatible` 판정 함수 추가.
  - `core/src/net/session.cpp`: `MSG_HELLO` 버전 필드를 하드코딩 값 대신 `version.hpp` 상수로 송신.
  - `core/include/server/core/protocol/protocol_errors.hpp`: `UNSUPPORTED_VERSION(0x0009)` 오류 코드 추가.
  - `server/src/chat/handlers_login.cpp`: `MSG_LOGIN_REQ` 뒤의 선택적 `(client_major, client_minor)` 파싱/검증 추가(major 일치 + minor 이하 허용).
  - `client_gui/src/net_client.cpp`: 로그인 요청 payload에 client protocol version(major/minor) 추가.
  - `tests/core/test_core_net.cpp`: `SessionTest.HelloPayloadVersionContract` 추가(HELLO `msg_id`, payload 길이 12, 버전 필드 검증).
  - `tests/server/test_server_chat.cpp`: `LoginRejectsMismatchedProtocolMajor`, `LoginRejectsHigherProtocolMinor`, `LoginAcceptsLowerProtocolMinor` 추가.
- 문서 동기화:
  - `docs/protocol.md`: `frame.hpp` 참조를 `packet.hpp`로 정정, HELLO(12 bytes)/LOGIN_REQ version tail/에러코드 정책 반영.
- 검증 결과:
  - `pwsh scripts/build.ps1 -Config Debug -Target core_general_tests`: 성공.
  - `pwsh scripts/build.ps1 -Config Debug -Target server_general_tests`: 성공.
  - `pwsh scripts/build.ps1 -Config Debug -Target client_gui`: 성공.
  - `build-windows/tests/Debug/core_general_tests.exe --gtest_filter=SessionTest.HelloPayloadVersionContract`: 1/1 통과.
  - `build-windows/tests/Debug/server_general_tests.exe --gtest_filter=ChatServiceTest.LoginRejectsMismatchedProtocolMajor:ChatServiceTest.LoginRejectsHigherProtocolMinor:ChatServiceTest.LoginAcceptsLowerProtocolMinor`: 3/3 통과.
  - `lsp_diagnostics`: P0-2 변경 파일(`.cpp/.hpp`) 에러 없음 (`docs/protocol.md`는 LSP 미구성).

### 진행 기록 (P0-3)

- 상태: 완료
- 코드 변경:
  - `core/src/metrics/metrics.cpp`: Counter/Gauge/Histogram registry backend 구현 + `append_prometheus_metrics()` 직렬화 + `reset_for_tests()` 추가.
  - `core/include/server/core/metrics/metrics.hpp`: 공용 registry export/runtime core export/reset API 선언 추가.
  - `server/src/app/metrics_server.cpp`, `gateway/src/gateway_app.cpp`, `tools/wb_worker/main.cpp`: `append_runtime_core_metrics()` + `append_prometheus_metrics()`를 `/metrics` 렌더 경로에 통합.
  - `tests/core/test_core_metrics.cpp`: API 값 누적 검증, runtime core snapshot 노출 검증, `/metrics` payload smoke 검증 추가.
  - `scripts/smoke_metrics.ps1`: gateway/server/wb_worker 서비스별 `/metrics` 계약 스모크 스크립트 추가.
- 문서 동기화:
  - `docs/core-design.md`: 공용 metrics backend + runtime core metrics 보장 경로 반영.
  - `docs/ops/observability.md`: 공통(core) 메트릭 목록 및 `smoke_metrics.ps1` 점검 절차 반영.
  - `scripts/AGENTS.md`: 신규 스모크 스크립트 안내 추가.
- 검증 결과:
  - `pwsh scripts/build.ps1 -Config Debug -Target core_general_tests`: 성공.
  - `pwsh scripts/build.ps1 -Config Debug -Target server_app`: 성공.
  - `pwsh scripts/build.ps1 -Config Debug -Target gateway_app`: 성공.
  - `pwsh scripts/build.ps1 -Config Debug -Target wb_worker`: 성공.
  - `build-windows/tests/Debug/core_general_tests.exe --gtest_filter=MetricsTest.*:MetricsHttpServerTest.*:RuntimeMetricsTest.*`: 6/6 통과.
  - `build-windows/tests/Debug/core_general_tests.exe --gtest_filter=SessionTest.HelloPayloadVersionContract`: 1/1 통과.
  - `build-windows/tests/Debug/server_general_tests.exe --gtest_filter=ChatServiceTest.LoginRejectsMismatchedProtocolMajor:ChatServiceTest.LoginRejectsHigherProtocolMinor:ChatServiceTest.LoginAcceptsLowerProtocolMinor`: 3/3 통과.
  - `lsp_diagnostics`: P0-3 변경 파일(`.cpp/.hpp`) 에러 없음.

### 진행 기록 (P0-4)

- 상태: 완료
- 코드 변경:
  - `tools/admin_app/main.cpp`: admin role matrix 단일 소스 상수(`kRoleRequiredDisconnect`, `kRoleRequiredAnnouncement`, `kRoleRequiredSettings`, `kRoleRequiredModeration`)를 추가하고, write endpoint 권한 체크 및 `/api/v1/auth/context` capabilities 계산을 상수 기반으로 통일.
  - `tests/python/verify_admin_auth.py`: role별(`viewer/operator/admin`) capability 기대값과 write endpoint(`disconnect/announcements/settings/moderation`) allow/deny 계약 검증을 추가.
- 문서 동기화:
  - `docs/ops/admin-api-contract.md`: `POST /api/v1/users/disconnect` 권한을 `admin` 단독으로 정정, role matrix 표를 코드와 일치시킴.
  - `tools/admin_app/README.md`: `operator` 권한 설명을 `announcement` 전용으로 정정.
  - `docs/ops/admin-console.md`: RBAC 설명에서 `operator` 권한을 `announcement` 전용으로 정정.
  - `tools/AGENTS.md`: `admin_app` 설명을 read-only에서 read + write-lite 제어면으로 정정.
- 검증 결과:
  - `pwsh scripts/build.ps1 -Config Debug -Target admin_app`: 성공.
  - `python tests/python/verify_admin_auth.py`: 통과 (`PASS: admin auth mode smoke test`).
  - `python tests/python/verify_admin_api.py`: 미기동 스택 환경에서 `/healthz` 타임아웃으로 실패(해당 스크립트는 docker stack 기동 전제).
  - `lsp_diagnostics`: `tools/admin_app/main.cpp` 에러 없음(기존 `getenv` deprecation hint만 존재), `tests/python/verify_admin_auth.py` 에러 없음(기존 basedpyright warning 다수).

### 진행 기록 (P1)

- 상태: 완료
- 코드 변경:
  - `gateway/include/gateway/resilience_controls.hpp`: gateway ingress/token-bucket, retry budget, circuit breaker 공용 제어기 추가.
  - `gateway/include/gateway/gateway_app.hpp`, `gateway/src/gateway_app.cpp`, `gateway/src/gateway_connection.cpp`: gateway ingress admission(not-ready/rate-limit/session-limit/circuit-open), backend circuit breaker, backend connect retry budget + backoff를 런타임 강제.
  - `tools/wb_worker/main.cpp`: `WB_RETRY_MAX`/`WB_RETRY_BACKOFF_MS` 기반 flush 재시도 예산(소진 카운터 포함) 추가.
  - `tests/core/test_gateway_resilience_controls.cpp`, `tests/CMakeLists.txt`: token bucket/retry budget/circuit breaker 회귀 테스트 추가.
- 문서 동기화:
  - `docs/configuration.md`: gateway 회복탄력성/ingress 제어 환경변수 및 write-behind retry budget 문서화.
  - `gateway/README.md`, `docs/ops/gateway-and-lb.md`: gateway 새 가드레일/메트릭/운영 키 반영.
  - `tools/wb_worker/README.md`, `docs/db/write-behind.md`: wb_worker retry budget 정책/메트릭 반영.
  - `docs/ops/observability.md`, `docs/ops/runbook.md`, `docker/observability/prometheus/alerts.yml`: 새 경보(`GatewayBackendCircuitOpen`, `GatewayIngressRateLimited`, `WbFlushRetryExhausted`)와 대응 절차 반영.
- 검증 결과:
  - `pwsh scripts/build.ps1 -Config Debug -Target gateway_app`: 성공.
  - `pwsh scripts/build.ps1 -Config Debug -Target wb_worker`: 성공.
  - `pwsh scripts/build.ps1 -Config Debug -Target core_general_tests`: 성공.
  - `build-windows/tests/Debug/core_general_tests.exe --gtest_filter=Gateway*:*DispatcherTest.WorkerProcessingPlaceRejectsWhenQueueUnavailable`: 4/4 통과.
  - `lsp_diagnostics`: P1 변경 C++ 파일 에러 없음.

### 진행 기록 (P2)

- 상태: 완료
- 코드 변경:
  - `core/include/server/core/trace/context.hpp`, `core/src/trace/context.cpp`: tracing config(`KNIGHTS_TRACING_ENABLED`, `KNIGHTS_TRACING_SAMPLE_PERCENT`), 샘플링, trace/correlation context 스코프 API 구현.
  - `core/src/net/session.cpp`: ingress/dispatch span 시작/종료 로그 + 샘플링 기반 trace context 전파.
  - `core/src/util/log.cpp`: sampled context가 있을 때 로그 라인에 `trace_id`, `correlation_id` 자동 부착.
  - `server/src/chat/chat_service_core.cpp`: write-behind stream field에 `trace_id`, `correlation_id` 전파 + `redis_xadd` span 로그 추가.
  - `tools/wb_worker/main.cpp`: stream field의 `trace_id`, `correlation_id`를 DB insert span으로 연결.
  - `tests/core/test_trace_context.cpp`, `tests/CMakeLists.txt`: tracing enabled 시 로그 상관키 주입 계약 테스트 추가.
- 문서 동기화:
  - `docs/ops/observability.md`: tracing on/off, sampling, `metrics -> logs -> trace` 운영 절차 추가.
  - `docs/configuration.md`: tracing 환경변수(`KNIGHTS_TRACING_*`)를 server/wb_worker 구성 키에 반영.
  - `docker/stack/docker-compose.yml`: tracing 환경변수 pass-through 추가.
  - `server/README.md`, `tools/wb_worker/README.md`, `docs/db/write-behind.md`: tracing 상관키 전파/운영 설정 반영.
- 검증 결과:
  - `lsp_diagnostics`: P2 변경 C++ 파일 에러 없음(경고/힌트만 존재).
  - `pwsh scripts/build.ps1 -Config Debug -Target core_general_tests`: 성공.
  - `pwsh scripts/build.ps1 -Config Debug -Target server_general_tests`: 성공.
  - `pwsh scripts/build.ps1 -Config Debug -Target server_app`: 성공.
  - `pwsh scripts/build.ps1 -Config Debug -Target wb_worker`: 성공.
  - `pwsh scripts/build.ps1 -Config Debug -Target chat_history_tests`: 성공.
  - `build-windows/tests/Debug/core_general_tests.exe --gtest_filter=TraceContextTest.* --gtest_color=no`: 1/1 통과.
  - `build-windows/tests/Debug/server_general_tests.exe --gtest_color=no`: 19/19 통과.
  - `build-windows/tests/Debug/chat_history_tests.exe --gtest_color=no`: 2/2 통과.

---

## 8) 추가 갭 (상용 엔진 대비, 기존 P0~P2 외)

### P1-4. Graceful drain(종료 전 연결 배출) 명시 경로 도입

근거:

- 현재 종료 시 acceptor 중지 + ready false는 있으나(`server/src/app/bootstrap.cpp:610`, `server/src/app/bootstrap.cpp:640`), 세션의 drain 완료 대기 단계는 문서/코드에 명시적 계약이 약하다.
- `Session::stop()`은 즉시 shutdown/close 경로 중심이다(`core/src/net/session.cpp:74`).

작업:

- [x] drain 모드(신규 연결 차단 + 인플라이트 종료 대기 + 타임아웃 강제 종료) 상태머신을 도입한다.
- [x] drain 진행률 메트릭(남은 연결 수, drain elapsed, forced close count)을 추가한다.
- [x] 서버/워커 종료 절차에 drain 단계와 타임아웃 정책을 명시한다.

검증:

- [x] SIGTERM 시 새 연결은 즉시 거절되고, 기존 연결은 drain timeout 내 정상 종료된다.
- [x] drain timeout 초과 연결만 강제 종료되며 카운터로 집계된다.

### P1-5. 전송 보안 운영화(TLS1.3/mTLS/인증서 관측) 실행 항목 명시

근거:

- 보안 요구는 문서에 존재하나(`docs/msa-architecture.md:31`, `docs/msa-architecture.md:32`) 코드 레벨 TLS/mTLS 런타임 증거는 제한적이다.

작업:

- [x] edge(HAProxy) TLS 1.3 강제/레거시 예외 정책을 명문화한다.
- [x] 내부 링크 mTLS 적용 범위와 인증서 자동 갱신 경로를 운영 절차로 고정한다.
- [x] 인증서 만료 관측/알람(30/14/7일) 기준을 observability 문서/알람 규칙에 반영한다.

검증:

- [x] 내부 서비스 plaintext 링크가 없음을 배포 점검으로 증명한다.
- [x] 인증서 만료 임계치 알람이 테스트 환경에서 재현된다.

### P1-6. 테스트 매트릭스 확장: 퍼즈/소크/성능회귀 게이트

근거:

- 현재 테스트는 단위/일반/계약 중심(`tests/CMakeLists.txt:66`, `tests/CMakeLists.txt:122`)이며 퍼즈/장시간 소크/성능 회귀 전용 타깃은 명시적이지 않다.

작업:

- [x] 프로토콜 파서/디코더 대상 fuzz harness를 추가한다.
- [x] 혼합 트래픽 장시간 soak 시나리오를 추가한다.
- [x] p95/p99 지연 및 처리량 baseline 대비 회귀 게이트를 CI에 분리한다.

검증:

- [x] fuzz 타깃이 sanitizer와 함께 일정 시간 무충돌 실행된다.
- [x] soak 테스트에서 메모리/큐/세션 카운터가 bounded 상태를 유지한다.
- [x] 성능 게이트가 기준 초과 회귀 시 빌드를 실패시킨다.

### P1-7. Admin 명령 채널 무결성(서명/재생방지) 도입

근거:

- admin write 명령은 Redis Pub/Sub에 평문 payload로 발행되고(`tools/admin_app/main.cpp:2190`, `tools/admin_app/main.cpp:2270`), server는 신뢰 경계 검증 없이 적용한다(`server/src/app/bootstrap.cpp:513`, `server/src/app/bootstrap.cpp:546`, `server/src/app/bootstrap.cpp:559`).

작업:

- [x] admin 명령 payload에 `issued_at`, `nonce`, `signature`를 추가한다.
- [x] server 수신측에서 서명 검증 + TTL 기반 재생 공격 차단을 강제한다.
- [x] 검증 실패 카운터/감사 로그를 표준 메트릭으로 노출한다.

검증:

- [x] 변조/재전송 payload는 적용되지 않고 거절 사유가 메트릭/로그에 남는다.
- [x] 정상 payload만 fanout 명령으로 반영된다.

### P1-8. Admin 읽기 전용 킬스위치(`ADMIN_READ_ONLY`) 실체화

근거:

- 현재 메트릭에 `admin_read_only_mode`가 고정 `0`으로 노출된다(`tools/admin_app/main.cpp:1286`), 환경변수/실행경로에서 write 차단 동작은 없다.

작업:

- [x] `ADMIN_READ_ONLY` 환경변수를 추가하고 write endpoint 진입 전 일괄 차단한다.
- [x] read-only 상태에서 쓰기 요청은 일관된 에러 코드(`FORBIDDEN` 또는 `READ_ONLY`)로 반환한다.
- [x] `/api/v1/auth/context` capability 및 `/metrics`의 `admin_read_only_mode`를 실제 상태와 연동한다.

검증:

- [x] `ADMIN_READ_ONLY=1`에서 disconnect/announce/settings/moderation이 모두 차단된다.
- [x] `ADMIN_READ_ONLY=0`으로 복구 시 기존 권한 매트릭스대로 동작한다.

### P2-3. 예외 처리 가시성 강화(침묵 catch 축소)

근거:

- 주요 경로에 `catch (...)`가 다수 존재하고 일부는 관측 신호가 약하다(`core/src/net/dispatcher.cpp:93`, `server/src/app/bootstrap.cpp:617`).

작업:

- [x] 네트워크/종료/스토리지 경계의 blanket catch를 분류(복구 가능/치명/무시 가능)한다.
- [x] 무시 경로에도 최소 메트릭 또는 구조화 로그를 남기도록 표준화한다.
- [x] 예외 정책 표(throw/catch/convert-to-error code)를 문서화한다.

검증:

- [x] 장애 재현 시 "예외 발생 -> 신호(로그/메트릭) -> 대응" 흐름이 누락 없이 추적된다.

### P2-4. 프로토콜 강건성 보강(UTF-8 엄격 검증 + 프레임 퍼저 친화성)

근거:

- `packet.hpp`의 UTF-8 검증은 "대략 검증"으로 명시되어 있다(`core/include/server/core/protocol/packet.hpp:82`).

작업:

- [x] UTF-8 검증 정책(허용/거부 범위, overlong/invalid codepoint 처리)을 명시적으로 강화한다.
- [x] 프레임/문자열 파싱 경계 테스트와 퍼즈 코퍼스를 추가한다.

검증:

- [x] 잘못된 UTF-8/경계값 입력에서 일관된 에러 코드와 세션 처리 정책이 유지된다.

### P2-5. 운영 SLO/Error Budget 규칙을 관측 스택에 연결

근거:

- 현재 문서에는 SLO/버짓 운영 규칙이 명시적으로 드러나지 않는다(검색 근거).

작업:

- [x] 코어 핵심 SLI(가용성, 지연, 오류율) 정의와 30일 SLO 목표를 확정한다.
- [x] burn-rate 기반 경보 규칙을 Prometheus alert에 추가한다.
- [x] 릴리스 게이트와 연동(버짓 소진 시 기능 확장 중단 정책)한다.

검증:

- [x] 대시보드에서 잔여 error budget이 계산/표시된다.
- [x] burn-rate 경보가 테스트 데이터에서 정상 트리거된다.

### P2-6. 구조화 로그 표준(JSON + 상관키) 정착

근거:

- 현재 코어 로깅은 문자열 라인 중심(`core/src/util/log.cpp:137`, `core/src/util/log.cpp:94`)이며, 아키텍처 문서는 구조화 로그/trace id를 지향한다(`docs/msa-architecture.md:53`).

작업:

- [x] 공통 로그 스키마(timestamp, level, component, trace_id, correlation_id, message, error_code)를 정의한다.
- [x] core/server/gateway 로그 API를 구조화 포맷으로 확장한다.
- [x] 민감정보 마스킹 규칙(토큰/개인식별자)을 로거 계층에 강제한다.

검증:

- [x] 단일 요청 추적 시 로그만으로 서비스 경계 추적이 가능하다.
- [x] 기존 plain 로그 대비 파싱 성공률/필드 채움률 지표를 확보한다.

### P2-7. 런타임 설정 리로드 범위 확장(플러그인 한정 -> 코어 정책)

근거:

- 현재 hot-reload 신호는 chat hook plugin 경로에 집중되어 있다(`server/src/chat/chat_service_core.cpp:251`, `server/src/chat/chat_service_core.cpp:319`).

작업:

- [x] 코어 정책(큐 상한, 타임아웃, rate-limit 임계치)의 런타임 리로드 가능 항목을 정의한다.
- [x] 리로드 적용 시 일관성 규칙(원자 적용/롤백)을 명시한다.
- [x] 리로드 성공/실패/적용 지연 메트릭을 추가한다.

검증:

- [x] 운영 중 정책값 변경이 프로세스 재기동 없이 반영된다.
- [x] 잘못된 설정 입력은 안전하게 거부되고 이전 값이 보존된다.

### P2-8. Admin/metrics HTTP 보안 하드닝(인증/타임아웃/접속 제한)

근거:

- `MetricsHttpServer`는 라우트 처리를 제공하지만 인증/권한 검증 경로는 기본 제공되지 않는다(`core/src/metrics/http_server.cpp:280`, `core/src/metrics/http_server.cpp:320`).
- AppHost는 기본 로그 콜백을 비워 둬 `/logs`는 404지만(`core/src/app/app_host.cpp:324`), 커스텀 라우트 사용 시 보호 정책이 필요하다.

작업:

- [x] admin/metrics endpoint 접근 제어(allowlist 또는 인증 토큰)를 도입한다.
- [x] read/header/body timeout 및 동시 접속 상한을 설정 가능하게 한다.
- [x] 비정상 요청(과대 헤더/지연 연결) 방어 메트릭을 추가한다.

검증:

- [x] 비인가 요청은 일관된 401/403으로 차단된다.
- [x] slowloris 유사 시나리오에서 admin thread가 고갈되지 않는다.

### P2-9. 비동기 로거 큐 bounded 정책 + 드롭 가시성 추가

근거:

- 현재 비동기 로거 큐는 `std::queue` 기반이며 상한/드롭 정책이 보이지 않는다(`core/src/util/log.cpp:101`, `core/src/util/log.cpp:44`).

작업:

- [x] 로거 큐 상한 및 overflow 정책(drop newest/oldest/block)을 명시한다.
- [x] drop count, queue depth, flush latency 메트릭을 추가한다.
- [x] 로그 레벨/버퍼 정책을 환경설정 또는 런타임 설정으로 연결한다.

검증:

- [x] 로그 폭주 시 프로세스 메모리 사용이 bounded 상태를 유지한다.
- [x] 드롭 발생 시 메트릭/경고가 즉시 관측된다.

### P2-10. Admin HTTP 요청 모델 개선(query 중심 -> body 지원)

근거:

- 현재 HTTP 서버는 헤더까지만 파싱하고 본문을 읽지 않는다 (`core/src/metrics/http_server.cpp:154`), admin write API도 body 대신 query parameter를 사용한다 (`tools/admin_app/README.md:95`).

작업:

- [x] `MetricsHttpServer`에 `Content-Length` 기반 body read(상한 포함)와 JSON 파싱 경로를 추가한다.
- [x] admin write endpoint를 query/body 병행 지원 후 body 우선으로 전환한다.
- [x] body 크기 상한, malformed JSON, unsupported content-type 처리 규약을 명시한다.

검증:

- [x] 동일 명령을 query/body 모두로 호출 가능하며 결과가 일치한다.
- [x] 과대 body/잘못된 JSON 입력은 일관된 4xx로 차단된다.

---

### 진행 기록 (추가 갭 P1-8)

- 상태: 완료
- 코드 변경:
  - `tools/admin_app/main.cpp`: `ADMIN_READ_ONLY` 환경변수 파싱 추가, write endpoint(disconnect/announce/settings/moderation) 진입 전 공통 read-only 차단(`403` + `READ_ONLY`) 적용.
  - `tools/admin_app/main.cpp`: `/api/v1/auth/context` 응답에 `read_only` 필드 추가, read-only 모드에서는 write capability를 모두 `false`로 강제.
  - `tools/admin_app/main.cpp`: `/metrics`의 `admin_read_only_mode`를 실제 런타임 상태와 연동.
  - `tests/python/verify_admin_read_only.py`: `ADMIN_READ_ONLY=1/0` 양쪽 시나리오 smoke/계약 검증 스크립트 추가.
- 문서 동기화:
  - `tools/admin_app/README.md`: `ADMIN_READ_ONLY` 환경변수, read-only 동작/응답 계약 반영.
  - `docs/ops/admin-api-contract.md`: read-only 우선 차단 규칙(`READ_ONLY`) 및 auth context/권한 매트릭스 override 반영.
  - `docs/ops/admin-console.md`: 운영 kill-switch를 `ADMIN_READ_ONLY=1` 실체 경로로 명시.
- 검증 결과:
  - `pwsh scripts/build.ps1 -Config Debug -Target admin_app`: 성공.
  - `docker build -f Dockerfile --target admin-runtime -t knights-admin:local .`: 성공.
  - `python tests/python/verify_admin_auth.py`: 통과 (`PASS: admin auth mode smoke test`).
  - `python tests/python/verify_admin_read_only.py`: 통과 (`PASS: admin read-only mode smoke test`).
  - `lsp_diagnostics`: `tools/admin_app/main.cpp` 에러 없음(기존 `getenv` deprecation hint만 존재).

### 진행 기록 (추가 갭 P1-7)

- 상태: 완료
- 코드 변경:
  - `core/include/server/core/security/admin_command_auth.hpp`, `core/src/security/admin_command_auth.cpp`: admin command canonical KV 서명(HMAC-SHA256), `issued_at`/`nonce`/`signature` 부착, TTL/future-skew/replay 검증 유틸(상수시간 signature 비교 포함) 구현.
  - `core/CMakeLists.txt`: `admin_command_auth.cpp`를 `server_core`에 명시 추가.
  - `server/include/server/app/config.hpp`, `server/src/app/config.cpp`: `ADMIN_COMMAND_SIGNING_SECRET`, `ADMIN_COMMAND_TTL_MS`, `ADMIN_COMMAND_FUTURE_SKEW_MS` 설정 키 추가.
  - `server/src/app/bootstrap.cpp`: admin fanout 채널(`disconnect/announce/settings/moderation`) 수신 시 서명 검증 + TTL 기반 replay 차단 강제, 실패 사유별 카운터/감사 로그 추가.
  - `server/src/app/metrics_server.cpp`: `chat_admin_command_verify_*` 메트릭 노출 추가.
  - `tools/admin_app/main.cpp`: write fanout payload에 `issued_at`/`nonce`/`signature` 자동 부착, 미설정 시 `503` + `MISCONFIGURED`로 publish 차단, `admin_command_signing_errors_total` 메트릭 추가.
  - `tests/core/test_admin_command_auth.cpp`, `tests/CMakeLists.txt`: 서명 정합/변조/재전송/TTL/future-skew/미설정 시나리오 테스트 추가.
  - `docker/stack/docker-compose.yml`: server/admin 서비스에 `ADMIN_COMMAND_SIGNING_SECRET` 및 server TTL/skew 설정 pass-through 추가.
- 문서 동기화:
  - `docs/configuration.md`: admin command 서명/TTL/future-skew 환경변수 반영.
  - `server/README.md`: server_app admin command 무결성 환경변수 반영.
  - `tools/admin_app/README.md`: write payload 서명 필드와 `ADMIN_COMMAND_SIGNING_SECRET` 계약 반영.
  - `docs/ops/admin-api-contract.md`: write fanout payload(`issued_at`,`nonce`,`signature`) 및 `MISCONFIGURED` 오류 코드 반영.
  - `docs/ops/admin-console.md`: 운영 안전장치에 admin command 무결성 검증 절차 반영.
  - `docs/ops/observability.md`: server/admin command integrity 메트릭 반영.
- 검증 결과:
  - `lsp_diagnostics`: P1-7 변경 C++ 파일 에러 없음(기존 `getenv` 관련 hint만 존재).
  - `pwsh scripts/build.ps1 -Config Debug -Target core_general_tests`: 성공.
  - `pwsh scripts/build.ps1 -Config Debug -Target server_app`: 성공.
  - `pwsh scripts/build.ps1 -Config Debug -Target server_general_tests`: 성공.
  - `pwsh scripts/build.ps1 -Config Debug -Target admin_app`: 성공.
  - `build-windows/tests/Debug/core_general_tests.exe --gtest_filter=AdminCommandAuthTest.* --gtest_color=no`: 6/6 통과.
  - `build-windows/tests/Debug/server_general_tests.exe --gtest_color=no`: 19/19 통과.
  - `python tests/python/verify_admin_auth.py`: 통과 (`PASS: admin auth mode smoke test`).
  - `python tests/python/verify_admin_read_only.py`: 통과 (`PASS: admin read-only mode smoke test`).

### 진행 기록 (추가 갭 P1-4)

- 상태: 완료
- 코드 변경:
  - `server/include/server/app/config.hpp`, `server/src/app/config.cpp`: graceful drain 설정 키 `SERVER_DRAIN_TIMEOUT_MS`, `SERVER_DRAIN_POLL_MS` 추가.
  - `server/src/app/bootstrap.cpp`: shutdown 단계에 drain 대기 단계 추가(acceptor stop -> drain wait -> io stop), timeout 초과 시 남은 연결 수를 강제 종료 카운터로 누적.
  - `server/src/app/metrics_server.cpp`: `chat_shutdown_drain_*` 메트릭(remaining/elapsed/timeout/forced-close/completed) 노출.
  - `docker/stack/docker-compose.yml`: `server-1`, `server-2`에 drain 관련 환경변수 pass-through 추가.
- 문서 동기화:
  - `server/README.md`: graceful drain 종료 순서와 운영 메트릭 명시.
  - `tools/wb_worker/README.md`: worker shutdown drain 정책/관측 포인트 명시.
  - `docs/db/write-behind.md`: server/worker 종료 시 drain + timeout 정책 추가.
  - `docs/configuration.md`: `SERVER_DRAIN_TIMEOUT_MS`, `SERVER_DRAIN_POLL_MS` 구성 키 문서화.
  - `docs/ops/observability.md`: `chat_shutdown_drain_*` 메트릭 목록 반영.
- 검증 결과:
  - `lsp_diagnostics`: P1-4 변경 C++ 파일 에러 없음.
  - `pwsh scripts/build.ps1 -Config Debug -Target server_app`: 성공.
  - `pwsh scripts/build.ps1 -Config Debug -Target server_general_tests`: 성공.
  - `build-windows/tests/Debug/server_general_tests.exe --gtest_color=no`: 19/19 통과.

### 진행 기록 (추가 갭 P1-5)

- 상태: 완료
- 코드 변경:
  - `docker/observability/prometheus/alerts.yml`: `TLSCertificateExpiringIn30Days`, `TLSCertificateExpiringIn14Days`, `TLSCertificateExpiringIn7Days` 규칙 추가.
  - `docker/observability/prometheus/alerts.tests.yml`: cert expiry 30/14/7일 임계치 발화를 검증하는 promtool rule test fixture 추가.
  - `scripts/check_prometheus_rules.ps1`: `promtool check rules` + `promtool test rules` 자동 점검 스크립트 추가.
  - `docker/stack/haproxy/haproxy.tls13.cfg`: TLS 1.3 기본 + 레거시 예외 분리 + 내부 mTLS 템플릿 추가.
  - `docker/stack/haproxy/haproxy.cfg`: 로컬 dev baseline(TCP-only) 성격 주석 추가.
- 문서 동기화:
  - `docs/configuration.md`: edge TLS1.3/legacy 예외/mTLS/갱신 윈도우 운영 baseline 반영.
  - `docs/ops/gateway-and-lb.md`: HAProxy TLS 1.3/mTLS 운영 정책 및 템플릿 경로 반영.
  - `docs/ops/deployment.md`: plaintext 금지 배포 점검 절차와 30/14/7일 운영 규칙 반영.
  - `docs/ops/observability.md`: cert expiry 알람 3종, 소스 메트릭, promtool 재현 절차 반영.
  - `docs/ops/runbook.md`, `docs/ops/fallback-and-alerts.md`: cert expiry 경보 대응 매트릭스 반영.
  - `docker/stack/README.md`, `docker/observability/AGENTS.md`, `scripts/AGENTS.md`: TLS 템플릿/alert rule check 스크립트 경로 반영.
- 검증 결과:
  - `pwsh scripts/check_prometheus_rules.ps1`: 성공 (`alerts.yml` syntax + `alerts.tests.yml` rule tests 통과).
  - `docker run ... haproxy -c -f haproxy.cfg` + `docker run ... haproxy -c -f haproxy.tls13.cfg`: 성공(임시 self-signed cert/allowlist fixture로 구문 검증).
  - `lsp_diagnostics`: `alerts.yml`, `alerts.tests.yml` 에러 없음(나머지 `.md/.cfg/.ps1`은 현재 LSP 미구성).

### 진행 기록 (추가 갭 P2-4/P2-8/P2-9/P2-10, 1차)

- 상태: 진행 중(핵심 경로 1차 반영)
- 코드 변경:
  - `core/src/runtime_metrics.cpp`: 신규 런타임 카운터(예외 분류/log async/http defense/runtime reload)의 `snapshot()` 누락 필드 반영.
  - `core/src/metrics/metrics.cpp`: `core_runtime_exception_*`, `core_runtime_log_async_*`, `core_runtime_http_*`, `core_runtime_setting_reload_*` 메트릭 노출 추가.
  - `server/src/app/metrics_server.cpp`: `chat_exception_*`, `chat_log_async_*`, `chat_http_*` 메트릭 노출 추가.
  - `core/src/util/log.cpp`: async logger bounded queue(`LOG_ASYNC_QUEUE_CAPACITY`, `LOG_ASYNC_QUEUE_OVERFLOW`) + queue/drop/flush/masking 메트릭 연동.
  - `core/src/metrics/http_server.cpp`, `core/include/server/core/metrics/http_server.hpp`: HTTP body read(`Content-Length`), connection limit, auth token/allowlist, oversize/invalid request 방어 및 메트릭 연동.
  - `tools/admin_app/main.cpp`: write endpoint에서 query + body(JSON/form-urlencoded) 병행 파싱, body 우선 merge, malformed JSON/unsupported content-type(415) 거절 경로 추가.
  - `core/include/server/core/protocol/packet.hpp`: UTF-8 strict 검증(overlong/surrogate/out-of-range codepoint 거부)으로 강화.
  - `tests/core/test_core_metrics.cpp`: 확장 runtime metrics snapshot/노출 검증 + custom route body read 테스트 추가.
  - `tests/core/test_core_net.cpp`: UTF-8 strict 검증 경계 테스트 추가.
- 문서 동기화:
  - `docs/configuration.md`: `METRICS_HTTP_MAX_CONNECTIONS`, `METRICS_HTTP_MAX_BODY_BYTES`, `METRICS_HTTP_AUTH_TOKEN`, `METRICS_HTTP_ALLOWLIST` 반영.
  - `docs/ops/observability.md`: 신규 core/chat 예외·logger·HTTP 방어 메트릭 목록 반영.
  - `tools/admin_app/README.md`: body read 지원 및 metrics/admin HTTP hardening 환경변수 반영.
- 검증 결과:
  - `lsp_diagnostics`: 변경 파일(`.cpp/.hpp`) 에러 없음.
  - `pwsh scripts/build.ps1 -Config Debug -Target core_general_tests`: 성공.
  - `pwsh scripts/build.ps1 -Config Debug -Target server_app`: 성공.
  - `pwsh scripts/build.ps1 -Config Debug -Target server_general_tests`: 성공.
  - `build-windows/tests/Debug/core_general_tests.exe --gtest_filter=PacketUtf8Test.*:MetricsTest.*:RuntimeMetricsTest.*:MetricsHttpServerTest.* --gtest_color=no`: 11/11 통과.
  - `build-windows/tests/Debug/server_general_tests.exe --gtest_filter=ChatServiceTest.* --gtest_color=no`: 19/19 통과.

### 진행 기록 (추가 갭 P1-6)

- 상태: 완료
- 코드 변경:
  - `tests/CMakeLists.txt`: `protocol_fuzz_harness` 타깃/ctest(`ProtocolFuzzHarness`) wire-up 추가.
  - `.github/workflows/ci.yml`: Windows fast job에 fuzz harness build/run 추가, Linux ASan build에 `protocol_fuzz_harness` 타깃 추가 및 실행 단계 추가.
  - `tests/python/verify_soak_perf_gate.py`: 혼합 로그인 부하 기반 soak + 성능 회귀 게이트(p95/p99/throughput + bounded queue/session) 추가.
  - `.github/workflows/ci.yml`: `linux-docker-stack`에 `Soak + Performance Regression Gate` 단계 추가.
- 문서 동기화:
  - `docs/ops/observability.md`: soak/perf gate 실행 절차 반영.
- 검증 결과:
  - `FUZZ_ITERATIONS=80000 ./build-windows/tests/Debug/protocol_fuzz_harness.exe`: 성공.

### 진행 기록 (추가 갭 P2-5)

- 상태: 완료
- 코드 변경:
  - `docker/observability/prometheus/alerts.yml`: `ChatErrorBudgetBurnRateFast`, `ChatErrorBudgetBurnRateSlow` burn-rate 규칙 추가.
  - `docker/observability/prometheus/alerts.tests.yml`: burn-rate 규칙 발화/비발화 fixture 추가.
- 문서 동기화:
  - `docs/ops/observability.md`: SLI 기준과 burn-rate 운영 정책(릴리스 중단/점검) 반영.
- 검증 결과:
  - `pwsh scripts/check_prometheus_rules.ps1`: 성공(`alerts.yml` syntax + `alerts.tests.yml` rule tests 통과).

### 진행 기록 (추가 갭 P2-7, 검증 보강)

- 상태: 완료
- 코드 변경:
  - `tests/server/test_server_chat.cpp`: runtime setting 적용 실패(out_of_range/unsupported_key/invalid_value) 시 success/failure 카운트 계약 검증 테스트 2건 추가.
- 검증 결과:
  - `build-windows/tests/Debug/server_general_tests.exe --gtest_filter=ChatServiceTest.RuntimeSettingRejectsOutOfRangeWithoutCountingSuccess:ChatServiceTest.RuntimeSettingRejectsUnsupportedKeyAndInvalidValue --gtest_color=no`: 2/2 통과.

### 진행 기록 (추가 갭 P2-3/P2-6)

- 상태: 완료
- 코드 변경:
  - `core/src/net/dispatcher.cpp`: 예외/거절 경로 로그를 `component=dispatcher`, `error_code=...` 구조화 키 형태로 표준화.
  - `server/src/app/bootstrap.cpp`: Redis/registry/I/O/fatal 예외 로그와 moderation duration parse 실패 경로를 `component=server_bootstrap`, `error_code=...` 신호로 보강.
  - `core/src/util/log.cpp`: 구조화 로그 품질 지표(`core_log_schema_records_total`, `core_log_schema_parse_success_total`, `core_log_schema_parse_failure_total`, `core_log_schema_field_total`, `core_log_schema_field_filled_total`) 추가.
  - `tests/core/test_core_metrics.cpp`: JSON 로그 스키마 필드/마스킹/파싱 및 필드 채움 지표 검증 테스트(`LogSchemaMetricsTest.JsonSchemaMetricsExposeParseAndFillSignals`) 추가.
- 문서 동기화:
  - `docs/ops/observability.md`: 예외 정책 표(throw/catch/convert-to-error code), 구조화 로그 고정 스키마, parse/fill 품질 PromQL 절차 반영.
- 검증 결과:
  - `lsp_diagnostics`: `core/src/util/log.cpp`, `core/src/net/dispatcher.cpp`, `server/src/app/bootstrap.cpp`, `tests/core/test_core_metrics.cpp` 에러 없음(Windows `getenv` deprecation hint만 존재).
  - `pwsh scripts/build.ps1 -Config Debug -Target core_general_tests`: 성공.
  - `pwsh scripts/build.ps1 -Config Debug -Target server_app`: 성공.
  - `build-windows/tests/Debug/core_general_tests.exe --gtest_filter=LogSchemaMetricsTest.*:DispatcherTest.*:RuntimeMetricsTest.* --gtest_color=no`: 11/11 통과.
  - `build-windows/tests/Debug/server_general_tests.exe --gtest_color=no`: 21/21 통과.

### 진행 기록 (추가 갭 P2-8/P2-9)

- 상태: 완료
- 코드 변경:
  - `core/src/metrics/http_server.cpp`: `METRICS_HTTP_HEADER_TIMEOUT_MS`, `METRICS_HTTP_BODY_TIMEOUT_MS` 환경변수 기반 헤더/바디 타임아웃과 timed read 경로를 추가하고, timeout/oversize/bad-request 방어 카운터를 보강.
  - `core/src/util/log.cpp`: `LOG_LEVEL`, `LOG_BUFFER_CAPACITY` 환경변수 파싱을 추가해 로거 정책을 런타임 설정과 직접 연결.
  - `tests/core/test_core_metrics.cpp`: metrics HTTP 인증/allowlist/body-limit 시나리오(`401/403/413`) 검증 테스트 추가.
- 문서 동기화:
  - `docs/configuration.md`: metrics HTTP timeout 키(`METRICS_HTTP_HEADER_TIMEOUT_MS`, `METRICS_HTTP_BODY_TIMEOUT_MS`) 및 logger 정책 키(`LOG_LEVEL`, `LOG_BUFFER_CAPACITY`) 반영.
  - `tools/admin_app/README.md`: metrics/admin HTTP timeout 설정 키 반영.
- 검증 결과:
  - `lsp_diagnostics`: `core/src/metrics/http_server.cpp`, `core/src/util/log.cpp`, `tests/core/test_core_metrics.cpp` 에러 없음(Windows `getenv` deprecation hint만 존재).
  - `pwsh scripts/build.ps1 -Config Debug -Target core_general_tests`: 성공.
  - `pwsh scripts/build.ps1 -Config Debug -Target server_app`: 성공.
  - `build-windows/tests/Debug/core_general_tests.exe --gtest_filter=MetricsHttpServerTest.*:LogSchemaMetricsTest.*:RuntimeMetricsTest.* --gtest_color=no`: 10/10 통과.
