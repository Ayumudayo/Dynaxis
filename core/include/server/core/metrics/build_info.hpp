#pragma once

#include <ostream>
#include <string_view>

#include "server/core/build_info.hpp"

namespace server::core::metrics {

namespace detail {

/**
 * @brief Prometheus label value에서 이스케이프가 필요한 문자를 처리합니다.
 * @param out 출력 스트림
 * @param v 원본 label 값
 */
inline void write_prometheus_escaped_label_value(std::ostream& out, std::string_view v) {
    for (const char ch : v) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default: out << ch; break;
        }
    }
}

} // namespace detail

/**
 * @brief 표준 build-info 메트릭 라인을 출력합니다.
 * @param out 출력 스트림
 * @param metric_name 출력할 메트릭 이름
 *
 * 출력 형식:
 * `runtime_build_info{git_hash="...", git_describe="...", build_time_utc="..."} 1`
 *
 * 의존성을 최소화해 모든 바이너리(server/gateway/tools)가 부담 없이 포함하도록 유지합니다.
 */
inline void append_build_info(std::ostream& out, std::string_view metric_name = "runtime_build_info") {
    out << "# TYPE " << metric_name << " gauge\n";
    out << metric_name << "{git_hash=\"";
    detail::write_prometheus_escaped_label_value(out, server::core::build_info::git_hash());
    out << "\",git_describe=\"";
    detail::write_prometheus_escaped_label_value(out, server::core::build_info::git_describe());
    out << "\",build_time_utc=\"";
    detail::write_prometheus_escaped_label_value(out, server::core::build_info::build_time_utc());
    out << "\"} 1\n";
}

} // namespace server::core::metrics
