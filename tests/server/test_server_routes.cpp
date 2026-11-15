#include <gtest/gtest.h>

#include <server/app/router.hpp>
#include <server/chat/chat_service.hpp>
#include <server/core/concurrent/job_queue.hpp>
#include <server/core/config/options.hpp>
#include <server/core/memory/memory_pool.hpp>
#include <server/core/net/connection_runtime_state.hpp>
#include <server/core/net/dispatcher.hpp>
#include <server/core/net/session.hpp>
#include <server/protocol/game_opcodes.hpp>

TEST(ServerRoutingTest, RemovedOpcodeIsRejected) {
    boost::asio::io_context io;
    server::core::JobQueue jobs;
    server::core::BufferManager buffers(4096, 16);
    server::core::Dispatcher dispatcher;
    server::app::chat::ChatService chat(io, jobs, nullptr, nullptr);
    auto session = std::make_shared<server::core::Session>(
        boost::asio::ip::tcp::socket(io), dispatcher, buffers,
        std::make_shared<server::core::SessionOptions>(),
        std::make_shared<server::core::net::ConnectionRuntimeState>());
    server::app::register_routes(dispatcher, chat);
    EXPECT_FALSE(dispatcher.dispatch(0x0206, *session, {}));
    session->stop();
    io.poll();
}
