#include <gtest/gtest.h>

#include <server/core/config/options.hpp>
#include <server/core/memory/memory_pool.hpp>
#include <server/core/net/dispatcher.hpp>
#include <server/core/net/hive.hpp>
#include <server/core/net/connection_runtime_state.hpp>
#include <server/core/net/session.hpp>
#include <server/protocol/game_opcodes.hpp>

#include <memory>
#include <vector>

/**
 * @brief core 네트워크 계층(Dispatcher/Hive) 기본 수명주기 동작을 검증합니다.
 */
using namespace server::core;
using namespace server::core::net;

TEST(DispatcherTest, RegisterAndDispatch) {
    boost::asio::io_context io;
    Dispatcher dispatcher;
    BufferManager buffer_manager(2048, 8);
    auto options = std::make_shared<SessionOptions>();
    auto state = std::make_shared<server::core::net::ConnectionRuntimeState>();

    Session session(
        boost::asio::ip::tcp::socket(io),
        dispatcher,
        buffer_manager,
        options,
        state
    );

    bool called = false;
    std::vector<std::uint8_t> observed_payload;
    const std::uint16_t msg_id = 1001;
    dispatcher.register_handler(msg_id, [&](Session&, std::span<const std::uint8_t> payload) {
        called = true;
        observed_payload.assign(payload.begin(), payload.end());
    });

    const std::vector<std::uint8_t> payload{1, 2, 3, 4};
    EXPECT_TRUE(dispatcher.dispatch(msg_id, session, payload));
    EXPECT_TRUE(called);
    EXPECT_EQ(observed_payload, payload);

    EXPECT_FALSE(dispatcher.dispatch(9999, session, payload));
}

TEST(DispatcherTest, BlocksHandlerWhenRequiredStateNotSatisfied) {
    boost::asio::io_context io;
    Dispatcher dispatcher;
    BufferManager buffer_manager(2048, 8);
    auto options = std::make_shared<SessionOptions>();
    auto state = std::make_shared<server::core::net::ConnectionRuntimeState>();

    Session session(
        boost::asio::ip::tcp::socket(io),
        dispatcher,
        buffer_manager,
        options,
        state
    );

    bool called = false;
    server::core::protocol::OpcodePolicy policy{};
    policy.required_state = server::core::protocol::SessionStatus::kAuthenticated;

    const std::uint16_t msg_id = 2001;
    dispatcher.register_handler(msg_id, [&](Session&, std::span<const std::uint8_t>) {
        called = true;
    }, policy);

    const std::vector<std::uint8_t> payload{9, 8, 7};
    EXPECT_TRUE(dispatcher.dispatch(msg_id, session, payload));
    EXPECT_FALSE(called);

    session.set_session_status(server::core::protocol::SessionStatus::kAuthenticated);
    EXPECT_TRUE(dispatcher.dispatch(msg_id, session, payload));
    EXPECT_TRUE(called);
}

TEST(DispatcherTest, BlocksHandlerWhenTransportNotAllowed) {
    boost::asio::io_context io;
    Dispatcher dispatcher;
    BufferManager buffer_manager(2048, 8);
    auto options = std::make_shared<SessionOptions>();
    auto state = std::make_shared<server::core::net::ConnectionRuntimeState>();

    Session session(
        boost::asio::ip::tcp::socket(io),
        dispatcher,
        buffer_manager,
        options,
        state
    );

    bool called = false;
    server::core::protocol::OpcodePolicy policy{};
    policy.transport = server::core::protocol::TransportMask::kUdp;

    const std::uint16_t msg_id = 2002;
    dispatcher.register_handler(msg_id, [&](Session&, std::span<const std::uint8_t>) {
        called = true;
    }, policy);

    const std::vector<std::uint8_t> payload{1, 1, 2, 3};

    EXPECT_TRUE(dispatcher.dispatch(msg_id, session, payload));
    EXPECT_FALSE(called);

    EXPECT_TRUE(dispatcher.dispatch(msg_id, session, payload, server::core::protocol::TransportKind::kUdp));
    EXPECT_TRUE(called);
}

TEST(HiveTest, Lifecycle) {
    boost::asio::io_context io;
    Hive hive(io);

    EXPECT_FALSE(hive.is_stopped());

    hive.stop();
    EXPECT_TRUE(hive.is_stopped());
}

TEST(OpcodePolicyTest, UdpBindOpcodePoliciesAreExpected) {
    using server::core::protocol::DeliveryClass;
    using server::core::protocol::SessionStatus;
    using server::core::protocol::TransportKind;
    using server::core::protocol::TransportMask;

    const auto bind_req_policy = server::protocol::opcode_policy(server::protocol::MSG_UDP_BIND_REQ);
    EXPECT_EQ(bind_req_policy.required_state, SessionStatus::kAny);
    EXPECT_EQ(bind_req_policy.transport, TransportMask::kUdp);
    EXPECT_EQ(bind_req_policy.delivery, DeliveryClass::kReliable);
    EXPECT_FALSE(server::core::protocol::transport_allows(bind_req_policy.transport, TransportKind::kTcp));
    EXPECT_TRUE(server::core::protocol::transport_allows(bind_req_policy.transport, TransportKind::kUdp));

    const auto bind_res_policy = server::protocol::opcode_policy(server::protocol::MSG_UDP_BIND_RES);
    EXPECT_EQ(bind_res_policy.required_state, SessionStatus::kAny);
    EXPECT_EQ(bind_res_policy.transport, TransportMask::kBoth);
    EXPECT_EQ(bind_res_policy.delivery, DeliveryClass::kReliable);
    EXPECT_TRUE(server::core::protocol::transport_allows(bind_res_policy.transport, TransportKind::kTcp));
    EXPECT_TRUE(server::core::protocol::transport_allows(bind_res_policy.transport, TransportKind::kUdp));
}
