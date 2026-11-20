#include <gtest/gtest.h>
#include <server/core/net/connection.hpp>
#include <server/core/net/hive.hpp>

#include <memory>
#include <thread>
#include <vector>

namespace {
class BudgetConnection final : public server::core::net::Connection {
public:
    using Connection::Connection;
    std::size_t errors{0};
    std::size_t disconnects{0};
    void on_error(const boost::system::error_code& ec) override {
        if (ec == boost::asio::error::no_buffer_space) {
            ++errors;
        }
    }
    void on_disconnect() override { ++disconnects; }
};
}

TEST(CoreTransportBudgetTest, SlowExecutorDoesNotRetainUnboundedSendRequests) {
    boost::asio::io_context io;
    auto hive = std::make_shared<server::core::net::Hive>(io);
    auto connection = std::make_shared<BudgetConnection>(hive, 1024);
    for (int i = 0; i < 1000; ++i) {
        connection->async_send(std::vector<std::uint8_t>(1024, 7));
    }
    // executor가 아직 돌지 않아도 pending payload와 overflow 알림만 connection을 보유해야 한다.
    // 기존 구현에서는 1000개 요청 각각이 connection과 1KiB payload를 붙잡았다.
    EXPECT_LE(connection.use_count(), 4);
    io.poll();
    EXPECT_TRUE(connection->is_stopped());
    EXPECT_EQ(connection->errors, 1u);
    EXPECT_EQ(connection->disconnects, 1u);
}

TEST(CoreTransportBudgetTest, StopReleasesExecutorOwnedConnectionAfterDrain) {
    boost::asio::io_context io;
    auto hive = std::make_shared<server::core::net::Hive>(io);
    auto connection = std::make_shared<BudgetConnection>(hive, 16);
    std::weak_ptr<BudgetConnection> weak = connection;
    connection->async_send(std::vector<std::uint8_t>(8));
    connection->stop();
    connection.reset();
    io.poll();
    EXPECT_TRUE(weak.expired());
}

TEST(CoreTransportBudgetTest, ConcurrentProducersShareOneExecutorBudget) {
    boost::asio::io_context io;
    auto hive = std::make_shared<server::core::net::Hive>(io);
    auto connection = std::make_shared<BudgetConnection>(hive, 4096);
    std::vector<std::thread> producers;
    for (int i = 0; i < 4; ++i) {
        producers.emplace_back([connection] {
            for (int n = 0; n < 250; ++n) {
                connection->async_send(std::vector<std::uint8_t>(1024, 7));
            }
        });
    }
    for (auto& producer : producers) { producer.join(); }
    EXPECT_LE(connection.use_count(), 7);
    io.poll();
    EXPECT_EQ(connection->errors, 1u);
    EXPECT_EQ(connection->disconnects, 1u);
}
