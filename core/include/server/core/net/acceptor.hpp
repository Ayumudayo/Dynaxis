#pragma once

#include <memory>
#include <functional>
#include <boost/asio.hpp>

namespace server::core {

namespace asio = boost::asio;

class Session;
class Dispatcher;
class BufferManager;
struct SessionOptions;
struct SharedState;

class Acceptor : public std::enable_shared_from_this<Acceptor> {
public:
    using new_session_cb_t = std::function<void(std::shared_ptr<Session>)>;
    Acceptor(asio::io_context& io,
             const asio::ip::tcp::endpoint& ep,
             Dispatcher& dispatcher,
             BufferManager& buffer_manager,
             std::shared_ptr<const SessionOptions> options,
             std::shared_ptr<SharedState> state,
             new_session_cb_t on_new_session = {});

    void start();
    void stop();

private:
    void schedule_accept_retry();
    void do_accept();

    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer accept_retry_timer_;
    std::atomic<bool> running_{false};
    Dispatcher& dispatcher_;
    BufferManager& buffer_manager_;
    std::shared_ptr<const SessionOptions> options_;
    std::shared_ptr<SharedState> state_;
    new_session_cb_t on_new_session_{};
};

} // namespace server::core
