#include "server/state/redis_instance_registry.hpp"

#include <utility>

namespace server::state {

namespace {

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
        auto record = detail::deserialize_json(*payload);
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
    const auto payload = detail::serialize_json(record);
    const auto ttl = static_cast<unsigned int>(ttl_.count());
    return client_->setex(key, payload, ttl);
}

std::shared_ptr<RedisInstanceStateBackend::IRedisClient>
make_redis_state_client(std::shared_ptr<server::core::storage::redis::IRedisClient> client) {
    return std::make_shared<RedisClientAdapter>(std::move(client));
}

} // namespace server::state
