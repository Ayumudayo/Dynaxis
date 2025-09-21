# Observability 계획 — 로깅/메트릭/트레이싱

## 핵심 메트릭(권장 스키마)
- 처리량(Counter): `chat_msgs_in_total{server_id,room}`, `chat_msgs_out_total{server_id,room}`,
  `bytes_in_total{dir=ingress|egress}`, `bytes_out_total{dir}`
- 지연(Histogram/Summary): `pipeline_latency_ms{stage}`, `broadcast_fanout_ms{server_id,room}`,
  `db_query_ms{op}`, `redis_op_ms{cmd}`
- 동시성(Gauge): `connections{server_id}`, `sessions_active{server_id}`, `rooms_active{server_id}`, `room_members{room}`
- 큐/부하(Gauge/Counter): `job_queue_depth{type}`, `cpu_utilization`, `mem_bytes`, `sockets_open`, `send_queue_bytes{server_id}`
- 오류(Counter): `errors_total{type,stage}`, `disconnects_total{reason}`

## 파이프라인 단계(지연 분해)
- 단계: `accept` → `read_frame` → `decode` → `auth_check` → `route_lookup` → `repo_ops(db)` → `redis_ops` → `build_payload` → `fanout_send` → `write_frame`
- 수집: 각 단계 시작/종료 타임스탬프 기록 → `pipeline_latency_ms{stage}` 집계, end-to-end는 `stage="total"`
- fanout: 대상 수, 전송 완료까지의 지연을 `broadcast_fanout_ms{server_id,room}`로 기록

## 로그 필드(구조적 JSON)
- 공통: `ts`, `level`, `logger`, `msg`, `version`, `server_id`, `instance`, `gateway_id`, `origin`, `trace_id`, `span_id`
- 도메인: `opcode`, `room`, `user_id`, `session_id`, `msg_id`, `sizes{in,out}`, `durations{...}`, `fanout_targets`
- 정책: info 기본, debug 샘플링(예: 1%), 오류/지연 상위 P99는 항상 기록
- 출력: 표준출력(JSON Line) → 수집기(예: fluent-bit/Vector) → 백엔드(ELK/ClickHouse 등)

## 라벨/키 설계(분산 대비)
- 분산 식별: `server_id`(호스트/인스턴스), `gateway_id`(브릿지), `origin`(발신), `shard`(샤드/파티션), `build_version`
- 룸/유저: `room`, `user_id`, `session_id`
- 운영: `env`, `region`, `az`

## 복수 채팅 서버 지표
- 서버 단위: 처리량/지연/오류율/연결 수
- 룸 단위: `msgs_per_sec`, 멤버 수, fanout 지연 분포
- 분산 브로드캐스트: `publish_total`, `subscribe_total`, `subscribe_lag_ms`, `duplicates_dropped_total`, `self_echo_drop_total`
  - 현재 구현된 최소 로그: `metric=publish_total value=<n> room=<name>`, `metric=subscribe_total value=<n> room=<name>`, `metric=self_echo_drop_total value=<n>`, `metric=subscribe_lag_ms value=<ms> room=<name>`

## 수집/표시
- Prometheus pull 수집, Grafana 대시보드 구성
- 권장 대시보드 패널: 서버별 처리량/지연 히스토그램, 룸별 멤버/메시지, 오류 heatmap, Redis/DB round-trip, 큐 길이/CPU/메모리

## Metrics 노출(간단 HTTP)
- 환경 `METRICS_PORT` 설정 시 server_app이 `:METRICS_PORT/metrics`로 텍스트 포맷 노출
- 현재 노출되는 최소 지표(전역):
  - `chat_subscribe_total` (Counter)
  - `chat_self_echo_drop_total` (Counter)
  - `chat_subscribe_last_lag_ms` (Gauge)
- 예시: `METRICS_PORT=9090` → `curl http://127.0.0.1:9090/metrics`

## 트레이싱(선택)
- OpenTelemetry SDK 계측 → OTLP 수집기 → Jaeger/Tempo
- 로그/메트릭 라벨에 `trace_id`/`span_id`를 포함해 상관관계 구축

## 구현 포인트(우선순위)
- server_core: Acceptor(연결/해제 카운터+gauge, accept 지연), Session(read/write/bytes/frame, decode/encode 지연), JobQueue(큐 길이)
- Chat 핸들러: on_chat_send 스팬/히스토, fanout 대상/지연, snapshot/rooms/users 쿼리 지연
- Storage: Postgres op별 쿼리 지연/오류, Redis cmd별 지연/실패 카운트
- 분산: Pub/Sub publish/subscribe 카운트, subscribe lag, self-echo drop 카운트

## Write-behind 지표(추가)
- wb_batch_size: 배치 크기(이벤트 수) — Histogram
- wb_commit_ms: 배치 커밋 시간(ms) — Histogram/Summary
- wb_fail_total: 배치 실패 건수 — Counter
- wb_pending: 컨슈머 그룹 pending length — Gauge
- wb_dlq_total: DLQ로 이동한 이벤트 수 — Counter
  - 현재 구현된 최소 로그(키=값): `metric=wb_flush wb_commit_ms=<ms> wb_batch_size=<n> wb_ok_total=<n> wb_fail_total=<n> wb_dlq_total=<n>`, `metric=wb_pending value=<n>`

## 다음 단계(권장)
- 서버/워커에 Prometheus 지표를 직접 추가(wb_* 계열 Counter/Gauge/Histogram)
- 대시보드 초안: 워커 배치 크기/지연, 실패율, 펜딩 길이, DLQ 길이, 서버 subscribe lag 등
- 알람 임계치: `wb_fail_total` 증분, `wb_pending` 장시간 상승, `subscribe_lag_ms` P95 상향, `/metrics` 응답 실패
- 라벨 표준: `server_id`, `gateway_id`, `room_id`(가능 시), `env`, `build_version`

## 로그 수집 가이드(키=값 패턴)
- 형식: `metric=<name> k1=v1 k2=v2 ...` (공백 구분)
- 수집기 파서: 공백으로 분리 후 `key=value`를 필드로 파싱, 숫자형은 정수/실수로 변환
- 추천 필드: `server_id`, `gateway_id`, `room`, `stage`

## 메트릭 명세
- pipeline_latency_ms: 히스토그램 또는 서머리. 단위 ms. 라벨 `stage`(accept|read_frame|decode|auth_check|route_lookup|repo_ops|redis_ops|build_payload|fanout_send|write_frame|total), `server_id` 선택.
- chat_msgs_in_total: 카운터. 단위 건수. 라벨 `server_id`, `room`(또는 room_id). 입력 파이프라인에 들어온 채팅 메시지 건수.
- chat_msgs_out_total: 카운터. 단위 건수. 라벨 `server_id`, `room`(또는 room_id). 클라이언트로 송신된 채팅 메시지 건수.

## 게이트웨이/인증 단일 운용 가정
- Gateway/Auth는 초기 단일(논리) 인스턴스로 운용하되, 헬스체크/드레인/설정/로그/메트릭 표준화로 active/passive 전환 용이성 확보
- 필수 모니터링: 연결 수, 처리량, 오류율, P99 지연, 시스템 리소스, Redis/DB 헬스체크
