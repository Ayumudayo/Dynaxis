#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace server::core::storage::redis {

/**
 * @brief gateway/server/tools가 공통으로 쓰는 Redis 클라이언트 생성 옵션입니다.
 *
 * 이 옵션이 너무 구체적인 adapter 세부를 드러내지 않도록 유지하는 이유는, public
 * seam이 특정 Redis 라이브러리 설정 표면과 강하게 결합되지 않게 하기 위해서입니다.
 */
struct Options {
    std::size_t pool_max{10};
    bool use_streams{false};
};

/**
 * @brief gateway/server/tools가 공통으로 의존하는 축소 Redis 클라이언트 추상화입니다.
 *
 * 핵심 의도는 "모든 Redis 기능"을 공개하는 것이 아니라, 현재 저장소가 공유해야 하는
 * 최소 연산만 계약으로 고정하는 데 있습니다. 이렇게 해야 app-specific key schema나
 * 구체 라이브러리 결합을 public surface로 굳히지 않을 수 있습니다.
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
    virtual bool set(const std::string& key, const std::string& value) = 0;
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
