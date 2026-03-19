# 메트릭/수명주기(Metrics/Lifecycle) API 가이드

## 안정성

| 헤더 | 안정성 |
|---|---|
| `server/core/app/app_host.hpp` | `[Stable]` |
| `server/core/app/engine_builder.hpp` | `[Stable]` |
| `server/core/app/engine_context.hpp` | `[Stable]` |
| `server/core/app/engine_runtime.hpp` | `[Stable]` |
| `server/core/app/termination_signals.hpp` | `[Stable]` |
| `server/core/metrics/metrics.hpp` | `[Stable]` |
| `server/core/metrics/http_server.hpp` | `[Stable]` |
| `server/core/metrics/build_info.hpp` | `[Stable]` |
| `server/core/runtime_metrics.hpp` | `[Stable]` |

## 수명주기 계약
- `AppHost` 수명주기 단계: `init -> bootstrapping -> running -> stopping -> stopped|failed`
- `EngineBuilder`는 bootstrap 기본값(`bootstrapping`, `ready=false`, declared dependencies, optional admin HTTP)을 같은 규약으로 설정합니다.
- `EngineRuntime`는 `AppHost` + `EngineContext` 조합으로 lifecycle/dependency/shutdown/admin-http를 한 인스턴스 단위로 묶습니다.
- `EngineRuntime::set_alias()` / `bridge_alias()`는 stack object reference를 alias shared_ptr로 context/global registry에 연결하는 표준 helper입니다.
- `EngineRuntime::snapshot()`은 lifecycle/readiness/stop/context-service-count/compatibility-bridge-count를 인스턴스 단위로 읽는 canonical consumer surface입니다.
- `EngineRuntime::mark_running()` / `mark_stopped()` / `mark_failed()`는 readiness + lifecycle terminal transitions를 같은 shape로 적용합니다.
- `EngineRuntime::wait_for_stop()`은 non-Asio control-plane/worker loop가 termination polling을 open-code하지 않도록 하는 표준 helper입니다.
- `EngineRuntime::run_shutdown()`은 `request_stop() + ready=false + registered shutdown steps`를 normal teardown 경로에서도 같은 shape로 실행합니다.
- `EngineRuntime::clear_global_services()`는 legacy `ServiceRegistry` compatibility bridge를 runtime-owner 단위로 정리해 peer runtime bridge를 지우지 않습니다.
- `EngineContext`는 앱별 bootstrap이 core primitive를 지역적으로 등록하는 instance-scoped typed registry이며 `service_count()`로 local ownership을 직접 관측할 수 있습니다.
- `termination_signals`는 `sig_atomic_t` 플래그 기반으로 프로세스 전역의 비동기 시그널 안전 종료 의도 폴링을 제공합니다.
- readiness/health는 수명주기 단계와 분리되어 독립적으로 보고할 수 있습니다.
- `AppHost::start_admin_http()` / `EngineRuntime::start_admin_http()`는 built-in `/metrics`/`/healthz`/`/readyz` 외에 optional custom route callback도 받을 수 있고, registered shutdown path에 admin-http stop step을 함께 연결합니다.

## 메트릭 계약
- `metrics` API(`get_counter/get_gauge/get_histogram`)는 exporter 백엔드가 없을 때도 no-op 안전 fallback을 보장합니다.
- `MetricsHttpServer`는 `/metrics`, `/healthz`, `/readyz`와 선택적 커스텀 라우트를 노출합니다.
- `build_info` helper는 Prometheus 텍스트 포맷으로 `runtime_build_info`를 출력합니다.
- `runtime_metrics` snapshot은 프로세스 전역 카운터와 히스토그램 버킷을 안정적인 읽기 계약으로 제공합니다.
- canonical embeddability consumer는 인스턴스 ownership 관측에 `EngineRuntime::snapshot()`을 우선 사용하고, `runtime_metrics`는 process-wide operational telemetry로 취급합니다.

## 운영 규칙
- 메트릭 콜백은 가볍고 논블로킹으로 유지합니다.
- public bootstrap은 `server/core/**` 헤더만으로 조립 가능해야 하며, 앱별 route/listener/worker 세부 구현은 runtime composition 위에 남깁니다.
