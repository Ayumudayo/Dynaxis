#include <chrono>

#include <gtest/gtest.h>

#include "server/state/instance_registry.hpp"

using namespace server::state;

namespace {

InstanceRecord sample_record() {
    InstanceRecord record;
    record.instance_id = "core-1";
    record.host = "127.0.0.1";
    record.port = 7000;
    record.role = "chat";
    record.capacity = 100;
    record.active_sessions = 12;
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
        return true;
    }

    bool del(const std::string& key) override {
        last_key = key;
        ++del_calls;
        return true;
    }

    std::string last_key;
    std::string last_value;
    unsigned int last_ttl{0};
    std::size_t setex_calls{0};
    std::size_t del_calls{0};
};

} // namespace

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

TEST(RedisInstanceStateBackendTests, PersistsWithSetex) {
    auto client = std::make_shared<FakeRedisClient>();
    RedisInstanceStateBackend backend(client, "gateway/test", std::chrono::seconds{5});
    auto record = sample_record();

    EXPECT_TRUE(backend.upsert(record));
    EXPECT_EQ(client->setex_calls, 1u);
    EXPECT_NE(client->last_key.find("gateway/test/"), std::string::npos);

    EXPECT_TRUE(backend.touch("core-1", 1234));
    EXPECT_EQ(client->setex_calls, 2u);
    EXPECT_EQ(client->last_ttl, 5u);

    auto all = backend.list_instances();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all.front().last_heartbeat_ms, 1234u);

    EXPECT_TRUE(backend.remove("core-1"));
    EXPECT_EQ(client->del_calls, 1u);
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
