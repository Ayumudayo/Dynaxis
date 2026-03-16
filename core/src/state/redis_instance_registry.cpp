#include "server/core/state/redis_instance_registry.hpp"

#include <array>
#include <cctype>
#include <sstream>
#include <string_view>
#include <utility>

namespace server::core::state {

namespace {

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

class RedisClientAdapter final : public RedisInstanceStateBackend::IRedisClient {
public:
    explicit RedisClientAdapter(std::shared_ptr<server::core::storage::redis::IRedisClient> client)
        : client_(std::move(client)) {}

    bool scan_keys(const std::string& pattern, std::vector<std::string>& keys) override {
        if (!client_) {
            return false;
        }
        return client_->scan_keys(pattern, keys);
    }

    std::optional<std::string> get(const std::string& key) override {
        if (!client_) {
            return std::nullopt;
        }
        return client_->get(key);
    }

    bool mget(const std::vector<std::string>& keys,
              std::vector<std::optional<std::string>>& out) override {
        if (!client_) {
            out.clear();
            return false;
        }
        return client_->mget(keys, out);
    }

    bool setex(const std::string& key, const std::string& value, unsigned int ttl_sec) override {
        if (!client_) {
            return false;
        }
        return client_->setex(key, value, ttl_sec);
    }

    bool del(const std::string& key) override {
        if (!client_) {
            return false;
        }
        return client_->del(key);
    }

private:
    std::shared_ptr<server::core::storage::redis::IRedisClient> client_;
};

} // namespace

RedisInstanceStateBackend::RedisInstanceStateBackend(std::shared_ptr<IRedisClient> client,
                                                     std::string key_prefix,
                                                     std::chrono::seconds ttl)
    : client_(std::move(client))
    , key_prefix_(std::move(key_prefix))
    , ttl_(ttl) {
    if (key_prefix_.empty()) {
        key_prefix_ = "gateway/instances/";
    }
    if (key_prefix_.back() != '/') {
        key_prefix_.push_back('/');
    }
    if (ttl_ <= std::chrono::seconds::zero()) {
        ttl_ = std::chrono::seconds{10};
    }
}

bool RedisInstanceStateBackend::upsert(const InstanceRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_[record.instance_id] = record;
    return write_record(record);
}

bool RedisInstanceStateBackend::remove(const std::string& instance_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.erase(instance_id);
    if (!client_) {
        return true;
    }
    return client_->del(key_prefix_ + instance_id);
}

bool RedisInstanceStateBackend::touch(const std::string& instance_id, std::uint64_t heartbeat_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(instance_id);
    if (it == cache_.end()) {
        return false;
    }
    it->second.last_heartbeat_ms = heartbeat_ms;
    return write_record(it->second);
}

bool RedisInstanceStateBackend::reload_cache_from_backend() const {
    if (!client_) {
        return false;
    }
    std::vector<std::string> keys;
    if (!client_->scan_keys(key_prefix_ + "*", keys)) {
        return false;
    }
    std::vector<std::optional<std::string>> payloads;
    const bool batch_loaded = !keys.empty()
        && client_->mget(keys, payloads)
        && payloads.size() == keys.size();

    std::unordered_map<std::string, InstanceRecord> next;
    next.reserve(keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto& key = keys[i];
        std::optional<std::string> payload;
        if (batch_loaded) {
            payload = std::move(payloads[i]);
        } else {
            payload = client_->get(key);
        }
        if (!payload || payload->empty()) {
            continue;
        }
        auto record = deserialize_json(*payload);
        if (!record) {
            continue;
        }
        if (record->instance_id.empty() && key.size() > key_prefix_.size()) {
            record->instance_id = key.substr(key_prefix_.size());
        }
        next[record->instance_id] = *record;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_ = std::move(next);
    }
    return true;
}

std::vector<InstanceRecord> RedisInstanceStateBackend::list_instances() const {
    if (client_) {
        const auto now = std::chrono::steady_clock::now();
        bool should_reload = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (now - last_reload_attempt_ >= reload_min_interval_) {
                last_reload_attempt_ = now;
                should_reload = true;
            }
        }

        if (should_reload) {
            bool expected = false;
            if (reload_in_progress_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                const bool ok = reload_cache_from_backend();
                reload_in_progress_.store(false, std::memory_order_release);
                if (ok) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    last_reload_ok_ = now;
                }
            }
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<InstanceRecord> result;
    result.reserve(cache_.size());
    for (const auto& [_, record] : cache_) {
        result.push_back(record);
    }
    return result;
}

bool RedisInstanceStateBackend::write_record(const InstanceRecord& record) {
    if (!client_) {
        return true;
    }
    const auto key = key_prefix_ + record.instance_id;
    const auto payload = serialize_json(record);
    const auto ttl = static_cast<unsigned int>(ttl_.count());
    return client_->setex(key, payload, ttl);
}

std::shared_ptr<RedisInstanceStateBackend::IRedisClient>
make_redis_state_client(std::shared_ptr<server::core::storage::redis::IRedisClient> client) {
    return std::make_shared<RedisClientAdapter>(std::move(client));
}

} // namespace server::core::state
