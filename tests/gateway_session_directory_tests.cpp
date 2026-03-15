#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "../gateway/include/gateway/session_directory.hpp"
#include "server/core/storage/redis/client.hpp"

namespace {

class FakeRedisClient final : public server::core::storage::redis::IRedisClient {
public:
    bool health_check() override { return true; }
    bool lpush_trim(const std::string&, const std::string&, std::size_t) override { return true; }
    bool sadd(const std::string&, const std::string&) override { return true; }
    bool srem(const std::string&, const std::string&) override { return true; }
    bool smembers(const std::string&, std::vector<std::string>&) override { return true; }
    bool scard(const std::string&, std::size_t& out) override { out = 0; return true; }
    bool scard_many(const std::vector<std::string>& keys, std::vector<std::size_t>& out) override {
        out.assign(keys.size(), 0);
        return true;
    }
    bool del(const std::string& key) override {
        ++del_calls;
        store_.erase(key);
        return true;
    }
    bool set(const std::string& key, const std::string& value) override {
        store_[key] = value;
        return true;
    }
    std::optional<std::string> get(const std::string& key) override {
        ++get_calls;
        auto it = store_.find(key);
        if (it == store_.end()) {
            return std::nullopt;
        }
        return it->second;
    }
    bool mget(const std::vector<std::string>& keys, std::vector<std::optional<std::string>>& out) override {
        out.clear();
        out.reserve(keys.size());
        for (const auto& key : keys) {
            out.push_back(get(key));
        }
        return true;
    }
    bool set_if_not_exists(const std::string& key, const std::string& value, unsigned int ttl_sec) override {
        ++set_if_not_exists_calls;
        auto [it, inserted] = store_.emplace(key, value);
        if (inserted) {
            ttl_store_[key] = ttl_sec;
            return true;
        }
        return false;
    }
    bool set_if_equals(const std::string& key, const std::string& expected, const std::string& value, unsigned int ttl_sec) override {
        ++set_if_equals_calls;
        auto it = store_.find(key);
        if (it == store_.end() || it->second != expected) {
            return false;
        }
        it->second = value;
        ttl_store_[key] = ttl_sec;
        return true;
    }
    bool del_if_equals(const std::string& key, const std::string& expected) override {
        ++del_if_equals_calls;
        auto it = store_.find(key);
        if (it == store_.end() || it->second != expected) {
            return false;
        }
        store_.erase(it);
        ttl_store_.erase(key);
        return true;
    }
    bool scan_keys(const std::string&, std::vector<std::string>&) override { return true; }
    bool lrange(const std::string&, long long, long long, std::vector<std::string>&) override { return true; }
    bool scan_del(const std::string&) override { return true; }
    bool setex(const std::string& key, const std::string& value, unsigned int ttl_sec) override {
        store_[key] = value;
        ttl_store_[key] = ttl_sec;
        return true;
    }
    bool publish(const std::string&, const std::string&) override { return true; }
    bool start_psubscribe(const std::string&, std::function<void(const std::string&, const std::string&)>) override { return true; }
    void stop_psubscribe() override {}
    bool xgroup_create_mkstream(const std::string&, const std::string&) override { return true; }
    bool xadd(const std::string&, const std::vector<std::pair<std::string, std::string>>&, std::string*, std::optional<std::size_t>, bool) override { return true; }
    bool xreadgroup(const std::string&, const std::string&, const std::string&, long long, std::size_t, std::vector<StreamEntry>&) override { return true; }
    bool xack(const std::string&, const std::string&, const std::string&) override { return true; }
    bool xpending(const std::string&, const std::string&, long long& total) override { total = 0; return true; }
    bool xautoclaim(const std::string&, const std::string&, const std::string&, long long, const std::string& start, std::size_t, StreamAutoClaimResult& out) override {
        out.next_start_id = start;
        out.entries.clear();
        out.deleted_ids.clear();
        return true;
    }

    std::unordered_map<std::string, std::string> store_;
    std::unordered_map<std::string, unsigned int> ttl_store_;
    std::size_t get_calls{0};
    std::size_t set_if_not_exists_calls{0};
    std::size_t set_if_equals_calls{0};
    std::size_t del_calls{0};
    std::size_t del_if_equals_calls{0};
};

TEST(SessionDirectoryTest, FallsBackToLocalCacheWithoutRedis) {
    gateway::SessionDirectory directory(nullptr, "gateway/session", std::chrono::seconds{30});

    auto ensured = directory.ensure_backend("client-1", "backend-a");
    ASSERT_TRUE(ensured.has_value());
    EXPECT_EQ(*ensured, "backend-a");

    auto found = directory.find_backend("client-1");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, "backend-a");

    directory.refresh_backend("client-1", "backend-a");
    directory.release_backend("client-1", "backend-a");
    EXPECT_FALSE(directory.find_backend("client-1").has_value());
}

TEST(SessionDirectoryTest, EnsureBackendUsesExistingRedisMappingAndCachesIt) {
    auto redis = std::make_shared<FakeRedisClient>();
    redis->store_["gateway/session/client-1"] = "backend-existing";

    gateway::SessionDirectory directory(redis, "gateway/session", std::chrono::seconds{60});

    auto ensured = directory.ensure_backend("client-1", "backend-new");
    ASSERT_TRUE(ensured.has_value());
    EXPECT_EQ(*ensured, "backend-existing");
    EXPECT_EQ(redis->set_if_not_exists_calls, 0u);

    const auto get_calls_after_ensure = redis->get_calls;
    auto found = directory.find_backend("client-1");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, "backend-existing");
    EXPECT_EQ(redis->get_calls, get_calls_after_ensure);
}

TEST(SessionDirectoryTest, RefreshAndReleaseRespectExpectedBackend) {
    auto redis = std::make_shared<FakeRedisClient>();
    gateway::SessionDirectory directory(redis, "gateway/session", std::chrono::seconds{45});

    auto ensured = directory.ensure_backend("client-1", "backend-a");
    ASSERT_TRUE(ensured.has_value());
    EXPECT_EQ(redis->ttl_store_["gateway/session/client-1"], 45u);

    directory.refresh_backend("client-1", "backend-a");
    EXPECT_EQ(redis->set_if_equals_calls, 1u);

    directory.release_backend("client-1", "backend-b");
    EXPECT_EQ(redis->del_if_equals_calls, 1u);
    ASSERT_TRUE(directory.find_backend("client-1").has_value());

    directory.release_backend("client-1", "backend-a");
    EXPECT_EQ(redis->del_if_equals_calls, 2u);
    EXPECT_FALSE(directory.find_backend("client-1").has_value());
}

} // namespace
