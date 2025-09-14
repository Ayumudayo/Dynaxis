#include "server/storage/redis/client.hpp"

#include <memory>
#include <utility>

#if defined(HAVE_REDIS_PLUS_PLUS)
#include <sw/redis++/redis++.h>
#endif
#include "server/core/util/log.hpp"

namespace server::storage::redis {

#if defined(HAVE_REDIS_PLUS_PLUS)
class RedisClientImpl final : public IRedisClient {
public:
    explicit RedisClientImpl(const std::string& uri, Options opts) {
        sw::redis::ConnectionOptions conn_opts;
        conn_opts.uri = uri; // 지원되는 형태: redis://, rediss://, tcp:// 등
        sw::redis::ConnectionPoolOptions pool_opts;
        if (opts.pool_max > 0) pool_opts.size = opts.pool_max;
        redis_ = std::make_unique<sw::redis::Redis>(conn_opts, pool_opts);
    }

    bool health_check() override {
        try { auto pong = redis_->ping(); return !pong.empty(); } catch (...) { return false; }
    }

    bool lpush_trim(const std::string& key, const std::string& value, std::size_t maxlen) override {
        try {
            redis_->lpush(key, value);
            if (maxlen > 0) redis_->ltrim(key, 0, static_cast<long long>(maxlen - 1));
            return true;
        } catch (...) { return false; }
    }

private:
    std::unique_ptr<sw::redis::Redis> redis_;
};
#else
class RedisClientStub final : public IRedisClient {
public:
    explicit RedisClientStub(std::string uri, Options opts)
        : uri_(std::move(uri)), opts_(opts) {}
    bool health_check() override { (void)uri_; (void)opts_; return true; }
    bool lpush_trim(const std::string& key, const std::string& value, std::size_t maxlen) override { (void)key; (void)value; (void)maxlen; return true; }
private:
    std::string uri_; Options opts_{};
};
#endif

std::shared_ptr<IRedisClient> make_redis_client(const std::string& uri, const Options& opts) {
#if defined(HAVE_REDIS_PLUS_PLUS)
    server::core::log::info("Redis backend: redis-plus-plus (real client)");
    return std::make_shared<RedisClientImpl>(uri, opts);
#else
    server::core::log::warn("Redis backend: stub (redis-plus-plus not found at build time)");
    return std::make_shared<RedisClientStub>(uri, opts);
#endif
}

} // namespace server::storage::redis
