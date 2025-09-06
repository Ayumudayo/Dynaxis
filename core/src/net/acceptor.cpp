#include "server/core/acceptor.hpp"
#include "server/core/session.hpp"
#include "server/core/dispatcher.hpp"
#include "server/core/util/log.hpp"
#include "server/core/options.hpp"
#include "server/core/shared_state.hpp"

using boost::system::error_code;

namespace server::core {

Acceptor::Acceptor(asio::io_context& io,
                   const asio::ip::tcp::endpoint& ep,
                   Dispatcher& dispatcher,
                   std::shared_ptr<const SessionOptions> options,
                   std::shared_ptr<SharedState> state)
    : io_(io), acceptor_(io), dispatcher_(dispatcher), options_(std::move(options)), state_(std::move(state)) {
    error_code ec;
    acceptor_.open(ep.protocol(), ec);
    if (ec) {
        log::error("acceptor open 실패: " + ec.message());
        return;
    }
    acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
    acceptor_.bind(ep, ec);
    if (ec) {
        log::error("acceptor bind 실패: " + ec.message());
        return;
    }
    acceptor_.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        log::error("acceptor listen 실패: " + ec.message());
        return;
    }
}

void Acceptor::start() {
    if (running_) return;
    running_ = true;
    log::info("Acceptor 시작");
    do_accept();
}

void Acceptor::stop() {
    if (!running_) return;
    running_ = false;
    error_code ec;
    acceptor_.close(ec);
}

void Acceptor::do_accept() {
    if (!running_) return;
    acceptor_.async_accept(
        [self = shared_from_this()](const error_code& ec, asio::ip::tcp::socket socket) {
            if (ec) {
                if (self->running_) {
                    log::warn(std::string("accept 실패: ") + ec.message());
                    self->do_accept();
                }
                return;
            }

            // 연결 제한 체크
            if (self->state_ && self->state_->max_connections > 0) {
                auto cur = self->state_->connection_count.load();
                if (cur >= self->state_->max_connections) {
                    log::warn("동시 연결 상한 초과로 새 연결을 종료");
                    error_code ignored;
                    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
                    socket.close(ignored);
                    self->do_accept();
                    return;
                }
            }

            // 세션 생성 및 시작
            try {
                auto session = std::make_shared<Session>(std::move(socket), self->dispatcher_, self->options_, self->state_);
                session->start();
            } catch (const std::exception& ex) {
                log::error(std::string("세션 생성 예외: ") + ex.what());
            }

            // 다음 accept 루프 지속
            self->do_accept();
        });
}

} // namespace server::core
