#include <gtest/gtest.h>

#include <server/core/concurrent/job_queue.hpp>
#include <server/core/net/transport_router.hpp>
#include <server/core/protocol/protocol_errors.hpp>
#include <server/core/util/service_registry.hpp>

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using server::core::JobQueue;
namespace services = server::core::util::services;

namespace {
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

class ScopedGlobalQueue {
public:
    explicit ScopedGlobalQueue(const std::shared_ptr<JobQueue>& queue)
        : owner_(reinterpret_cast<services::Registry::OwnerToken>(this)) {
        services::Registry::instance().set_owned(owner_, queue);
    }
    ~ScopedGlobalQueue() { services::Registry::instance().clear_owned(owner_); }
private:
    services::Registry::OwnerToken owner_;
};

server::core::protocol::OpcodePolicy worker_policy() {
    auto policy = server::core::protocol::default_opcode_policy();
    policy.processing_place = server::core::protocol::ProcessingPlace::kWorker;
    return policy;
}

} // namespace

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

TEST(TransportRouterTest, ExplicitQueuesIsolateRoutersFromGlobalQueue) {
    auto global_queue = std::make_shared<JobQueue>(4);
    ScopedGlobalQueue global(global_queue);
    auto first_queue = std::make_shared<JobQueue>(4);
    auto second_queue = std::make_shared<JobQueue>(4);
    server::core::net::TransportRouter first(first_queue), second(second_queue);
    auto first_session = std::make_shared<TestTransportSession>();
    auto second_session = std::make_shared<TestTransportSession>();
    std::vector<int> calls;
    first.register_handler(4200, [&](auto&, auto) { calls.push_back(1); }, worker_policy());
    second.register_handler(4200, [&](auto&, auto) { calls.push_back(2); }, worker_policy());
    EXPECT_TRUE(first.dispatch(4200, first_session, {}));
    EXPECT_TRUE(second.dispatch(4200, second_session, {}));
    first_queue->Stop();
    second_queue->Stop();
    global_queue->Stop();
    EXPECT_FALSE(global_queue->Pop());
    auto second_job = second_queue->Pop();
    ASSERT_TRUE(second_job);
    second_job();
    second_session->drain_serialized();
    EXPECT_EQ(calls, std::vector<int>{2});
    auto first_job = first_queue->Pop();
    ASSERT_TRUE(first_job);
    first_job();
    first_session->drain_serialized();
    EXPECT_EQ(calls, (std::vector<int>{2, 1}));
}

TEST(TransportRouterTest, ExplicitNullQueueRejectsWithoutGlobalFallback) {
    auto global_queue = std::make_shared<JobQueue>(4);
    ScopedGlobalQueue global(global_queue);
    server::core::net::TransportRouter router(nullptr);
    auto session = std::make_shared<TestTransportSession>();
    bool called = false;
    router.register_handler(4201, [&](auto&, auto) { called = true; }, worker_policy());
    EXPECT_TRUE(router.dispatch(4201, session, {}));
    ASSERT_EQ(session->errors.size(), 1u);
    EXPECT_EQ(session->errors[0].first, server::core::protocol::errc::SERVER_BUSY);
    EXPECT_FALSE(called);
    global_queue->Stop();
    EXPECT_FALSE(global_queue->Pop());
}

TEST(TransportRouterTest, FullOrStoppedQueueRejectsWithoutGlobalFallback) {
    auto global_queue = std::make_shared<JobQueue>(4);
    ScopedGlobalQueue global(global_queue);
    auto queue = std::make_shared<JobQueue>(1);
    ASSERT_TRUE(queue->TryPush([] {}));
    server::core::net::TransportRouter router(queue);
    auto session = std::make_shared<TestTransportSession>();
    bool called = false;
    router.register_handler(4202, [&](auto&, auto) { called = true; }, worker_policy());
    EXPECT_TRUE(router.dispatch(4202, session, {}));
    queue->Stop();
    EXPECT_TRUE(router.dispatch(4202, session, {}));
    EXPECT_EQ(session->errors.size(), 2u);
    EXPECT_TRUE(session->serialized_tasks.empty());
    EXPECT_FALSE(called);
    global_queue->Stop();
    EXPECT_FALSE(global_queue->Pop());
}

TEST(TransportRouterTest, DeferredHandlerOwnsPayloadAndSessionAfterRouterDestruction) {
    auto queue = std::make_shared<JobQueue>(1);
    auto session = std::make_shared<TestTransportSession>();
    std::weak_ptr<TestTransportSession> weak_session = session;
    std::vector<std::uint8_t> observed;
    {
        server::core::net::TransportRouter router(queue);
        router.register_handler(4203, [&](auto&, std::span<const std::uint8_t> payload) {
            observed.assign(payload.begin(), payload.end());
        }, worker_policy());
        std::vector<std::uint8_t> payload{1, 2, 3};
        EXPECT_TRUE(router.dispatch(4203, session, payload));
        payload.assign(3, 9);
    }
    session.reset();
    queue->Stop();
    auto job = queue->Pop();
    ASSERT_TRUE(job);
    job();
    auto retained = weak_session.lock();
    ASSERT_TRUE(retained);
    retained->drain_serialized();
    EXPECT_EQ(observed, (std::vector<std::uint8_t>{1, 2, 3}));
    job = {};
    retained.reset();
    EXPECT_TRUE(weak_session.expired());
}

TEST(TransportRouterTest, ClosedSessionRejectsDeferredWork) {
    auto queue = std::make_shared<JobQueue>(1);
    server::core::net::TransportRouter router(queue);
    auto session = std::make_shared<TestTransportSession>();
    bool called = false;
    router.register_handler(4204, [&](auto&, auto) { called = true; }, worker_policy());
    EXPECT_TRUE(router.dispatch(4204, session, {}));
    session->accept_serialized_posts = false;
    queue->Stop();
    auto job = queue->Pop();
    ASSERT_TRUE(job);
    job();
    ASSERT_EQ(session->errors.size(), 1u);
    EXPECT_EQ(session->errors[0].first, server::core::protocol::errc::SERVER_BUSY);
    EXPECT_FALSE(called);
}
