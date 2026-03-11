#include "server/state/instance_registry_common.hpp"

#include <array>
#include <cctype>
#include <sstream>
#include <string_view>
#include <utility>

namespace server::state::detail {

using JsonFieldSetter = void (*)(InstanceRecord&, std::string_view);

std::string trim_ascii_copy(std::string_view view) {
    std::size_t start = 0;
    std::size_t end = view.size();
    while (start < end && std::isspace(static_cast<unsigned char>(view[start])) != 0) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(view[end - 1])) != 0) {
        --end;
    }
    return std::string(view.substr(start, end - start));
}

std::vector<std::string> split_pipe_tokens(std::string_view value) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= value.size()) {
        std::size_t end = value.find('|', start);
        if (end == std::string_view::npos) {
            end = value.size();
        }

        std::string token = trim_ascii_copy(value.substr(start, end - start));
        if (!token.empty()) {
            out.push_back(std::move(token));
        }

        if (end == value.size()) {
            break;
        }
        start = end + 1;
    }
    return out;
}

std::string join_pipe_tokens(const std::vector<std::string>& values) {
    std::ostringstream oss;
    bool first = true;
    for (const auto& value : values) {
        if (value.empty()) {
            continue;
        }
        if (!first) {
            oss << '|';
        }
        oss << value;
        first = false;
    }
    return oss.str();
}

void set_instance_id(InstanceRecord& record, std::string_view value) { record.instance_id = value; }
void set_host(InstanceRecord& record, std::string_view value) { record.host = value; }
void set_role(InstanceRecord& record, std::string_view value) { record.role = value; }
void set_game_mode(InstanceRecord& record, std::string_view value) { record.game_mode = value; }
void set_region(InstanceRecord& record, std::string_view value) { record.region = value; }
void set_shard(InstanceRecord& record, std::string_view value) { record.shard = value; }
void set_tags(InstanceRecord& record, std::string_view value) { record.tags = split_pipe_tokens(value); }
void set_port(InstanceRecord& record, std::string_view value) { record.port = static_cast<std::uint16_t>(std::stoul(std::string(value))); }
void set_capacity(InstanceRecord& record, std::string_view value) { record.capacity = static_cast<std::uint32_t>(std::stoul(std::string(value))); }
void set_active_sessions(InstanceRecord& record, std::string_view value) { record.active_sessions = static_cast<std::uint32_t>(std::stoul(std::string(value))); }

void set_ready(InstanceRecord& record, std::string_view value) {
    if (value == "true" || value == "1") {
        record.ready = true;
    } else if (value == "false" || value == "0") {
        record.ready = false;
    }
}

void set_last_heartbeat_ms(InstanceRecord& record, std::string_view value) {
    record.last_heartbeat_ms = static_cast<std::uint64_t>(std::stoull(std::string(value)));
}

struct JsonFieldRule {
    std::string_view key;
    JsonFieldSetter setter;
};

constexpr std::array<JsonFieldRule, 12> kJsonFieldRules{{
    {"instance_id", &set_instance_id},
    {"host", &set_host},
    {"role", &set_role},
    {"game_mode", &set_game_mode},
    {"region", &set_region},
    {"shard", &set_shard},
    {"tags", &set_tags},
    {"port", &set_port},
    {"capacity", &set_capacity},
    {"active_sessions", &set_active_sessions},
    {"ready", &set_ready},
    {"last_heartbeat_ms", &set_last_heartbeat_ms},
}};

const JsonFieldSetter find_json_field_setter(std::string_view key) {
    for (const auto& rule : kJsonFieldRules) {
        if (rule.key == key) {
            return rule.setter;
        }
    }
    return nullptr;
}

std::string serialize_json(const InstanceRecord& record) {
    std::ostringstream oss;
    const std::string tags = join_pipe_tokens(record.tags);
    oss << '{';
    oss << "\"instance_id\":\"" << record.instance_id << "\",";
    oss << "\"host\":\"" << record.host << "\",";
    oss << "\"port\":" << record.port << ',';
    oss << "\"role\":\"" << record.role << "\",";
    oss << "\"game_mode\":\"" << record.game_mode << "\",";
    oss << "\"region\":\"" << record.region << "\",";
    oss << "\"shard\":\"" << record.shard << "\",";
    oss << "\"tags\":\"" << tags << "\",";
    oss << "\"capacity\":" << record.capacity << ',';
    oss << "\"active_sessions\":" << record.active_sessions << ',';
    oss << "\"ready\":" << (record.ready ? "true" : "false") << ',';
    oss << "\"last_heartbeat_ms\":" << record.last_heartbeat_ms;
    oss << '}';
    return oss.str();
}

std::optional<InstanceRecord> deserialize_json(std::string_view payload) {
    InstanceRecord record{};
    std::string json(payload);
    for (char& ch : json) {
        if (ch == '{' || ch == '}') {
            ch = ' ';
        }
    }
    std::stringstream ss(json);
    std::string item;
    while (std::getline(ss, item, ',')) {
        auto colon = item.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string key = trim_ascii_copy(std::string_view(item).substr(0, colon));
        std::string value = trim_ascii_copy(std::string_view(item).substr(colon + 1));
        if (!key.empty() && key.front() == '"') { key.erase(0, 1); }
        if (!key.empty() && key.back() == '"') { key.pop_back(); }
        if (!value.empty() && value.front() == '"') { value.erase(0, 1); }
        if (!value.empty() && value.back() == '"') { value.pop_back(); }

        const auto setter = find_json_field_setter(key);
        if (setter == nullptr) {
            continue;
        }

        try {
            setter(record, value);
        } catch (...) {
        }
    }
    return record;
}

} // namespace server::state::detail
