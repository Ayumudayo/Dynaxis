#include <chrono>
#include <unordered_map>

#include <gtest/gtest.h>

#include "server/state/instance_registry.hpp"

using namespace server::state;

/**
 * @brief 인스턴스 레지스트리(in-memory/Redis/Consul adapter) 동작을 검증합니다.
 */
namespace {

InstanceRecord sample_record() {
    InstanceRecord record;
    record.instance_id = "core-1";
    record.host = "127.0.0.1";
    record.port = 7000;
    record.role = "chat";
    record.capacity = 100;
    record.active_sessions = 12;
    record.ready = true;
    record.last_heartbeat_ms = 42;
    return record;
}

class FakeRedisClient final : public RedisInstanceStateBackend::IRedisClient {
public:
    bool setex(const std::string& key, const std::string& value, unsigned int ttl_sec) override {
        last_key = key;
        last_value = value;
        last_ttl = ttl_sec;
        ++setex_calls;
        store_[key] = value;
        return true;
    }

    bool del(const std::string& key) override {
        last_key = key;
        ++del_calls;
        store_.erase(key);
        return true;
    }

    bool scan_keys(const std::string& pattern, std::vector<std::string>& keys) override {
        keys.clear();
        std::string prefix = pattern;
        auto wildcard = prefix.find('*');
        if (wildcard != std::string::npos) {
            prefix.resize(wildcard);
        }
        for (const auto& [k, _] : store_) {
            if (k.rfind(prefix, 0) == 0) {
                keys.push_back(k);
            }
        }
        return true;
    }

    std::optional<std::string> get(const std::string& key) override {
        auto it = store_.find(key);
        if (it == store_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    bool mget(const std::vector<std::string>& keys,
              std::vector<std::optional<std::string>>& out) override {
        out.clear();
        out.reserve(keys.size());
        for (const auto& key : keys) {
            auto it = store_.find(key);
            if (it == store_.end()) {
                out.push_back(std::nullopt);
            } else {
                out.push_back(it->second);
            }
        }
        return true;
    }

    std::string last_key;
    std::string last_value;
    unsigned int last_ttl{0};
    std::size_t setex_calls{0};
    std::size_t del_calls{0};

private:
    std::unordered_map<std::string, std::string> store_;
};

} // namespace

// InMemory 백엔드에서 인스턴스 등록 및 조회가 정상 동작하는지 확인합니다.
TEST(InMemoryStateBackendTests, UpsertAndList) {
    InMemoryStateBackend backend;
    auto record = sample_record();
    EXPECT_TRUE(backend.upsert(record));
    auto all = backend.list_instances();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all.front().instance_id, "core-1");
}

TEST(InMemoryStateBackendTests, TouchUpdatesHeartbeat) {
    InMemoryStateBackend backend;
    auto record = sample_record();
    backend.upsert(record);
    EXPECT_TRUE(backend.touch("core-1", 999));
    auto all = backend.list_instances();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all.front().last_heartbeat_ms, 999u);
}

// Redis 백엔드가 SETEX 명령어를 사용하여 TTL과 함께 인스턴스를 저장하는지 확인합니다.
TEST(RedisInstanceStateBackendTests, PersistsWithSetex) {
    auto client = std::make_shared<FakeRedisClient>();
    RedisInstanceStateBackend backend(client, "gateway/test", std::chrono::seconds{5});
    auto record = sample_record();
    record.ready = false;

    // Upsert 시 SETEX 호출 확인
    EXPECT_TRUE(backend.upsert(record));
    EXPECT_EQ(client->setex_calls, 1u);
    EXPECT_NE(client->last_key.find("gateway/test/"), std::string::npos);
    EXPECT_NE(client->last_value.find("\"ready\":false"), std::string::npos);

    // Touch 시에도 SETEX 호출되어 TTL 갱신 확인
    EXPECT_TRUE(backend.touch("core-1", 1234));
    EXPECT_EQ(client->setex_calls, 2u);
    EXPECT_EQ(client->last_ttl, 5u);

    auto all = backend.list_instances();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all.front().last_heartbeat_ms, 1234u);
    EXPECT_FALSE(all.front().ready);

    // Remove 시 DEL 호출 확인
    EXPECT_TRUE(backend.remove("core-1"));
    EXPECT_EQ(client->del_calls, 1u);
}

TEST(InstanceRegistryJsonTests, MissingReadyDefaultsToTrue) {
    auto parsed = detail::deserialize_json(
        "{\"instance_id\":\"legacy\",\"host\":\"127.0.0.1\",\"port\":7000,\"role\":\"chat\",\"capacity\":10,\"active_sessions\":1,\"last_heartbeat_ms\":123}");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->ready);
}

TEST(InstanceRegistryJsonTests, ParsesExplicitReadyFalse) {
    auto parsed = detail::deserialize_json(
        "{\"instance_id\":\"s1\",\"host\":\"127.0.0.1\",\"port\":7000,\"role\":\"chat\",\"capacity\":10,\"active_sessions\":1,\"ready\":false,\"last_heartbeat_ms\":123}");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(parsed->ready);
}

TEST(ConsulInstanceStateBackendTests, UsesCallbacks) {
    std::vector<std::string> put_paths;
    std::vector<std::string> delete_paths;

    ConsulInstanceStateBackend backend(
        "v1/kv/gateway",
        [&](const std::string& path, const std::string& payload) {
            put_paths.push_back(path + ":" + payload);
            return true;
        },
        [&](const std::string& path, const std::string&) {
            delete_paths.push_back(path);
            return true;
        });

    auto record = sample_record();
    EXPECT_TRUE(backend.upsert(record));
    EXPECT_TRUE(backend.touch("core-1", 888));
    EXPECT_TRUE(backend.remove("core-1"));

    EXPECT_EQ(put_paths.size(), 2u);
    EXPECT_EQ(delete_paths.size(), 1u);
}
