#pragma once

#include <memory>
#include <boost/asio.hpp>
#include <memory>

namespace server::core {

namespace asio = boost::asio;

class Session;
class Dispatcher;
struct SessionOptions;
struct SharedState;

class Acceptor : public std::enable_shared_from_this<Acceptor> {
public:
    Acceptor(asio::io_context& io,
             const asio::ip::tcp::endpoint& ep,
             Dispatcher& dispatcher,
             std::shared_ptr<const SessionOptions> options,
             std::shared_ptr<SharedState> state);

    void start();
    void stop();

private:
    void do_accept();

    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    bool running_ {false};
    Dispatcher& dispatcher_;
    std::shared_ptr<const SessionOptions> options_;
    std::shared_ptr<SharedState> state_;
};

} // namespace server::core
