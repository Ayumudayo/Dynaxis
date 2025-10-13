#include "server/core/net/listener.hpp"

#include <utility>

namespace server::core::net {

Listener::Listener(std::shared_ptr<Hive> hive,
                   const boost::asio::ip::tcp::endpoint& endpoint,
                   connection_factory factory)
    : hive_(std::move(hive))
    , acceptor_(hive_->context())
    , factory_(std::move(factory)) {
    boost::system::error_code ec;
    acceptor_.open(endpoint.protocol(), ec);
    if (!ec) {
        acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true), ec);
    }
    if (!ec) {
        acceptor_.bind(endpoint, ec);
    }
    if (!ec) {
        acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
    }
    if (ec) {
        on_error(ec);
        stopped_.store(true, std::memory_order_relaxed);
    }
}

Listener::~Listener() {
    stop();
}

void Listener::start() {
    if (stopped_.load(std::memory_order_relaxed)) {
        return;
    }
    do_accept();
}

void Listener::stop() {
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    boost::system::error_code ec;
    acceptor_.close(ec);
}

bool Listener::is_stopped() const {
    return stopped_.load(std::memory_order_relaxed);
}

void Listener::on_accept(std::shared_ptr<Connection>) {}
void Listener::on_error(const boost::system::error_code&) {}

void Listener::do_accept() {
    auto self = shared_from_this();
    acceptor_.async_accept([self](const boost::system::error_code& ec, boost::asio::ip::tcp::socket socket) {
        if (ec) {
            self->on_error(ec);
        } else if (!self->stopped_.load(std::memory_order_relaxed)) {
            auto connection = self->factory_(self->hive_);
            connection->socket() = std::move(socket);
            connection->start();
            self->on_accept(connection);
        }

        if (!self->stopped_.load(std::memory_order_relaxed)) {
            self->do_accept();
        }
    });
}

} // namespace server::core::net
