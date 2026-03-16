#pragma once

#include <cctype>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace server::core::state {

struct WorldLifecyclePolicy {
    bool draining{false};
    std::string replacement_owner_instance_id;

    bool reassignment_declared() const noexcept {
        return !replacement_owner_instance_id.empty();
    }

    bool empty() const noexcept {
        return !draining && replacement_owner_instance_id.empty();
    }
};

inline std::string trim_ascii_copy(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }

    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return std::string(value.substr(begin, end - begin));
}

inline bool parse_world_lifecycle_bool(std::string_view raw) {
    std::string normalized;
    normalized.reserve(raw.size());
    for (char ch : raw) {
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    return normalized == "1"
        || normalized == "true"
        || normalized == "yes"
        || normalized == "on";
}

inline std::optional<WorldLifecyclePolicy> parse_world_lifecycle_policy(std::string_view payload) {
    if (payload.empty()) {
        return std::nullopt;
    }

    WorldLifecyclePolicy policy;
    bool recognized = false;
    std::size_t offset = 0;
    while (offset <= payload.size()) {
        const std::size_t line_end = payload.find('\n', offset);
        const std::string_view line = line_end == std::string_view::npos
            ? payload.substr(offset)
            : payload.substr(offset, line_end - offset);
        if (!line.empty()) {
            const std::size_t separator = line.find('=');
            if (separator != std::string_view::npos) {
                const std::string key = trim_ascii_copy(line.substr(0, separator));
                const std::string value = trim_ascii_copy(line.substr(separator + 1));
                if (key == "draining") {
                    policy.draining = parse_world_lifecycle_bool(value);
                    recognized = true;
                } else if (key == "replacement_owner_instance_id") {
                    policy.replacement_owner_instance_id = value;
                    recognized = true;
                }
            }
        }

        if (line_end == std::string_view::npos) {
            break;
        }
        offset = line_end + 1;
    }

    if (!recognized) {
        return std::nullopt;
    }
    return policy;
}

inline std::string serialize_world_lifecycle_policy(const WorldLifecyclePolicy& policy) {
    std::ostringstream out;
    out << "draining=" << (policy.draining ? "1" : "0") << '\n';
    out << "replacement_owner_instance_id=" << policy.replacement_owner_instance_id << '\n';
    return out.str();
}

} // namespace server::core::state
