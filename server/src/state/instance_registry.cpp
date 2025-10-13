#include "server/state/instance_registry.hpp"

#include <sstream>
#include <utility>

#include "server/core/util/log.hpp"
#include "server/storage/redis/client.hpp"

namespace server::state {

namespace {
constexpr const char* kContentTypeHeader = "application/json";
} // namespace

bool InMemoryStateBackend::upsert(const InstanceRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    records_[record.instance_id] = record;
    return true;
}

bool InMemoryStateBackend::remove(const std::string& instance_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.erase(instance_id) > 0;
}

bool InMemoryStateBackend::touch(const std::string& instance_id, std::uint64_t heartbeat_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find(instance_id);
    if (it == records_.end()) {
        return false;
    }
    it->second.last_heartbeat_ms = heartbeat_ms;
    return true;
}

std::vector<InstanceRecord> InMemoryStateBackend::list_instances() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<InstanceRecord> result;
    result.reserve(records_.size());
    for (const auto& [_, record] : records_) {
        result.push_back(record);
    }
    return result;
}

namespace detail {

std::string serialize_json(const InstanceRecord& record) {
    std::ostringstream oss;
    oss << '{';
    oss << "\"instance_id\":\"" << record.instance_id << "\",";
    oss << "\"host\":\"" << record.host << "\",";
    oss << "\"port\":" << record.port << ',';
    oss << "\"role\":\"" << record.role << "\",";
    oss << "\"capacity\":" << record.capacity << ',';
    oss << "\"active_sessions\":" << record.active_sessions << ',';
    oss << "\"last_heartbeat_ms\":" << record.last_heartbeat_ms;
    oss << '}';
    return oss.str();
}

} // namespace detail

namespace {

class RedisClientAdapter final : public RedisInstanceStateBackend::IRedisClient {
public:
    explicit RedisClientAdapter(std::shared_ptr<server::storage::redis::IRedisClient> client)
        : client_(std::move(client)) {}

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
    std::shared_ptr<server::storage::redis::IRedisClient> client_;
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

std::vector<InstanceRecord> RedisInstanceStateBackend::list_instances() const {
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

ConsulInstanceStateBackend::ConsulInstanceStateBackend(std::string base_path,
                                                       http_callback put_callback,
                                                       http_callback delete_callback)
    : base_path_(std::move(base_path))
    , put_(std::move(put_callback))
    , del_(std::move(delete_callback)) {
    if (base_path_.empty()) {
        base_path_ = "kv/gateway/instances/";
    }
    if (base_path_.back() != '/') {
        base_path_.push_back('/');
    }
}

bool ConsulInstanceStateBackend::upsert(const InstanceRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_[record.instance_id] = record;
    if (!put_) {
        return true;
    }
    const auto payload = detail::serialize_json(record);
    return put_(make_path(record.instance_id), payload);
}

bool ConsulInstanceStateBackend::remove(const std::string& instance_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.erase(instance_id);
    if (!del_) {
        return true;
    }
    return del_(make_path(instance_id), "");
}

bool ConsulInstanceStateBackend::touch(const std::string& instance_id, std::uint64_t heartbeat_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(instance_id);
    if (it == cache_.end()) {
        return false;
    }
    it->second.last_heartbeat_ms = heartbeat_ms;
    if (!put_) {
        return true;
    }
    const auto payload = detail::serialize_json(it->second);
    return put_(make_path(instance_id), payload);
}

std::vector<InstanceRecord> ConsulInstanceStateBackend::list_instances() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<InstanceRecord> result;
    result.reserve(cache_.size());
    for (const auto& [_, record] : cache_) {
        result.push_back(record);
    }
    return result;
}

std::string ConsulInstanceStateBackend::make_path(const std::string& instance_id) const {
    return base_path_ + instance_id;
}

std::shared_ptr<RedisInstanceStateBackend::IRedisClient>
make_redis_state_client(std::shared_ptr<server::storage::redis::IRedisClient> client) {
    return std::make_shared<RedisClientAdapter>(std::move(client));
}

} // namespace server::state
