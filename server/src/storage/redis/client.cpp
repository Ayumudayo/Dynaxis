#include "server/storage/redis/client.hpp"

#include <memory>
#include <utility>
#include <thread>
#include <atomic>
#include <functional>
#include <initializer_list>
#include <optional>

#if defined(HAVE_REDIS_PLUS_PLUS)
#include <sw/redis++/redis++.h>
#endif
#include "server/core/util/log.hpp"

namespace server::storage::redis {

#if defined(HAVE_REDIS_PLUS_PLUS)
// redis++ 기반 실제 Redis 클라이언트 구현
// redis-plus-plus 라이브러리를 사용하여 Redis 서버와 통신합니다.
// 모든 메서드는 예외를 잡아서 false나 nullopt를 반환하도록 래핑되어 있어,
// Redis 장애가 서버 전체의 크래시로 이어지지 않도록 방어적으로 구현되었습니다.
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

    bool smembers(const std::string& key, std::vector<std::string>& out) override {
        try {
            out.clear();
            redis_->smembers(key, std::back_inserter(out));
            return true;
        } catch (const std::exception& e) {
            server::core::log::warn(std::string("Redis SMEMBERS failed: ") + e.what());
            return false;
        } catch (...) {
            server::core::log::warn("Redis SMEMBERS failed: unknown");
            return false;
        }
    }

    bool setex(const std::string& key, const std::string& value, unsigned int ttl_sec) override {
        try { redis_->setex(key, static_cast<long long>(ttl_sec), value); return true; } catch (const std::exception& e) { server::core::log::warn(std::string("Redis SETEX failed: ") + e.what()); return false; } catch (...) { server::core::log::warn("Redis SETEX failed: unknown"); return false; }
    }

    bool publish(const std::string& channel, const std::string& message) override {
        try { redis_->publish(channel, message); return true; } catch (const std::exception& e) { server::core::log::warn(std::string("Redis PUBLISH failed: ") + e.what()); return false; } catch (...) { server::core::log::warn("Redis PUBLISH failed: unknown"); return false; }
    }

    // pattern에 대해 별도 쓰레드로 consume()을 돌리며 콜백을 호출한다.
    // Pub/Sub은 블로킹 작업이므로 별도의 스레드에서 실행해야 메인 로직을 방해하지 않습니다.
    // 네트워크 단절 시 자동으로 재접속 및 재구독을 시도하는 복구 로직이 포함되어 있습니다.
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
            // consume() 실패 시 짧은 backoff 후 재시도한다.
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

    std::optional<std::string> get(const std::string& key) override {
        try {
            auto value = redis_->get(key);
            if (value) {
                return *value;
            }
            return std::nullopt;
        } catch (const std::exception& e) {
            server::core::log::warn(std::string("Redis GET failed: ") + e.what());
            return std::nullopt;
        } catch (...) {
            server::core::log::warn("Redis GET failed: unknown");
            return std::nullopt;
        }
    }

    bool mget(const std::vector<std::string>& keys,
              std::vector<std::optional<std::string>>& out) override {
        try {
            out.clear();
            if (keys.empty()) {
                return true;
            }
            std::vector<sw::redis::OptionalString> raw;
            raw.reserve(keys.size());
            redis_->mget(keys.begin(), keys.end(), std::back_inserter(raw));
            out.reserve(raw.size());
            for (const auto& value : raw) {
                if (value) {
                    out.emplace_back(*value);
                } else {
                    out.emplace_back(std::nullopt);
                }
            }
            return out.size() == keys.size();
        } catch (const std::exception& e) {
            server::core::log::warn(std::string("Redis MGET failed: ") + e.what());
            out.clear();
            return false;
        } catch (...) {
            server::core::log::warn("Redis MGET failed: unknown");
            out.clear();
            return false;
        }
    }

    bool set_if_not_exists(const std::string& key, const std::string& value, unsigned int ttl_sec) override {
        try {
            std::string ttl = std::to_string(ttl_sec);
            long long result = redis_->eval<long long>("if redis.call('EXISTS', KEYS[1]) == 0 then redis.call('SET', KEYS[1], ARGV[1], 'EX', ARGV[2]); return 1 else return 0 end", {key}, {value, ttl});
            return result == 1;
        } catch (const std::exception& e) {
            server::core::log::warn(std::string("Redis SET NX failed: ") + e.what());
            return false;
        } catch (...) {
            server::core::log::warn("Redis SET NX failed: unknown");
            return false;
        }
    }

    bool set_if_equals(const std::string& key, const std::string& expected, const std::string& value, unsigned int ttl_sec) override {
        try {
            std::string ttl = std::to_string(ttl_sec);
            long long result = redis_->eval<long long>("if redis.call('GET', KEYS[1]) == ARGV[1] then redis.call('SET', KEYS[1], ARGV[2], 'EX', ARGV[3]); return 1 else return 0 end", {key}, {expected, value, ttl});
            return result == 1;
        } catch (const std::exception& e) {
            server::core::log::warn(std::string("Redis conditional SET failed: ") + e.what());
            return false;
        } catch (...) {
            server::core::log::warn("Redis conditional SET failed: unknown");
            return false;
        }
    }

    bool del_if_equals(const std::string& key, const std::string& expected) override {
        try {
            long long result = redis_->eval<long long>("if redis.call('GET', KEYS[1]) == ARGV[1] then return redis.call('DEL', KEYS[1]) else return 0 end", {key}, {expected});
            return result > 0;
        } catch (const std::exception& e) {
            server::core::log::warn(std::string("Redis conditional DEL failed: ") + e.what());
            return false;
        } catch (...) {
            server::core::log::warn("Redis conditional DEL failed: unknown");
            return false;
        }
    }

    bool scan_keys(const std::string& pattern, std::vector<std::string>& keys) override {
        try {
            keys.clear();
            long long cursor = 0;
            do {
                std::vector<std::string> batch;
                cursor = redis_->scan(cursor, pattern, 100, std::back_inserter(batch));
                keys.insert(keys.end(), batch.begin(), batch.end());
            } while (cursor != 0);
            return true;
        } catch (const std::exception& e) {
            server::core::log::warn(std::string("Redis SCAN failed: ") + e.what());
            return false;
        } catch (...) {
            server::core::log::warn("Redis SCAN failed: unknown");
            return false;
        }
    }
    bool lrange(const std::string& key, long long start, long long stop, std::vector<std::string>& out) override {
        try {
            out.clear();
            redis_->lrange(key, start, stop, std::back_inserter(out));
            return true;
        } catch (const std::exception& e) {
            server::core::log::warn(std::string("Redis LRANGE failed: ") + e.what());
            return false;
        } catch (...) {
            server::core::log::warn("Redis LRANGE failed: unknown");
            return false;
        }
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

    bool xautoclaim(const std::string& key,
                    const std::string& group,
                    const std::string& consumer,
                    long long min_idle_ms,
                    const std::string& start,
                    std::size_t count,
                    StreamAutoClaimResult& out) override {
        try {
            out.next_start_id.clear();
            out.entries.clear();
            out.deleted_ids.clear();

            if (count == 0) {
                count = 1;
            }
            const auto min_idle_str = std::to_string(min_idle_ms < 0 ? 0 : min_idle_ms);
            const auto count_str = std::to_string(static_cast<unsigned long long>(count));

            auto reply = redis_->command("XAUTOCLAIM", key, group, consumer, min_idle_str, start, "COUNT", count_str);
            if (!reply) {
                return false;
            }

            if (!sw::redis::reply::is_array(*reply) || reply->elements < 2) {
                throw sw::redis::ParseError("ARRAY(>=2)", *reply);
            }

            out.next_start_id = sw::redis::reply::parse<std::string>(*reply->element[0]);

            auto* entries_reply = reply->element[1];
            if (entries_reply && sw::redis::reply::is_array(*entries_reply) && entries_reply->element) {
                out.entries.reserve(entries_reply->elements);
                for (std::size_t i = 0; i < entries_reply->elements; ++i) {
                    auto* entry_reply = entries_reply->element[i];
                    if (!entry_reply || !sw::redis::reply::is_array(*entry_reply) || entry_reply->elements < 2) {
                        continue;
                    }

                    StreamEntry e;
                    e.id = sw::redis::reply::parse<std::string>(*entry_reply->element[0]);

                    auto* fields_reply = entry_reply->element[1];
                    if (fields_reply && sw::redis::reply::is_array(*fields_reply) && fields_reply->element) {
                        if (fields_reply->elements % 2 != 0) {
                            throw sw::redis::ProtoError("XAUTOCLAIM entry fields not key-value pair array");
                        }
                        e.fields.reserve(fields_reply->elements / 2);
                        for (std::size_t f = 0; f < fields_reply->elements; f += 2) {
                            auto* k = fields_reply->element[f];
                            auto* v = fields_reply->element[f + 1];
                            if (!k || !v) {
                                throw sw::redis::ProtoError("XAUTOCLAIM entry has null field reply");
                            }
                            e.fields.emplace_back(
                                sw::redis::reply::parse<std::string>(*k),
                                sw::redis::reply::parse<std::string>(*v)
                            );
                        }
                    }

                    out.entries.emplace_back(std::move(e));
                }
            }

            // Redis 7+ may return deleted IDs as a 3rd element.
            if (reply->elements >= 3) {
                auto* deleted_reply = reply->element[2];
                if (deleted_reply && sw::redis::reply::is_array(*deleted_reply) && deleted_reply->element) {
                    out.deleted_ids.reserve(deleted_reply->elements);
                    for (std::size_t i = 0; i < deleted_reply->elements; ++i) {
                        auto* id_reply = deleted_reply->element[i];
                        if (!id_reply) {
                            continue;
                        }
                        out.deleted_ids.emplace_back(sw::redis::reply::parse<std::string>(*id_reply));
                    }
                }
            }

            return true;
        } catch (const std::exception& e) {
            server::core::log::warn(std::string("Redis XAUTOCLAIM failed: ") + e.what());
            return false;
        } catch (...) {
            server::core::log::warn("Redis XAUTOCLAIM failed: unknown");
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
// redis++가 없는 빌드에서는 no-op Stub을 사용한다.
// 이를 통해 Redis 라이브러리가 없는 환경에서도 컴파일 및 실행이 가능하며(기능은 동작 안 함),
// 개발 초기 단계나 로컬 테스트 시 유용합니다.
class RedisClientStub final : public IRedisClient {
public:
    explicit RedisClientStub(std::string uri, Options opts)
        : uri_(std::move(uri)), opts_(opts) {}
    bool health_check() override { (void)uri_; (void)opts_; return true; }
    bool lpush_trim(const std::string& key, const std::string& value, std::size_t maxlen) override { (void)key; (void)value; (void)maxlen; return true; }
    bool sadd(const std::string& key, const std::string& member) override { (void)key; (void)member; return true; }
    bool srem(const std::string& key, const std::string& member) override { (void)key; (void)member; return true; }
    bool smembers(const std::string& key, std::vector<std::string>& out) override { (void)key; out.clear(); return true; }
    bool setex(const std::string& key, const std::string& value, unsigned int ttl_sec) override { (void)key; (void)value; (void)ttl_sec; return true; }
    bool publish(const std::string& channel, const std::string& message) override { (void)channel; (void)message; return true; }
    bool start_psubscribe(const std::string& pattern, std::function<void(const std::string&, const std::string&)> on_message) override { (void)pattern; (void)on_message; return true; }
    void stop_psubscribe() override {}
    bool xgroup_create_mkstream(const std::string& key, const std::string& group) override { (void)key; (void)group; return true; }
    bool xadd(const std::string& key, const std::vector<std::pair<std::string, std::string>>& fields, std::string* out_id, std::optional<std::size_t> maxlen, bool approximate) override { (void)key; (void)fields; (void)maxlen; (void)approximate; if (out_id) *out_id = "0-0"; return true; }
    bool xreadgroup(const std::string& key, const std::string& group, const std::string& consumer, long long block_ms, std::size_t count, std::vector<StreamEntry>& out) override { (void)key; (void)group; (void)consumer; (void)block_ms; (void)count; out.clear(); return true; }
    bool xack(const std::string& /*key*/, const std::string& /*group*/, const std::string& /*id*/) override { return true; }
    bool del(const std::string& key) override { (void)key; return true; }
    std::optional<std::string> get(const std::string& key) override { (void)key; return std::optional<std::string>{}; }
    bool mget(const std::vector<std::string>& keys, std::vector<std::optional<std::string>>& out) override {
        out.assign(keys.size(), std::nullopt);
        return true;
    }
    bool set_if_not_exists(const std::string& key, const std::string& value, unsigned int ttl_sec) override { (void)key; (void)value; (void)ttl_sec; return true; }
    bool set_if_equals(const std::string& key, const std::string& expected, const std::string& value, unsigned int ttl_sec) override { (void)key; (void)expected; (void)value; (void)ttl_sec; return true; }
    bool del_if_equals(const std::string& key, const std::string& expected) override { (void)key; (void)expected; return true; }
    bool scan_keys(const std::string& pattern, std::vector<std::string>& keys) override { (void)pattern; keys.clear(); return true; }
    bool lrange(const std::string& key, long long start, long long stop, std::vector<std::string>& out) override { (void)key; (void)start; (void)stop; out.clear(); return true; }
    bool scan_del(const std::string& pattern) override { (void)pattern; return true; }
    bool xpending(const std::string& key, const std::string& group, long long& total) override { (void)key; (void)group; total = 0; return true; }
    bool xautoclaim(const std::string& key,
                    const std::string& group,
                    const std::string& consumer,
                    long long min_idle_ms,
                    const std::string& start,
                    std::size_t count,
                    StreamAutoClaimResult& out) override {
        (void)key;
        (void)group;
        (void)consumer;
        (void)min_idle_ms;
        (void)count;
        out.next_start_id = start;
        out.entries.clear();
        out.deleted_ids.clear();
        return true;
    }
private:
    std::string uri_;
    Options opts_;
};

// redis++이 가능하면 실제 구현을, 아니면 Stub을 반환한다.
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


