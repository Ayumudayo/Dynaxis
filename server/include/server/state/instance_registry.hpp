#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace server {
namespace storage::redis {
class IRedisClient;
} // namespace storage::redis
} // namespace server

namespace server::state {

struct InstanceRecord {
    std::string instance_id;
    std::string host;
    std::uint16_t port{0};
    std::string role;
    std::uint32_t capacity{0};
    std::uint32_t active_sessions{0};
    std::uint64_t last_heartbeat_ms{0};
};

class IInstanceStateBackend {
public:
    virtual ~IInstanceStateBackend() = default;
    virtual bool upsert(const InstanceRecord& record) = 0;
    virtual bool remove(const std::string& instance_id) = 0;
    virtual bool touch(const std::string& instance_id, std::uint64_t heartbeat_ms) = 0;
    virtual std::vector<InstanceRecord> list_instances() const = 0;
};

class InMemoryStateBackend final : public IInstanceStateBackend {
public:
    bool upsert(const InstanceRecord& record) override;
    bool remove(const std::string& instance_id) override;
    bool touch(const std::string& instance_id, std::uint64_t heartbeat_ms) override;
    std::vector<InstanceRecord> list_instances() const override;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, InstanceRecord> records_;
};

namespace detail {
std::string serialize_json(const InstanceRecord& record);
} // namespace detail

class RedisInstanceStateBackend final : public IInstanceStateBackend {
public:
    class IRedisClient {
    public:
        virtual ~IRedisClient() = default;
        virtual bool setex(const std::string& key, const std::string& value, unsigned int ttl_sec) = 0;
        virtual bool del(const std::string& key) = 0;
    };

    RedisInstanceStateBackend(std::shared_ptr<IRedisClient> client,
                              std::string key_prefix,
                              std::chrono::seconds ttl);

    bool upsert(const InstanceRecord& record) override;
    bool remove(const std::string& instance_id) override;
    bool touch(const std::string& instance_id, std::uint64_t heartbeat_ms) override;
    std::vector<InstanceRecord> list_instances() const override;

private:
    bool write_record(const InstanceRecord& record);

    std::shared_ptr<IRedisClient> client_;
    std::string key_prefix_;
    std::chrono::seconds ttl_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, InstanceRecord> cache_;
};

class ConsulInstanceStateBackend final : public IInstanceStateBackend {
public:
    using http_callback = std::function<bool(const std::string& path, const std::string& payload)>;

    ConsulInstanceStateBackend(std::string base_path,
                               http_callback put_callback,
                               http_callback delete_callback);

    bool upsert(const InstanceRecord& record) override;
    bool remove(const std::string& instance_id) override;
    bool touch(const std::string& instance_id, std::uint64_t heartbeat_ms) override;
    std::vector<InstanceRecord> list_instances() const override;

private:
    std::string make_path(const std::string& instance_id) const;

    std::string base_path_;
    http_callback put_;
    http_callback del_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, InstanceRecord> cache_;
};

std::shared_ptr<RedisInstanceStateBackend::IRedisClient>
make_redis_state_client(std::shared_ptr<server::storage::redis::IRedisClient> client);

} // namespace server::state
