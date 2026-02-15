#pragma once

#include <ostream>
#include <string_view>

#include "server/core/build_info.hpp"

namespace server::core::metrics {

namespace detail {

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

// Writes a conventional build info metric:
//   knights_build_info{git_hash="...", git_describe="...", build_time_utc="..."} 1
//
// Keep this small and dependency-free so every binary can include it.
inline void append_build_info(std::ostream& out, std::string_view metric_name = "knights_build_info") {
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
