#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>

#include "server/chat/chat_service.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "server/storage/redis/client.hpp"
#include "wire.pb.h"

#include <deque>
#include <optional>
#include <unordered_map>

/**
 * @brief ChatService 최근 메시지 캐시 적재/조회 동작을 검증하는 테스트 파일입니다.
 */
namespace server::app::chat {
struct ChatServiceHistoryTester {
    static void OverrideHistoryConfig(ChatService& svc, std::size_t limit, std::size_t max_list) {
        svc.history_.recent_limit = limit;
        svc.history_.max_list_len = max_list;
    }

    static bool Cache(ChatService& svc,
                      const std::string& room_id,
                      const server::wire::v1::StateSnapshot::SnapshotMessage& msg) {
        return svc.cache_recent_message(room_id, msg);
    }

    static bool Load(ChatService& svc,
                     const std::string& room_id,
                     std::vector<server::wire::v1::StateSnapshot::SnapshotMessage>& out) {
        return svc.load_recent_messages_from_cache(room_id, out);
    }
};
} // namespace server::app::chat

class MockRedisClient : public server::storage::redis::IRedisClient {
public:
    bool health_check() override { return true; }
    bool lpush_trim(const std::string& key, const std::string& value, std::size_t maxlen) override {
        auto& list = lists_[key];
        list.push_front(value);
        if (maxlen > 0 && list.size() > maxlen) {
            list.resize(maxlen);
        }
        return true;
    }
    bool sadd(const std::string&, const std::string&) override { return true; }
    bool srem(const std::string&, const std::string&) override { return true; }
    bool smembers(const std::string&, std::vector<std::string>&) override { return true; }
    bool scard(const std::string&, std::size_t& out) override { out = 0; return true; }
    bool del(const std::string& key) override {
        kv_.erase(key);
        lists_.erase(key);
        return true;
    }
    std::optional<std::string> get(const std::string& key) override {
        auto it = kv_.find(key);
        if (it == kv_.end()) return std::nullopt;
        return it->second;
    }
    bool mget(const std::vector<std::string>& keys, std::vector<std::optional<std::string>>& out) override {
        out.clear();
        out.reserve(keys.size());
        for (const auto& key : keys) {
            auto it = kv_.find(key);
            if (it == kv_.end()) {
                out.emplace_back(std::nullopt);
            } else {
                out.emplace_back(it->second);
            }
        }
        return true;
    }
    bool set_if_not_exists(const std::string&, const std::string&, unsigned int) override { return true; }
    bool set_if_equals(const std::string&, const std::string&, const std::string&, unsigned int) override { return true; }
    bool del_if_equals(const std::string&, const std::string&) override { return true; }
    bool scan_keys(const std::string&, std::vector<std::string>&) override { return true; }
    bool lrange(const std::string& key, long long start, long long stop, std::vector<std::string>& out) override {
        out.clear();
        auto it = lists_.find(key);
        if (it == lists_.end()) return true;
        const auto& list = it->second;
        if (list.empty()) return true;
        auto normalize = [&](long long index) -> long long {
            if (index < 0) {
                index = static_cast<long long>(list.size()) + index;
            }
            if (index < 0) index = 0;
            if (index >= static_cast<long long>(list.size())) index = static_cast<long long>(list.size()) - 1;
            return index;
        };
        long long begin = normalize(start);
        long long end = normalize(stop);
        if (begin > end) return true;
        for (long long idx = begin; idx <= end; ++idx) {
            out.push_back(list[static_cast<std::size_t>(idx)]);
        }
        return true;
    }
    bool scan_del(const std::string&) override { return true; }
    bool setex(const std::string& key, const std::string& value, unsigned int) override {
        kv_[key] = value;
        return true;
    }
    bool publish(const std::string&, const std::string&) override { return true; }
    bool start_psubscribe(const std::string&,
                          std::function<void(const std::string&, const std::string&)>) override { return true; }
    void stop_psubscribe() override {}
    bool xgroup_create_mkstream(const std::string&, const std::string&) override { return true; }
    bool xadd(const std::string&,
              const std::vector<std::pair<std::string, std::string>>&,
              std::string*,
              std::optional<std::size_t>,
              bool) override { return true; }
    bool xreadgroup(const std::string&, const std::string&, const std::string&,
                    long long, std::size_t, std::vector<StreamEntry>&) override { return true; }
    bool xack(const std::string&, const std::string&, const std::string&) override { return true; }
    bool xpending(const std::string&, const std::string&, long long& total) override { total = 0; return true; }
    bool xautoclaim(const std::string&,
                    const std::string&,
                    const std::string&,
                    long long,
                    const std::string& start,
                    std::size_t,
                    StreamAutoClaimResult& out) override {
        out.next_start_id = start;
        out.entries.clear();
        out.deleted_ids.clear();
        return true;
    }

    void erase_payload(const std::string& key) { kv_.erase(key); }

private:
    std::unordered_map<std::string, std::string> kv_;
    std::unordered_map<std::string, std::deque<std::string>> lists_;
};

// 채팅 기록 캐시(Redis) 동작을 검증하는 테스트입니다.
// MockRedisClient를 사용하여 실제 Redis 없이 로직을 테스트합니다.
class ChatHistoryCacheTest : public ::testing::Test {
protected:
    boost::asio::io_context io_;
    server::core::JobQueue job_queue_;
    std::shared_ptr<MockRedisClient> redis_ = std::make_shared<MockRedisClient>();
    std::unique_ptr<server::app::chat::ChatService> service_;

    void SetUp() override {
        // 테스트 전 ChatService를 초기화하고, 히스토리 설정을 오버라이드합니다.
        service_ = std::make_unique<server::app::chat::ChatService>(io_, job_queue_, nullptr, redis_);
        server::app::chat::ChatServiceHistoryTester::OverrideHistoryConfig(*service_, 3, 16);
    }

    // 테스트용 스냅샷 메시지 생성 헬퍼
    static server::wire::v1::StateSnapshot::SnapshotMessage MakeMessage(std::uint64_t id, const std::string& sender) {
        server::wire::v1::StateSnapshot::SnapshotMessage msg;
        msg.set_id(id);
        msg.set_sender(sender);
        msg.set_text("hello-" + std::to_string(id));
        msg.set_ts_ms(id * 1000);
        return msg;
    }
};

// 캐시에 저장된 모든 메시지가 정상적으로 로드되는지 확인합니다.
TEST_F(ChatHistoryCacheTest, LoadsAllMessagesFromCache) {
    auto msg1 = MakeMessage(1, "alice");
    auto msg2 = MakeMessage(2, "bob");
    auto msg3 = MakeMessage(3, "carol");

    // 3개의 메시지를 캐시에 저장
    ASSERT_TRUE(server::app::chat::ChatServiceHistoryTester::Cache(*service_, "room-1", msg1));
    ASSERT_TRUE(server::app::chat::ChatServiceHistoryTester::Cache(*service_, "room-1", msg2));
    ASSERT_TRUE(server::app::chat::ChatServiceHistoryTester::Cache(*service_, "room-1", msg3));

    // 캐시에서 로드하여 개수와 마지막 메시지 ID 확인
    std::vector<server::wire::v1::StateSnapshot::SnapshotMessage> loaded;
    ASSERT_TRUE(server::app::chat::ChatServiceHistoryTester::Load(*service_, "room-1", loaded));
    EXPECT_EQ(loaded.size(), 3u);
    EXPECT_EQ(loaded.back().id(), 3u);
}

TEST_F(ChatHistoryCacheTest, ReturnsPartialWhenPayloadMissing) {
    auto msg1 = MakeMessage(10, "alice");
    auto msg2 = MakeMessage(11, "bob");

    ASSERT_TRUE(server::app::chat::ChatServiceHistoryTester::Cache(*service_, "room-42", msg1));
    ASSERT_TRUE(server::app::chat::ChatServiceHistoryTester::Cache(*service_, "room-42", msg2));

    redis_->erase_payload("msg:11");

    std::vector<server::wire::v1::StateSnapshot::SnapshotMessage> loaded;
    ASSERT_TRUE(server::app::chat::ChatServiceHistoryTester::Load(*service_, "room-42", loaded));
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded.front().id(), 10u);
}
