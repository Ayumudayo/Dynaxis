#include "admin/prometheus_parser.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

namespace dynaxis::admin {
namespace {

std::string_view trim_ws(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r')) {
        sv.remove_suffix(1);
    }
    return sv;
}

} // namespace

std::optional<double> extract_prometheus_metric_value(std::string_view body, std::string_view metric_name) {
    if (body.empty() || metric_name.empty()) {
        return std::nullopt;
    }

    std::size_t pos = 0;
    while (pos <= body.size()) {
        const std::size_t nl = body.find('\n', pos);
        const std::string_view raw_line =
            (nl == std::string_view::npos) ? body.substr(pos) : body.substr(pos, nl - pos);
        const std::string_view line = trim_ws(raw_line);

        if (!line.empty() && line.front() != '#') {
            if (line.size() > metric_name.size() && line.rfind(metric_name, 0) == 0) {
                const char next_ch = line[metric_name.size()];
                if (next_ch == ' ' || next_ch == '\t' || next_ch == '{') {
                    std::size_t val_start = metric_name.size();
                    if (next_ch == '{') {
                        bool in_quotes = false;
                        bool escaped = false;
                        std::size_t close_brace = std::string_view::npos;
                        for (std::size_t i = metric_name.size() + 1; i < line.size(); ++i) {
                            const char c = line[i];
                            if (escaped) {
                                escaped = false;
                                continue;
                            }
                            if (in_quotes) {
                                if (c == '\\') {
                                    escaped = true;
                                } else if (c == '"') {
                                    in_quotes = false;
                                }
                                continue;
                            }
                            if (c == '"') {
                                in_quotes = true;
                            } else if (c == '}') {
                                close_brace = i;
                                break;
                            }
                        }
                        if (close_brace == std::string_view::npos) {
                            val_start = std::string_view::npos;
                        } else {
                            val_start = close_brace + 1;
                        }
                    }

                    if (val_start != std::string_view::npos) {
                        std::string_view rest = trim_ws(line.substr(val_start));
                        const std::size_t sep = rest.find_first_of(" \t");
                        if (sep != std::string_view::npos) {
                            rest = rest.substr(0, sep);
                        }
                        if (!rest.empty()) {
                            const std::string token(rest);
                            char* end_ptr = nullptr;
                            const double parsed = std::strtod(token.c_str(), &end_ptr);
                            if (end_ptr != token.c_str() && *end_ptr == '\0' && std::isfinite(parsed)) {
                                return parsed;
                            }
                        }
                    }
                }
            }
        }

        if (nl == std::string_view::npos) {
            break;
        }
        pos = nl + 1;
    }

    return std::nullopt;
}

std::optional<std::int64_t> extract_prometheus_int64(std::string_view body, std::string_view metric_name) {
    const auto val = extract_prometheus_metric_value(body, metric_name);
    if (!val.has_value()) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(std::llround(*val));
}

} // namespace dynaxis::admin
