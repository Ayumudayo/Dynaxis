#include "server/storage/redis/client.hpp"

#include <memory>
#include <utility>
#include <thread>
#include <atomic>
#include <functional>
#include <initializer_list>

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

    bool setex(const std::string& key, const std::string& value, unsigned int ttl_sec) override {
        try { redis_->setex(key, static_cast<long long>(ttl_sec), value); return true; } catch (const std::exception& e) { server::core::log::warn(std::string("Redis SETEX failed: ") + e.what()); return false; } catch (...) { server::core::log::warn("Redis SETEX failed: unknown"); return false; }
    }

    bool publish(const std::string& channel, const std::string& message) override {
        try { redis_->publish(channel, message); return true; } catch (const std::exception& e) { server::core::log::warn(std::string("Redis PUBLISH failed: ") + e.what()); return false; } catch (...) { server::core::log::warn("Redis PUBLISH failed: unknown"); return false; }
    }

    bool start_psubscribe(const std::string& pattern,
                          std::function<void(const std::string& channel, const std::string& message)> on_message) override {
        try {
            if (sub_running_) return true;
            // 콜백/패턴 저장(재연결 시 재사용)
            sub_cb_ = std::move(on_message);
            sub_pattern_ = pattern;

            subscriber_ = std::make_unique<sw::redis::Subscriber>(redis_->subscriber());
            subscriber_->on_pmessage([this](std::string /*pat*/, std::string channel, std::string msg) mutable {
                auto& cb = sub_cb_;
                if (cb) cb(channel, msg);
            });
            subscriber_->psubscribe(sub_pattern_);
            sub_running_ = true;
            sub_thread_ = std::thread([this]() {
                // 예외 시 점증 백오프(최대 1초), 필요 시 재구독
                int backoff_ms = 10;
                while (sub_running_) {
                    try {
                        if (subscriber_) {
                            subscriber_->consume();
                            // 정상 소비 시 백오프 리셋
                            backoff_ms = 10;
                        } else {
                            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
                        }
                    } catch (const std::exception&) {
                        // 재구성/재구독 시도
                        try {
                            subscriber_.reset();
                            auto sub = std::make_unique<sw::redis::Subscriber>(redis_->subscriber());
                            sub->on_pmessage([this](std::string /*pat*/, std::string channel, std::string msg) mutable {
                                auto& cb = sub_cb_;
                                if (cb) cb(channel, msg);
                            });
                            sub->psubscribe(sub_pattern_);
                            subscriber_ = std::move(sub);
                            backoff_ms = 10;
                        } catch (...) {
                            // 실패 시 백오프 증가
                            backoff_ms = std::min(1000, backoff_ms * 2);
                            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
                        }
                    } catch (...) {
                        backoff_ms = std::min(1000, backoff_ms * 2);
                        std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
                    }
                }
            });
            return true;
        } catch (const std::exception& e) {
            server::core::log::warn(std::string("Redis PSUBSCRIBE failed: ") + e.what());
            return false;
        } catch (...) { server::core::log::warn("Redis PSUBSCRIBE failed: unknown"); return false; }
    }

    void stop_psubscribe() override {
        try {
            sub_running_ = false;
            if (subscriber_) { try { subscriber_->punsubscribe(); } catch (...) {} }
            if (sub_thread_.joinable()) sub_thread_.join();
            subscriber_.reset();
            sub_cb_ = nullptr;
            sub_pattern_.clear();
        } catch (...) {}
    }

    // Streams 구현 (redis-plus-plus 1.3.15 기준)
    bool xgroup_create_mkstream(const std::string& key, const std::string& group) override {
        try {
            // '$'로 그룹 생성하며 mkstream=true. 이미 존재 시 ReplyError(BUSYGROUP) 무시.
            redis_->xgroup_create(key, group, "$", true);
            return true;
        } catch (const sw::redis::ReplyError& e) {
            // 그룹 존재 시 성공으로 간주
            return true;
        } catch (const std::exception& e) {
            server::core::log::warn(std::string("Redis XGROUP CREATE failed: ") + e.what());
            return false;
        } catch (...) {
            server::core::log::warn("Redis XGROUP CREATE failed: unknown");
            return false;
        }
    }

    bool xadd(const std::string& key,
              const std::vector<std::pair<std::string, std::string>>& fields,
              std::string* out_id,
              std::optional<std::size_t> maxlen,
              bool approximate) override {
        try {
            std::string id;
            if (maxlen && *maxlen > 0) {
                id = redis_->xadd(key, "*", fields.begin(), fields.end(), static_cast<long long>(*maxlen), approximate);
            } else {
                id = redis_->xadd(key, "*", fields.begin(), fields.end());
            }
            if (out_id) *out_id = id;
            return true;
        } catch (const std::exception& e) {
            server::core::log::warn(std::string("Redis XADD failed: ") + e.what());
            return false;
        } catch (...) { server::core::log::warn("Redis XADD failed: unknown"); return false; }
    }

    bool xreadgroup(const std::string& key,
                    const std::string& group,
                    const std::string& consumer,
                    long long block_ms,
                    std::size_t count,
                    std::vector<StreamEntry>& out) override {
        try {
            // 결과 컨테이너: [[ key, [ [id, [field, val]...], ... ] ]]
            using EntryFields = std::vector<std::pair<std::string, std::string>>;
            using IdAndFields = std::vector<std::pair<std::string, EntryFields>>;
            using StreamArr = std::vector<std::pair<std::string, IdAndFields>>;
            StreamArr arr;

            if (block_ms > 0) {
                redis_->xreadgroup(group, consumer, key, ">", std::chrono::milliseconds(block_ms), static_cast<long long>(count), false,
                                   std::back_inserter(arr));
            } else {
                redis_->xreadgroup(group, consumer, key, ">", static_cast<long long>(count), false,
                                   std::back_inserter(arr));
            }

            out.clear();
            if (arr.empty()) return true; // 데이터 없음

            // 단일 key만 읽으므로 첫 요소만 사용
            const auto& items = arr.front().second;
            out.reserve(items.size());
            for (const auto& id_fields : items) {
                StreamEntry e;
                e.id = id_fields.first;
                e.fields = id_fields.second;
                out.emplace_back(std::move(e));
            }
            return true;
        } catch (const std::exception& e) {
            server::core::log::warn(std::string("Redis XREADGROUP failed: ") + e.what());
            return false;
        } catch (...) { server::core::log::warn("Redis XREADGROUP failed: unknown"); return false; }
    }

    bool xack(const std::string& key, const std::string& group, const std::string& id) override {
        try {
            long long n = redis_->xack(key, group, std::initializer_list<std::string>{id});
            return n > 0;
        } catch (const std::exception& e) {
            server::core::log::warn(std::string("Redis XACK failed: ") + e.what());
            return false;
        } catch (...) { server::core::log::warn("Redis XACK failed: unknown"); return false; }
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

    bool xpending(const std::string& key, const std::string& group, long long& total) override {
        try {
            std::vector<std::pair<std::string, long long>> consumers;
            auto tpl = redis_->xpending(key, group, std::back_inserter(consumers));
            total = std::get<0>(tpl);
            return true;
        } catch (const std::exception& e) {
            server::core::log::warn(std::string("Redis XPENDING failed: ") + e.what());
            return false;
        } catch (...) {
            server::core::log::warn("Redis XPENDING failed: unknown");
            return false;
        }
    }

private:
    std::unique_ptr<sw::redis::Redis> redis_;
    std::unique_ptr<sw::redis::Subscriber> subscriber_;
    std::thread sub_thread_;
    std::atomic<bool> sub_running_{false};
    std::function<void(const std::string&, const std::string&)> sub_cb_{};
    std::string sub_pattern_{};
};
#endif

// 항상 사용 가능한 안전한 폴백 Stub 구현
class RedisClientStub final : public IRedisClient {
public:
    explicit RedisClientStub(std::string uri, Options opts)
        : uri_(std::move(uri)), opts_(opts) {}
    bool health_check() override { (void)uri_; (void)opts_; return true; }
    bool lpush_trim(const std::string& key, const std::string& value, std::size_t maxlen) override { (void)key; (void)value; (void)maxlen; return true; }
    bool sadd(const std::string& key, const std::string& member) override { (void)key; (void)member; return true; }
    bool srem(const std::string& key, const std::string& member) override { (void)key; (void)member; return true; }
    bool setex(const std::string& key, const std::string& value, unsigned int ttl_sec) override { (void)key; (void)value; (void)ttl_sec; return true; }
    bool publish(const std::string& channel, const std::string& message) override { (void)channel; (void)message; return true; }
    bool start_psubscribe(const std::string& pattern, std::function<void(const std::string&, const std::string&)> on_message) override { (void)pattern; (void)on_message; return true; }
    void stop_psubscribe() override {}
    bool xgroup_create_mkstream(const std::string& key, const std::string& group) override { (void)key; (void)group; return true; }
    bool xadd(const std::string& key, const std::vector<std::pair<std::string, std::string>>& fields, std::string* out_id, std::optional<std::size_t> maxlen, bool approximate) override { (void)key; (void)fields; (void)maxlen; (void)approximate; if (out_id) *out_id = "0-0"; return true; }
    bool xreadgroup(const std::string& key, const std::string& group, const std::string& consumer, long long block_ms, std::size_t count, std::vector<StreamEntry>& out) override { (void)key; (void)group; (void)consumer; (void)block_ms; (void)count; out.clear(); return true; }
    bool xack(const std::string& /*key*/, const std::string& /*group*/, const std::string& /*id*/) override { return true; }
    bool del(const std::string& key) override { (void)key; return true; }
    bool scan_del(const std::string& pattern) override { (void)pattern; return true; }
    bool xpending(const std::string& key, const std::string& group, long long& total) override { (void)key; (void)group; total = 0; return true; }
private:
    std::string uri_;
    Options opts_;
};

std::shared_ptr<IRedisClient> make_redis_client(const std::string& uri, const Options& opts) {
#if defined(HAVE_REDIS_PLUS_PLUS)
    try {
        return std::make_shared<RedisClientImpl>(uri, opts);
    } catch (const std::exception& e) {
        server::core::log::warn(std::string("Failed to create Redis client, fallback to stub: ") + e.what());
        return std::make_shared<RedisClientStub>(uri, opts);
    }
#else
    return std::make_shared<RedisClientStub>(uri, opts);
#endif
}

} // namespace server::storage::redis


