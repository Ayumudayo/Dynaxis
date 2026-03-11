#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace server::core::storage::redis {

/** @brief Shared Redis client creation options. */
struct Options {
    std::size_t pool_max{10};
    bool use_streams{false};
};

/**
 * @brief Shared Redis client abstraction used by gateway/server/tools.
 */
class IRedisClient {
public:
    virtual ~IRedisClient() = default;

    virtual bool health_check() = 0;
    virtual bool lpush_trim(const std::string& key, const std::string& value, std::size_t maxlen) = 0;
    virtual bool sadd(const std::string& key, const std::string& member) = 0;
    virtual bool srem(const std::string& key, const std::string& member) = 0;
    virtual bool smembers(const std::string& key, std::vector<std::string>& out) = 0;
    virtual bool scard(const std::string& key, std::size_t& out) = 0;
    virtual bool scard_many(const std::vector<std::string>& keys,
                            std::vector<std::size_t>& out) = 0;
    virtual bool del(const std::string& key) = 0;
    virtual std::optional<std::string> get(const std::string& key) = 0;
    virtual bool mget(const std::vector<std::string>& keys,
                      std::vector<std::optional<std::string>>& out) = 0;
    virtual bool set_if_not_exists(const std::string& key, const std::string& value, unsigned int ttl_sec) = 0;
    virtual bool set_if_equals(const std::string& key,
                               const std::string& expected,
                               const std::string& value,
                               unsigned int ttl_sec) = 0;
    virtual bool del_if_equals(const std::string& key, const std::string& expected) = 0;
    virtual bool scan_keys(const std::string& pattern, std::vector<std::string>& keys) = 0;
    virtual bool lrange(const std::string& key, long long start, long long stop, std::vector<std::string>& out) = 0;
    virtual bool scan_del(const std::string& pattern) = 0;
    virtual bool setex(const std::string& key, const std::string& value, unsigned int ttl_sec) = 0;
    virtual bool publish(const std::string& channel, const std::string& message) = 0;
    virtual bool start_psubscribe(const std::string& pattern,
                                  std::function<void(const std::string& channel, const std::string& message)> on_message) = 0;
    virtual void stop_psubscribe() = 0;
    virtual bool xgroup_create_mkstream(const std::string& key, const std::string& group) = 0;
    virtual bool xadd(const std::string& key,
                      const std::vector<std::pair<std::string, std::string>>& fields,
                      std::string* out_id = nullptr,
                      std::optional<std::size_t> maxlen = std::nullopt,
                      bool approximate = true) = 0;

    /** @brief Redis Stream 한 항목의 ID와 필드 페이로드를 담습니다. */
    struct StreamEntry {
        std::string id;
        std::vector<std::pair<std::string, std::string>> fields;
    };

    /** @brief XAUTOCLAIM 호출이 반환하는 후속 cursor와 회수된 항목 집합입니다. */
    struct StreamAutoClaimResult {
        std::string next_start_id;
        std::vector<StreamEntry> entries;
        std::vector<std::string> deleted_ids;
    };

    virtual bool xreadgroup(const std::string& key,
                            const std::string& group,
                            const std::string& consumer,
                            long long block_ms,
                            std::size_t count,
                            std::vector<StreamEntry>& out) = 0;
    virtual bool xack(const std::string& key, const std::string& group, const std::string& id) = 0;
    virtual bool xpending(const std::string& key, const std::string& group, long long& total) = 0;
    virtual bool xautoclaim(const std::string& key,
                            const std::string& group,
                            const std::string& consumer,
                            long long min_idle_ms,
                            const std::string& start,
                            std::size_t count,
                            StreamAutoClaimResult& out) = 0;
};

} // namespace server::core::storage::redis
