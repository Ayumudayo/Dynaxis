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
 *
 * build 정보는 메트릭 label로 직접 노출되므로, escape 규칙이 흔들리면 scrape 자체가 깨질 수 있습니다.
 * 이 helper를 별도로 두는 이유는 모든 바이너리가 같은 escaping 규칙을 재사용하게 하기 위해서입니다.
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
 * 이 함수를 작은 header-only helper로 유지하는 이유는, 모든 바이너리(server/gateway/tools)가
 * 동일한 build 정보 메트릭을 거의 추가 비용 없이 노출하게 하기 위해서입니다.
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
