#include <server/core/app/engine_builder.hpp>
#include <server/core/concurrent/task_scheduler.hpp>
#include <server/core/net/connection.hpp>
#include <server/core/net/listener.hpp>
#include <server/core/protocol/packet.hpp>

#include <array>
#include <chrono>
#include <thread>
#include <vector>

namespace {
class EchoConnection final : public server::core::net::Connection {
public:
    using Connection::Connection;
    void on_read(const std::uint8_t* data, std::size_t size) override {
        async_send(std::vector<std::uint8_t>(data, data + size));
    }
};
}

// 설치된 최소 package에서도 실제 TCP와 runtime 수명주기를 함께 검증한다.
int main() {
    using namespace std::chrono_literals;
    using tcp = boost::asio::ip::tcp;
    auto engine = server::core::app::EngineBuilder("runtime-smoke").build();
    boost::asio::io_context io;
    auto hive = std::make_shared<server::core::net::Hive>(io);
    std::shared_ptr<EchoConnection> connection;
    auto listener = std::make_shared<server::core::net::Listener>(
        hive, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0),
        [&](auto h) {
            connection = std::make_shared<EchoConnection>(std::move(h));
            return connection;
        });
    listener->start();
    tcp::socket peer(io);
    peer.connect(listener->local_endpoint());
    const std::array<std::uint8_t, 4> payload{1, 2, 3, 4};
    boost::asio::write(peer, boost::asio::buffer(payload));
    peer.non_blocking(true);
    std::array<std::uint8_t, 4> reply{};
    std::size_t received = 0;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (received < reply.size() && std::chrono::steady_clock::now() < deadline) {
        io.poll();
        boost::system::error_code ec;
        received += peer.read_some(boost::asio::buffer(reply.data() + received, reply.size() - received), ec);
        if (ec && ec != boost::asio::error::would_block && ec != boost::asio::error::try_again) {
            return 1;
        }
        std::this_thread::sleep_for(1ms);
    }
    if (received != payload.size() || reply != payload) {
        return 2;
    }
    server::core::concurrent::TaskScheduler scheduler;
    bool ran = false;
    scheduler.post([&] { ran = true; });
    if (scheduler.poll() != 1 || !ran) {
        return 3;
    }
    connection->stop();
    listener->stop();
    peer.close();
    io.poll();
    hive->stop();
    engine.mark_stopped();
    return connection->is_stopped() && listener->is_stopped() ? 0 : 4;
}
