#include <gtest/gtest.h>

#include <server/core/config/options.hpp>
#include <server/core/concurrent/job_queue.hpp>
#include <server/core/memory/memory_pool.hpp>
#include <server/core/net/connection.hpp>
#include <server/core/net/dispatcher.hpp>
#include <server/core/net/hive.hpp>
#include <server/core/net/connection_runtime_state.hpp>
#include <server/core/net/queue_budget.hpp>
#include <server/core/net/session.hpp>
#include <server/core/net/transport_router.hpp>
#include <server/core/protocol/packet.hpp>
#include <server/core/protocol/protocol_errors.hpp>
#include <server/core/protocol/system_opcodes.hpp>
#include <server/core/protocol/version.hpp>
#include <server/core/runtime_metrics.hpp>
#include <server/core/util/service_registry.hpp>
#include <server/protocol/game_opcodes.hpp>
#include <gateway/gateway_app.hpp>
#include <gateway/transport_session.hpp>
#include "gateway_backend_connection.hpp"

#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

/**
 * @brief core 네트워크 계층(Dispatcher/Hive) 기본 수명주기 동작을 검증합니다.
 */
using namespace server::core;
using namespace server::core::net;
namespace services = server::core::util::services;

namespace {

class ObservingConnection : public Connection {
public:
    ObservingConnection(std::shared_ptr<Hive> hive, std::size_t send_queue_max_bytes)
        : Connection(std::move(hive), send_queue_max_bytes) {}

    std::optional<boost::system::error_code> last_error;
    std::size_t disconnect_count{0};

protected:
    void on_error(const boost::system::error_code& ec) override {
        last_error = ec;
    }

    void on_disconnect() override {
        ++disconnect_count;
    }
};

class TestTransportSession final : public server::core::net::ITransportSession {
public:
    server::core::protocol::SessionStatus status{
        server::core::protocol::SessionStatus::kAny};
    bool accept_serialized_posts{true};
    std::vector<std::pair<std::uint16_t, std::string>> errors;
    std::deque<std::function<void()>> serialized_tasks;

    server::core::protocol::SessionStatus session_status() const noexcept override {
        return status;
    }

    bool post_serialized(std::function<void()> fn) override {
        if (!accept_serialized_posts) {
            return false;
        }
        serialized_tasks.push_back(std::move(fn));
        return true;
    }

    void send_error(std::uint16_t code, std::string_view message) override {
        errors.emplace_back(code, std::string(message));
    }

    void drain_serialized() {
        while (!serialized_tasks.empty()) {
            auto task = std::move(serialized_tasks.front());
            serialized_tasks.pop_front();
            task();
        }
    }
};

} // namespace

TEST(GatewayTransportAbstractionTest, BackendConnectionImplementsTransportSession) {
    EXPECT_TRUE((std::is_base_of_v<gateway::ITransportSession,
                                   gateway::BackendConnection>));
}

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

TEST(DispatcherTest, AllowsHandlerWhenTransportAllowsBothTcpAndUdp) {
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

    std::size_t called = 0;
    server::core::protocol::OpcodePolicy policy{};
    policy.transport = server::core::protocol::TransportMask::kBoth;

    const std::uint16_t msg_id = 2003;
    dispatcher.register_handler(msg_id, [&](Session&, std::span<const std::uint8_t>) {
        ++called;
    }, policy);

    const std::vector<std::uint8_t> payload{7, 7, 7};

    EXPECT_TRUE(dispatcher.dispatch(msg_id, session, payload));
    EXPECT_TRUE(dispatcher.dispatch(msg_id, session, payload, server::core::protocol::TransportKind::kUdp));
    EXPECT_EQ(called, 2u);
}

TEST(DispatcherTest, WorkerProcessingPlaceQueuesAndRunsViaJobQueue) {
    boost::asio::io_context io;
    Dispatcher dispatcher;
    BufferManager buffer_manager(2048, 8);
    auto options = std::make_shared<SessionOptions>();
    auto state = std::make_shared<server::core::net::ConnectionRuntimeState>();
    auto session = std::make_shared<Session>(
        boost::asio::ip::tcp::socket(io),
        dispatcher,
        buffer_manager,
        options,
        state);

    auto job_queue = std::make_shared<JobQueue>(16);
    services::set(job_queue);

    bool called = false;
    server::core::protocol::OpcodePolicy policy{};
    policy.processing_place = server::core::protocol::ProcessingPlace::kWorker;
    const std::uint16_t msg_id = 3001;
    dispatcher.register_handler(msg_id, [&](Session&, std::span<const std::uint8_t>) {
        called = true;
    }, policy);

    const auto before = server::core::runtime_metrics::snapshot();
    EXPECT_TRUE(dispatcher.dispatch(msg_id, *session, std::vector<std::uint8_t>{1, 2, 3}));
    EXPECT_FALSE(called);

    auto job = job_queue->Pop();
    ASSERT_TRUE(static_cast<bool>(job));
    job();
    io.poll();
    EXPECT_TRUE(called);

    const auto after = server::core::runtime_metrics::snapshot();
    EXPECT_EQ(after.dispatch_processing_place_calls_total[1],
              before.dispatch_processing_place_calls_total[1] + 1);
    EXPECT_EQ(after.dispatch_processing_place_reject_total[1],
              before.dispatch_processing_place_reject_total[1]);

    job_queue->Stop();
    services::clear();
}

TEST(DispatcherTest, WorkerProcessingPlaceRejectsWhenQueueUnavailable) {
    boost::asio::io_context io;
    Dispatcher dispatcher;
    BufferManager buffer_manager(2048, 8);
    auto options = std::make_shared<SessionOptions>();
    auto state = std::make_shared<server::core::net::ConnectionRuntimeState>();
    auto session = std::make_shared<Session>(
        boost::asio::ip::tcp::socket(io),
        dispatcher,
        buffer_manager,
        options,
        state);

    services::clear();

    bool called = false;
    server::core::protocol::OpcodePolicy policy{};
    policy.processing_place = server::core::protocol::ProcessingPlace::kWorker;
    const std::uint16_t msg_id = 3002;
    dispatcher.register_handler(msg_id, [&](Session&, std::span<const std::uint8_t>) {
        called = true;
    }, policy);

    const auto before = server::core::runtime_metrics::snapshot();
    EXPECT_TRUE(dispatcher.dispatch(msg_id, *session, std::vector<std::uint8_t>{1}));
    EXPECT_FALSE(called);

    const auto after = server::core::runtime_metrics::snapshot();
    EXPECT_EQ(after.dispatch_processing_place_calls_total[1],
              before.dispatch_processing_place_calls_total[1] + 1);
    EXPECT_EQ(after.dispatch_processing_place_reject_total[1],
              before.dispatch_processing_place_reject_total[1] + 1);
}

TEST(DispatcherTest, RoomStrandProcessingPlacePostsToSessionContext) {
    boost::asio::io_context io;
    Dispatcher dispatcher;
    BufferManager buffer_manager(2048, 8);
    auto options = std::make_shared<SessionOptions>();
    auto state = std::make_shared<server::core::net::ConnectionRuntimeState>();
    auto session = std::make_shared<Session>(
        boost::asio::ip::tcp::socket(io),
        dispatcher,
        buffer_manager,
        options,
        state);

    bool called = false;
    server::core::protocol::OpcodePolicy policy{};
    policy.processing_place = server::core::protocol::ProcessingPlace::kRoomStrand;
    const std::uint16_t msg_id = 3003;
    dispatcher.register_handler(msg_id, [&](Session&, std::span<const std::uint8_t>) {
        called = true;
    }, policy);

    const auto before = server::core::runtime_metrics::snapshot();
    EXPECT_TRUE(dispatcher.dispatch(msg_id, *session, std::vector<std::uint8_t>{9, 9}));
    EXPECT_FALSE(called);

    io.poll();
    EXPECT_TRUE(called);

    const auto after = server::core::runtime_metrics::snapshot();
    EXPECT_EQ(after.dispatch_processing_place_calls_total[2],
              before.dispatch_processing_place_calls_total[2] + 1);
    EXPECT_EQ(after.dispatch_processing_place_reject_total[2],
              before.dispatch_processing_place_reject_total[2]);
}

TEST(DispatcherTest, InvalidProcessingPlaceIsRejected) {
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
    policy.processing_place = static_cast<server::core::protocol::ProcessingPlace>(255);
    const std::uint16_t msg_id = 3004;
    dispatcher.register_handler(msg_id, [&](Session&, std::span<const std::uint8_t>) {
        called = true;
    }, policy);

    const auto before = server::core::runtime_metrics::snapshot();
    EXPECT_TRUE(dispatcher.dispatch(msg_id, session, std::vector<std::uint8_t>{4, 2}));
    EXPECT_FALSE(called);

    const auto after = server::core::runtime_metrics::snapshot();
    EXPECT_EQ(after.dispatch_processing_place_calls_total[0], before.dispatch_processing_place_calls_total[0]);
    EXPECT_EQ(after.dispatch_processing_place_calls_total[1], before.dispatch_processing_place_calls_total[1]);
    EXPECT_EQ(after.dispatch_processing_place_calls_total[2], before.dispatch_processing_place_calls_total[2]);
}

TEST(TransportRouterTest, RegisterAndDispatchWithoutPacketSession) {
    server::core::net::TransportRouter router;
    auto session = std::make_shared<TestTransportSession>();

    bool called = false;
    std::vector<std::uint8_t> observed_payload;
    router.register_handler(
        4101,
        [&](server::core::net::ITransportSession& routed_session,
            std::span<const std::uint8_t> payload) {
            called = &routed_session == session.get();
            observed_payload.assign(payload.begin(), payload.end());
        });

    const std::vector<std::uint8_t> payload{4, 1, 0, 1};
    EXPECT_TRUE(router.dispatch(4101, session, payload));
    EXPECT_TRUE(called);
    EXPECT_EQ(observed_payload, payload);
    EXPECT_FALSE(router.dispatch(4999, session, payload));
}

TEST(TransportRouterTest, HonorsPolicyAndProcessingPlaceWithoutPacketSession) {
    server::core::net::TransportRouter router;
    auto session = std::make_shared<TestTransportSession>();

    bool called = false;
    server::core::protocol::OpcodePolicy policy{};
    policy.required_state = server::core::protocol::SessionStatus::kAuthenticated;
    policy.transport = server::core::protocol::TransportMask::kUdp;
    policy.processing_place = server::core::protocol::ProcessingPlace::kWorker;
    router.register_handler(
        4102,
        [&](server::core::net::ITransportSession&, std::span<const std::uint8_t>) {
            called = true;
        },
        policy);

    EXPECT_TRUE(router.dispatch(4102, session, std::vector<std::uint8_t>{1, 2, 3}));
    ASSERT_EQ(session->errors.size(), 1u);
    EXPECT_EQ(session->errors.front().first, server::core::protocol::errc::FORBIDDEN);
    EXPECT_FALSE(called);

    session->errors.clear();
    session->status = server::core::protocol::SessionStatus::kAuthenticated;

    auto job_queue = std::make_shared<JobQueue>(16);
    services::set(job_queue);
    EXPECT_TRUE(router.dispatch(
        4102,
        session,
        std::vector<std::uint8_t>{4, 1, 0, 2},
        server::core::protocol::TransportKind::kUdp));
    EXPECT_FALSE(called);

    auto job = job_queue->Pop();
    ASSERT_TRUE(static_cast<bool>(job));
    job();
    EXPECT_FALSE(called);

    session->drain_serialized();
    EXPECT_TRUE(called);

    job_queue->Stop();
    services::clear();
}

TEST(SessionTest, HelloPayloadVersionContract) {
    boost::asio::io_context io;
    Dispatcher dispatcher;
    BufferManager buffer_manager(2048, 8);
    auto options = std::make_shared<SessionOptions>();
    auto state = std::make_shared<server::core::net::ConnectionRuntimeState>();

    boost::asio::ip::tcp::acceptor acceptor(
        io,
        boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));

    boost::asio::ip::tcp::socket session_socket(io);
    boost::asio::ip::tcp::socket peer_socket(io);
    peer_socket.connect(acceptor.local_endpoint());
    acceptor.accept(session_socket);

    auto session = std::make_shared<Session>(
        std::move(session_socket),
        dispatcher,
        buffer_manager,
        options,
        state);

    session->start();

    boost::system::error_code ec;
    bool has_data = false;
    for (int i = 0; i < 40; ++i) {
        io.poll();
        if (peer_socket.available(ec) >= server::core::protocol::k_header_bytes) {
            has_data = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(has_data);

    std::array<std::uint8_t, server::core::protocol::k_header_bytes> header_buf{};
    boost::asio::read(peer_socket, boost::asio::buffer(header_buf), ec);
    ASSERT_FALSE(ec);

    server::core::protocol::PacketHeader header{};
    server::core::protocol::decode_header(header_buf.data(), header);
    EXPECT_EQ(header.msg_id, server::core::protocol::MSG_HELLO);
    EXPECT_EQ(header.length, 12);

    std::vector<std::uint8_t> payload(header.length, 0);
    boost::asio::read(peer_socket, boost::asio::buffer(payload), ec);
    ASSERT_FALSE(ec);

    EXPECT_EQ(server::core::protocol::read_be16(payload.data()),
              server::core::protocol::kProtocolVersionMajor);
    EXPECT_EQ(server::core::protocol::read_be16(payload.data() + 2),
              server::core::protocol::kProtocolVersionMinor);
}

TEST(HiveTest, Lifecycle) {
    boost::asio::io_context io;
    Hive hive(io);

    EXPECT_FALSE(hive.is_stopped());

    hive.stop();
    EXPECT_TRUE(hive.is_stopped());
}

TEST(QueueBudgetTest, HandlesUnlimitedExactFitAndOverflowCases) {
    EXPECT_FALSE(server::core::net::exceeds_queue_budget(0, 4096, 8192));
    EXPECT_FALSE(server::core::net::exceeds_queue_budget(16, 0, 16));
    EXPECT_FALSE(server::core::net::exceeds_queue_budget(16, 4, 12));
    EXPECT_TRUE(server::core::net::exceeds_queue_budget(16, 5, 12));
    EXPECT_TRUE(server::core::net::exceeds_queue_budget(16, 0, 17));
}

TEST(ConnectionBackpressureTest, OversizedSendStopsConnectionWithNoBufferSpaceError) {
    boost::asio::io_context io;
    auto hive = std::make_shared<Hive>(io);
    auto connection = std::make_shared<ObservingConnection>(hive, 4);

    connection->async_send(std::vector<std::uint8_t>(5, 0x42));
    for (int i = 0; i < 8 && connection->disconnect_count == 0; ++i) {
        io.poll();
    }

    EXPECT_TRUE(connection->is_stopped());
    ASSERT_TRUE(connection->last_error.has_value());
    EXPECT_EQ(connection->last_error->value(), boost::asio::error::no_buffer_space);
    EXPECT_EQ(connection->disconnect_count, 1u);
}

TEST(SessionBackpressureTest, OversizedPacketDropsSessionAndRecordsQueueDropMetric) {
    boost::asio::io_context io;
    Dispatcher dispatcher;
    BufferManager buffer_manager(2048, 8);
    auto options = std::make_shared<SessionOptions>();
    options->send_queue_max = 16;
    auto state = std::make_shared<server::core::net::ConnectionRuntimeState>();
    auto session = std::make_shared<Session>(
        boost::asio::ip::tcp::socket(io),
        dispatcher,
        buffer_manager,
        options,
        state);

    std::size_t close_count = 0;
    session->set_on_close([&](std::shared_ptr<Session>) {
        ++close_count;
    });

    const auto before = server::core::runtime_metrics::snapshot();
    session->async_send(server::core::protocol::MSG_PING, std::vector<std::uint8_t>(32, 0x7F));
    io.run();
    const auto after = server::core::runtime_metrics::snapshot();

    EXPECT_EQ(after.send_queue_drop_total, before.send_queue_drop_total + 1);
    EXPECT_EQ(close_count, 1u);
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

TEST(PacketUtf8Test, RejectsOverlongAndSurrogateSequences) {
    const std::array<std::uint8_t, 2> overlong_slash{0xC0, 0xAF};
    EXPECT_FALSE(server::core::protocol::is_valid_utf8(overlong_slash));

    const std::array<std::uint8_t, 3> surrogate{0xED, 0xA0, 0x80};
    EXPECT_FALSE(server::core::protocol::is_valid_utf8(surrogate));

    const std::array<std::uint8_t, 4> out_of_range{0xF4, 0x90, 0x80, 0x80};
    EXPECT_FALSE(server::core::protocol::is_valid_utf8(out_of_range));
}

TEST(PacketUtf8Test, AcceptsValidMultibyteUtf8) {
    const std::array<std::uint8_t, 16> bytes{
        0x68, 0x65, 0x6C, 0x6C, 0x6F, 0x20,
        0xE2, 0x82, 0xAC, // EUR
        0x20,
        0xF0, 0x9F, 0x98, 0x80, // grinning face
        0x21,
        0x0A,
    };
    EXPECT_TRUE(server::core::protocol::is_valid_utf8(bytes));
}
