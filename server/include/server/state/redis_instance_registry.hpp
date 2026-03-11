#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "server/core/storage/redis/client.hpp"
#include "server/state/instance_registry_common.hpp"

namespace server::state {

/** @brief Redis 키 공간을 인스턴스 레지스트리 백엔드로 사용하는 구현입니다. */
class RedisInstanceStateBackend final : public IInstanceStateBackend {
public:
    /** @brief 상태 레지스트리 구현에 필요한 Redis 연산만 노출하는 축소 어댑터입니다. */
    class IRedisClient {
    public:
        virtual ~IRedisClient() = default;
        virtual bool setex(const std::string& key, const std::string& value, unsigned int ttl_sec) = 0;
        virtual bool scan_keys(const std::string& pattern, std::vector<std::string>& keys) = 0;
        virtual std::optional<std::string> get(const std::string& key) = 0;
        virtual bool mget(const std::vector<std::string>& keys,
                          std::vector<std::optional<std::string>>& out) = 0;
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
    bool reload_cache_from_backend() const;
    bool write_record(const InstanceRecord& record);

    std::shared_ptr<IRedisClient> client_;
    std::string key_prefix_;
    std::chrono::seconds ttl_;
    std::chrono::milliseconds reload_min_interval_{500};
    mutable std::chrono::steady_clock::time_point last_reload_attempt_{};
    mutable std::chrono::steady_clock::time_point last_reload_ok_{};
    mutable std::atomic<bool> reload_in_progress_{false};
    mutable std::mutex mutex_;
    mutable std::unordered_map<std::string, InstanceRecord> cache_;
};

std::shared_ptr<RedisInstanceStateBackend::IRedisClient>
make_redis_state_client(std::shared_ptr<server::core::storage::redis::IRedisClient> client);

} // namespace server::state
