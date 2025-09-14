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
        (void)opts;
        // 일부 배포판에선 ConnectionOptions.uri 필드가 없을 수 있음. 간단한 URI 생성자를 사용.
        redis_ = std::make_unique<sw::redis::Redis>(uri);
    }

    bool health_check() override {
        try {
            auto pong = redis_->ping();
            return !pong.empty();
        } catch (const std::exception& e) {
            server::core::log::warn(std::string("Redis PING failed: ") + e.what());
            return false;
        } catch (...) {
            server::core::log::warn("Redis PING failed: unknown exception");
            return false;
        }
    }

    bool lpush_trim(const std::string& key, const std::string& value, std::size_t maxlen) override {
        try {
            redis_->lpush(key, value);
            if (maxlen > 0) redis_->ltrim(key, 0, static_cast<long long>(maxlen - 1));
            return true;
        } catch (const std::exception& e) {
            server::core::log::warn(std::string("Redis LPUSH/LTRIM failed: ") + e.what());
            return false;
        } catch (...) {
            server::core::log::warn("Redis LPUSH/LTRIM failed: unknown exception");
            return false;
        }
    }

    bool sadd(const std::string& key, const std::string& member) override {
        try { redis_->sadd(key, member); return true; } catch (const std::exception& e) { server::core::log::warn(std::string("Redis SADD failed: ") + e.what()); return false; } catch (...) { server::core::log::warn("Redis SADD failed: unknown"); return false; }
    }
    bool srem(const std::string& key, const std::string& member) override {
        try { redis_->srem(key, member); return true; } catch (const std::exception& e) { server::core::log::warn(std::string("Redis SREM failed: ") + e.what()); return false; } catch (...) { server::core::log::warn("Redis SREM failed: unknown"); return false; }
    }

    bool del(const std::string& key) override {
        try { redis_->del(key); return true; } catch (const std::exception& e) { server::core::log::warn(std::string("Redis DEL failed: ") + e.what()); return false; } catch (...) { server::core::log::warn("Redis DEL failed: unknown"); return false; }
    }

    bool scan_del(const std::string& pattern) override {
        try {
            long long cursor = 0;
            do {
                std::vector<std::string> keys;
                cursor = redis_->scan(cursor, pattern, 100, std::back_inserter(keys));
                if (!keys.empty()) {
                    redis_->del(keys.begin(), keys.end());
                }
            } while (cursor != 0);
            return true;
        } catch (const std::exception& e) {
            server::core::log::warn(std::string("Redis SCAN/DEL failed: ") + e.what());
            return false;
        } catch (...) {
            server::core::log::warn("Redis SCAN/DEL failed: unknown");
            return false;
        }
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
    bool sadd(const std::string& key, const std::string& member) override { (void)key; (void)member; return true; }
    bool srem(const std::string& key, const std::string& member) override { (void)key; (void)member; return true; }
    bool del(const std::string& key) override { (void)key; return true; }
    bool scan_del(const std::string& pattern) override { (void)pattern; return true; }
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
