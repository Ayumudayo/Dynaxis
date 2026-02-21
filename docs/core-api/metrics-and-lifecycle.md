# 메트릭/수명주기(Metrics/Lifecycle) API 가이드

## 안정성

| 헤더 | 안정성 |
|---|---|
| `server/core/app/app_host.hpp` | `[Stable]` |
| `server/core/app/termination_signals.hpp` | `[Stable]` |
| `server/core/metrics/metrics.hpp` | `[Stable]` |
| `server/core/metrics/http_server.hpp` | `[Stable]` |
| `server/core/metrics/build_info.hpp` | `[Stable]` |
| `server/core/runtime_metrics.hpp` | `[Stable]` |

## 수명주기 계약
- `AppHost` 수명주기 단계: `init -> bootstrapping -> running -> stopping -> stopped|failed`
- `termination_signals`는 `sig_atomic_t` 플래그 기반으로 프로세스 전역의 비동기 시그널 안전 종료 의도 폴링을 제공합니다.
- readiness/health는 수명주기 단계와 분리되어 독립적으로 보고할 수 있습니다.

## 메트릭 계약
- `metrics` API(`get_counter/get_gauge/get_histogram`)는 exporter 백엔드가 없을 때도 no-op 안전 fallback을 보장합니다.
- `MetricsHttpServer`는 `/metrics`, `/healthz`, `/readyz`와 선택적 커스텀 라우트를 노출합니다.
- `build_info` helper는 Prometheus 텍스트 포맷으로 `knights_build_info`를 출력합니다.
- `runtime_metrics` snapshot은 프로세스 전역 카운터와 히스토그램 버킷을 안정적인 읽기 계약으로 제공합니다.

## 운영 규칙
- 메트릭 콜백은 가볍고 논블로킹으로 유지합니다.
