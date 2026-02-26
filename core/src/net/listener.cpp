#include "server/core/net/listener.hpp"

#include <utility>

/**
 * @brief Listener 공통 accept 루프 구현입니다.
 *
 * connection factory로 생성 전략을 분리해 gateway/server가 동일 루프를 재사용하고,
 * 리스너는 transport 초기화와 수락 책임만 담당합니다.
 */
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
        // 초기화가 실패하면 이후 start 호출이 들어와도 accept를 재시도하지 않습니다.
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
    // Listener는 Acceptor와 달리 핸들러를 가볍게 감싸는 추상화이므로 별도의 running 플래그 없이
    // stopped_만으로 생명주기를 제어합니다.
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

boost::asio::ip::tcp::endpoint Listener::local_endpoint() const {
    boost::system::error_code ec;
    auto endpoint = acceptor_.local_endpoint(ec);
    if (ec) {
        return {};
    }
    return endpoint;
}

void Listener::do_accept() {
    auto self = shared_from_this();
    acceptor_.async_accept([self](const boost::system::error_code& ec, boost::asio::ip::tcp::socket socket) {
        if (ec) {
            self->on_error(ec);
        } else if (!self->stopped_.load(std::memory_order_relaxed)) {
            // 외부에서 제공한 factory를 통해 파생 Connection을 생성하므로,
            // Listener는 transport 초기화까지만 책임집니다.
            // 생성된 Connection 객체는 스스로의 수명을 관리합니다.
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
