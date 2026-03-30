#include "gateway_backend_connection.hpp"

#include <memory>
#include <utility>
#include <vector>

#include <boost/asio/connect.hpp>
#include <boost/asio/write.hpp>

#include "gateway/gateway_connection.hpp"
#include "gateway_app_access.hpp"
#include "server/core/net/queue_budget.hpp"
#include "server/core/util/log.hpp"

namespace gateway {

namespace {

constexpr std::uint32_t kDefaultBackendConnectTimeoutMs = 5000;
constexpr std::size_t kDefaultBackendSendQueueMaxBytes = 256 * 1024;

} // namespace

BackendConnection::BackendConnection(GatewayApp& app,
                                     std::string session_id,
                                     std::string client_id,
                                     std::string backend_instance_id,
                                     bool sticky_hit,
                                     std::weak_ptr<GatewayConnection> connection,
                                     std::size_t send_queue_max_bytes,
                                     std::chrono::milliseconds connect_timeout)
    : app_(app)
    , session_id_(std::move(session_id))
    , client_id_(std::move(client_id))
    , backend_instance_id_(std::move(backend_instance_id))
    , sticky_hit_(sticky_hit)
    , connection_(std::move(connection))
    , socket_(GatewayAppAccess::io_context(app))
    , connect_timer_(GatewayAppAccess::io_context(app))
    , retry_timer_(GatewayAppAccess::io_context(app))
    , send_queue_max_bytes_(send_queue_max_bytes > 0 ? send_queue_max_bytes : kDefaultBackendSendQueueMaxBytes)
    , connect_timeout_(connect_timeout > std::chrono::milliseconds{0}
                           ? connect_timeout
                           : std::chrono::milliseconds{kDefaultBackendConnectTimeoutMs}) {
}

BackendConnection::~BackendConnection() {
    close();
}

void BackendConnection::connect(const std::string& host, std::uint16_t port) {
    if (closed_.load(std::memory_order_relaxed)) {
        return;
    }

    connect_host_ = host;
    connect_port_ = port;
    retry_attempt_ = 0;

    do_connect(host, port);
}

void BackendConnection::do_connect(const std::string& host, std::uint16_t port) {
    close_socket_for_retry();

    auto self = shared_from_this();
    auto resolver = std::make_shared<boost::asio::ip::tcp::resolver>(GatewayAppAccess::io_context(app_));

    resolver->async_resolve(
        host,
        std::to_string(port),
        [self, this, resolver](const boost::system::error_code& ec,
                               boost::asio::ip::tcp::resolver::results_type results) {
            if (closed_.load(std::memory_order_relaxed)) {
                return;
            }

            if (ec) {
                GatewayAppAccess::record_backend_resolve_fail(app_);
                GatewayAppAccess::record_backend_connect_failure_event(app_);
                server::core::log::warn("BackendConnection resolve failed: " + ec.message());
                if (schedule_connect_retry("resolve failed")) {
                    return;
                }
                if (auto conn = connection_.lock()) {
                    conn->handle_backend_close("resolve failed");
                } else {
                    close();
                }
                return;
            }

            connect_timer_.expires_after(connect_timeout_);
            connect_timer_.async_wait([self, this](const boost::system::error_code& timer_ec) {
                if (timer_ec == boost::asio::error::operation_aborted) {
                    return;
                }
                on_connect_timeout();
            });

            boost::asio::async_connect(
                socket_,
                results,
                [self, this](const boost::system::error_code& ec,
                             const boost::asio::ip::tcp::endpoint& /*endpoint*/) {
                    (void)connect_timer_.cancel();

                    if (closed_.load(std::memory_order_relaxed)) {
                        return;
                    }

                    if (ec) {
                        GatewayAppAccess::record_backend_connect_fail(app_);
                        GatewayAppAccess::record_backend_connect_failure_event(app_);
                        server::core::log::warn("BackendConnection connect failed: " + ec.message());
                        if (schedule_connect_retry("connect failed")) {
                            return;
                        }
                        if (auto conn = connection_.lock()) {
                            conn->handle_backend_close("connect failed");
                        } else {
                            close();
                        }
                        return;
                    }

                    {
                        std::lock_guard<std::mutex> lock(send_mutex_);
                        connected_ = true;
                        retry_attempt_ = 0;
                        if (!write_queue_.empty()) {
                            do_write();
                        }
                    }

                    (void)retry_timer_.cancel();
                    GatewayAppAccess::record_backend_connect_success_event(app_);
                    GatewayAppAccess::on_backend_connected(app_, client_id_, backend_instance_id_, sticky_hit_);
                    do_read();
                });
        });
}

void BackendConnection::close_socket_for_retry() {
    boost::system::error_code ignored;
    if (socket_.is_open()) {
        socket_.cancel(ignored);
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
        socket_.close(ignored);
    }

    std::lock_guard<std::mutex> lock(send_mutex_);
    connected_ = false;
    write_in_progress_ = false;
}

bool BackendConnection::schedule_connect_retry(const char* reason) {
    if (!GatewayAppAccess::consume_backend_retry_budget(app_)) {
        GatewayAppAccess::record_backend_retry_budget_exhausted(app_);
        server::core::log::warn(
            "BackendConnection retry budget exhausted: session=" + session_id_ + " reason=" + reason);
        return false;
    }

    ++retry_attempt_;
    GatewayAppAccess::record_backend_retry_scheduled(app_);
    const auto delay = GatewayAppAccess::backend_retry_delay(app_, retry_attempt_);
    close_socket_for_retry();

    server::core::log::warn(
        "BackendConnection scheduling retry: session=" + session_id_
        + " attempt=" + std::to_string(retry_attempt_)
        + " delay_ms=" + std::to_string(delay.count())
        + " reason=" + reason);

    auto self = shared_from_this();
    retry_timer_.expires_after(delay);
    retry_timer_.async_wait([self, this](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted) {
            return;
        }
        if (closed_.load(std::memory_order_relaxed)) {
            return;
        }
        do_connect(connect_host_, connect_port_);
    });

    return true;
}

void BackendConnection::on_connect_timeout() {
    if (closed_.load(std::memory_order_relaxed)) {
        return;
    }

    GatewayAppAccess::record_backend_connect_timeout(app_);
    GatewayAppAccess::record_backend_connect_failure_event(app_);
    server::core::log::warn(
        "BackendConnection connect timeout after " + std::to_string(connect_timeout_.count()) + "ms");

    if (schedule_connect_retry("connect timeout")) {
        return;
    }

    if (auto conn = connection_.lock()) {
        conn->handle_backend_close("connect timeout");
    } else {
        close();
    }
}

void BackendConnection::send(std::vector<std::uint8_t> payload) {
    if (payload.empty()) {
        return;
    }

    auto buffer = std::make_shared<std::vector<std::uint8_t>>(std::move(payload));
    bool overflow = false;

    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        if (closed_) {
            return;
        }

        const auto payload_bytes = buffer->size();
        if (server::core::net::exceeds_queue_budget(send_queue_max_bytes_, queued_bytes_, payload_bytes)) {
            overflow = true;
        } else {
            queued_bytes_ += payload_bytes;
            write_queue_.push_back(std::move(buffer));
            if (connected_ && !write_in_progress_) {
                do_write();
            }
        }
    }

    if (overflow) {
        GatewayAppAccess::record_backend_send_queue_overflow(app_);
        server::core::log::warn(
            "BackendConnection send queue overflow: max_bytes=" + std::to_string(send_queue_max_bytes_));
        if (auto conn = connection_.lock()) {
            conn->handle_backend_close("backend send queue overflow");
        } else {
            close();
        }
    }
}

void BackendConnection::send(const std::uint8_t* data, std::size_t length) {
    if (!data || length == 0) {
        return;
    }

    auto buffer = std::make_shared<std::vector<std::uint8_t>>(data, data + length);
    bool overflow = false;

    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        if (closed_) {
            return;
        }

        const auto payload_bytes = buffer->size();
        if (server::core::net::exceeds_queue_budget(send_queue_max_bytes_, queued_bytes_, payload_bytes)) {
            overflow = true;
        } else {
            queued_bytes_ += payload_bytes;
            write_queue_.push_back(std::move(buffer));
            if (connected_ && !write_in_progress_) {
                do_write();
            }
        }
    }

    if (overflow) {
        GatewayAppAccess::record_backend_send_queue_overflow(app_);
        server::core::log::warn(
            "BackendConnection send queue overflow: max_bytes=" + std::to_string(send_queue_max_bytes_));
        if (auto conn = connection_.lock()) {
            conn->handle_backend_close("backend send queue overflow");
        } else {
            close();
        }
    }
}

void BackendConnection::do_write() {
    if (write_queue_.empty()) {
        write_in_progress_ = false;
        return;
    }

    write_in_progress_ = true;
    auto msg = write_queue_.front();
    if (!msg) {
        write_queue_.pop_front();
        if (!write_queue_.empty()) {
            do_write();
        } else {
            write_in_progress_ = false;
        }
        return;
    }

    auto self = shared_from_this();
    boost::asio::async_write(
        socket_,
        boost::asio::buffer(*msg),
        [self, this, msg](const boost::system::error_code& ec, std::size_t /*bytes_transferred*/) {
            if (ec == boost::asio::error::operation_aborted || closed_.load(std::memory_order_relaxed)) {
                return;
            }
            if (ec) {
                GatewayAppAccess::record_backend_write_error(app_);
                if (auto conn = connection_.lock()) {
                    conn->handle_backend_close("backend write failed");
                } else {
                    close();
                }
                return;
            }

            std::lock_guard<std::mutex> lock(send_mutex_);
            if (!write_queue_.empty()) {
                const auto sent = msg->size();
                queued_bytes_ = queued_bytes_ >= sent ? (queued_bytes_ - sent) : 0;
                write_queue_.pop_front();
            }
            if (!write_queue_.empty()) {
                do_write();
            } else {
                write_in_progress_ = false;
            }
        });
}

void BackendConnection::do_read() {
    auto self = shared_from_this();
    socket_.async_read_some(boost::asio::buffer(buffer_),
                            [self, this](const boost::system::error_code& ec, std::size_t bytes_transferred) {
                                on_read(ec, bytes_transferred);
                            });
}

void BackendConnection::on_read(const boost::system::error_code& ec, std::size_t bytes_transferred) {
    if (ec) {
        if (ec != boost::asio::error::operation_aborted) {
            if (auto conn = connection_.lock()) {
                conn->handle_backend_close(ec.message());
            }
            close();
        }
        return;
    }

    if (bytes_transferred > 0) {
        if (auto conn = connection_.lock()) {
            std::vector<std::uint8_t> data(buffer_.begin(), buffer_.begin() + bytes_transferred);
            conn->handle_backend_payload(std::move(data));
        }
        do_read();
    }
}

void BackendConnection::close() {
    bool expected = false;
    if (!closed_.compare_exchange_strong(expected, true)) {
        return;
    }

    (void)connect_timer_.cancel();
    (void)retry_timer_.cancel();

    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        write_queue_.clear();
        queued_bytes_ = 0;
        connected_ = false;
        write_in_progress_ = false;
    }

    boost::system::error_code ignored;
    if (socket_.is_open()) {
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
        socket_.close(ignored);
    }
}

const std::string& BackendConnection::session_id() const {
    return session_id_;
}

} // namespace gateway
