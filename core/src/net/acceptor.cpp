#include "server/core/net/acceptor.hpp"
#include "server/core/net/session.hpp"
#include "server/core/net/dispatcher.hpp"
#include "server/core/util/log.hpp"
#include "server/core/config/options.hpp"
#include "server/core/state/shared_state.hpp"
#include "server/core/memory/memory_pool.hpp"
#include "server/core/runtime_metrics.hpp"

using boost::system::error_code;

namespace server::core {

Acceptor::Acceptor(asio::io_context& io,
                   const asio::ip::tcp::endpoint& ep,
                   Dispatcher& dispatcher,
                   BufferManager& buffer_manager,
                   std::shared_ptr<const SessionOptions> options,
                   std::shared_ptr<SharedState> state,
                   new_session_cb_t on_new_session)
    : io_(io), 
      acceptor_(io), 
      dispatcher_(dispatcher), 
      buffer_manager_(buffer_manager),
      options_(std::move(options)), 
      state_(std::move(state)), 
      on_new_session_(std::move(on_new_session)) {
    error_code ec;
    acceptor_.open(ep.protocol(), ec);
    if (ec) {
        log::error("acceptor open failed: " + ec.message());
        return;
    }
    acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
    acceptor_.bind(ep, ec);
    if (ec) {
        log::error("acceptor bind failed: " + ec.message());
        return;
    }
    acceptor_.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        log::error("acceptor listen failed: " + ec.message());
        return;
    }
}

void Acceptor::start() {
    if (running_) return;
    running_ = true;
    log::info("Acceptor started");
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
                    log::warn(std::string("accept failed: ") + ec.message());
                    self->do_accept();
                }
                return;
            }

            // 동시 연결 상한을 검사한다.
            if (self->state_ && self->state_->max_connections > 0) {
                auto cur = self->state_->connection_count.load();
                if (cur >= self->state_->max_connections) {
                    log::warn("max concurrent connections reached; closing new connection");
                    error_code ignored;
                    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
                    socket.close(ignored);
                    self->do_accept();
                    return;
                }
            }

            // 새로운 세션을 생성하고 시작한다.
            runtime_metrics::record_accept();
            try {
                auto session = std::make_shared<Session>(std::move(socket), self->dispatcher_, self->buffer_manager_, self->options_, self->state_);
                if (self->on_new_session_) self->on_new_session_(session);
                session->start();
            } catch (const std::exception& ex) {
                log::error(std::string("session creation threw: ") + ex.what());
            }

            // 다음 연결 요청을 계속 수락한다.
            self->do_accept();
        });
}

} // namespace server::core





