#include "server/storage/redis/client.hpp"

#include <memory>
#include <utility>

#if defined(HAVE_REDIS_PLUS_PLUS)
#include <sw/redis++/redis++.h>
#endif

namespace server::storage::redis {

#if defined(HAVE_REDIS_PLUS_PLUS)
class RedisClientImpl final : public IRedisClient {
public:
    explicit RedisClientImpl(std::string uri, Options opts)
        : redis_(sw::redis::ConnectionOptions(uri)) { (void)opts; }

    bool health_check() override {
        try { auto pong = redis_.ping(); return !pong.empty(); } catch (...) { return false; }
    }

private:
    sw::redis::Redis redis_;
};
#else
class RedisClientStub final : public IRedisClient {
public:
    explicit RedisClientStub(std::string uri, Options opts)
        : uri_(std::move(uri)), opts_(opts) {}
    bool health_check() override { (void)uri_; (void)opts_; return true; }
private:
    std::string uri_; Options opts_{};
};
#endif

std::shared_ptr<IRedisClient> make_redis_client(const std::string& uri, const Options& opts) {
#if defined(HAVE_REDIS_PLUS_PLUS)
    return std::make_shared<RedisClientImpl>(uri, opts);
#else
    return std::make_shared<RedisClientStub>(uri, opts);
#endif
}

} // namespace server::storage::redis
