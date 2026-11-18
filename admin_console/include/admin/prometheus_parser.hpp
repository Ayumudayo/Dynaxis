#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace dynaxis::admin {

/**
 * @brief Prometheus exposition 텍스트(`/metrics`)에서 단일 숫자 메트릭 값을 추출합니다.
 *
 * 주석(`# HELP`, `# TYPE`)과 레이블 유무(`metric{...} value`)를 모두 처리하며 `NaN`/`Inf`는 무시합니다.
 *
 * @param body `/metrics` HTTP 응답 본문
 * @param metric_name 찾을 메트릭 이름
 * @return 첫 유효 샘플 값 (없거나 파싱 실패 시 `std::nullopt`)
 */
std::optional<double> extract_prometheus_metric_value(std::string_view body, std::string_view metric_name);

/**
 * @brief `/metrics` 응답에서 지정한 메트릭 이름의 값을 정수(`int64_t`)로 추출합니다.
 * @param body `/metrics` HTTP 응답 본문
 * @param metric_name 찾을 메트릭 이름
 * @return 반올림 또는 절단된 정수 값 (없으면 `std::nullopt`)
 */
std::optional<std::int64_t> extract_prometheus_int64(std::string_view body, std::string_view metric_name);

} // namespace dynaxis::admin
