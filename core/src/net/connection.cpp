#include "server/core/net/connection.hpp"

#include <algorithm>
#include <utility>
#include <limits>

/**
 * @brief Connection 기본 템플릿 메서드(on_connect/on_read/on_write) 구현입니다.
 *
 * strand 직렬화를 통해 송수신 큐 동시 접근을 방지하고,
 * 파생 클래스는 프로토콜별 동작만 오버라이드하도록 책임을 분리합니다.
 */
namespace server::core::net {

// 버퍼의 마지막 소유자가 사라질 때 예약량을 반환한다. close 중에도 진행 중인 write가
// buffer를 소유하므로, 큐를 비웠다는 이유만으로 아직 사용 중인 메모리를 예산에서 빼지 않는다.
struct Connection::PendingWrite {
    std::vector<std::uint8_t> data;
    std::shared_ptr<std::atomic<std::size_t>> budget;

    PendingWrite(std::vector<std::uint8_t> payload, std::shared_ptr<std::atomic<std::size_t>> reserved)
        : data(std::move(payload)), budget(std::move(reserved)) {}
    ~PendingWrite() { budget->fetch_sub(data.size(), std::memory_order_acq_rel); }
};

Connection::Connection(std::shared_ptr<Hive> hive,
                       std::size_t send_queue_max_bytes)
    : hive_(std::move(hive))
    , socket_(hive_->context())
    , strand_(socket_.get_executor())
    , send_queue_max_bytes_(send_queue_max_bytes) {}

Connection::~Connection() {
    stop();
}

Connection::socket_type& Connection::socket() {
    return socket_;
}

void Connection::start() {
    if (stopped_.load(std::memory_order_relaxed)) {
        return;
    }

    auto self = shared_from_this();
    boost::asio::dispatch(strand_, [self]() {
        if (self->is_stopped()) {
            return;
        }
        // 템플릿 메서드 패턴을 따르므로 파생 클래스에서 on_connect/on_read 등을 오버라이드하여
        // 구체적인 동작(예: 패킷 파싱, 로깅)을 구현합니다.
        self->on_connect();
        self->read_loop();
    });
}

void Connection::stop() {
    bool expected = false;
    // compare_exchange_strong을 사용하여 여러 스레드에서 동시에 stop()을 호출해도
    // 한 번만 실행되도록 보장합니다.
    if (!stopped_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    std::shared_ptr<Connection> self;
    try {
        self = shared_from_this();
    } catch (...) {
        finalize_stop();
        return;
    }

    boost::asio::dispatch(strand_, [self]() {
        self->finalize_stop();
    });
}

void Connection::finalize_stop() {
    if (socket_.is_open()) {
        boost::system::error_code ec;
        [[maybe_unused]] const auto shutdown_result = socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        [[maybe_unused]] const auto close_result = socket_.close(ec);
    }

    write_queue_.clear();
    on_disconnect();
}

bool Connection::is_stopped() const {
    return stopped_.load(std::memory_order_relaxed);
}

void Connection::async_send(std::vector<std::uint8_t> data) {
    if (data.empty() || is_stopped() || send_overflow_.load(std::memory_order_acquire)) {
        return;
    }

    auto self = shared_from_this();
    const auto payload_size = data.size();
    const auto limit = send_queue_max_bytes_ == 0
        ? std::numeric_limits<std::size_t>::max() : send_queue_max_bytes_;
    auto pending = pending_bytes_->load(std::memory_order_relaxed);
    for (;;) {
        if (payload_size > limit - std::min(limit, pending)) {
            // reject notification 자체도 한 번만 게시해 과부하에서 executor가 다시 무한히 커지지 않게 한다.
            if (!send_overflow_.exchange(true, std::memory_order_acq_rel)) {
                boost::asio::dispatch(strand_, [self]() {
                    if (self->is_stopped()) {
                        return;
                    }
                    self->on_error(boost::asio::error::make_error_code(boost::asio::error::no_buffer_space));
                    self->stop();
                });
            }
            return;
        }
        if (pending_bytes_->compare_exchange_weak(pending, pending + payload_size,
                                                  std::memory_order_acq_rel)) {
            break;
        }
    }

    std::shared_ptr<PendingWrite> buffer;
    try {
        buffer = std::make_shared<PendingWrite>(std::move(data), pending_bytes_);
    } catch (...) {
        pending_bytes_->fetch_sub(payload_size, std::memory_order_acq_rel);
        throw;
    }
    boost::asio::post(strand_, [self, buffer = std::move(buffer)]() {
        if (self->is_stopped()) {
            return;
        }
        const bool idle = self->write_queue_.empty();
        self->write_queue_.push_back(buffer);
        if (idle) {
            self->do_write();
        }
    });
}

void Connection::on_connect() {}
void Connection::on_disconnect() {}
void Connection::on_read(const std::uint8_t*, std::size_t) {}
void Connection::on_write(std::size_t) {}
void Connection::on_error(const boost::system::error_code&) {}

boost::asio::io_context& Connection::io() {
    return hive_->context();
}

void Connection::read_loop() {
    auto self = shared_from_this();
    // 비동기 읽기를 시작합니다. 데이터가 들어오면 handle_read가 호출됩니다.
    socket_.async_read_some(boost::asio::buffer(read_buffer_),
        boost::asio::bind_executor(self->strand_, [self](const boost::system::error_code& ec, std::size_t bytes_transferred) {
            self->handle_read(ec, bytes_transferred);
        }));
}

void Connection::handle_read(const boost::system::error_code& ec, std::size_t bytes_transferred) {
    if (ec) {
        // read/write 루프는 하나라도 실패하면 세션 전체를 닫아 정의되지 않은 상태를 피합니다.
        // TCP 연결이 끊어지거나 에러가 발생한 경우입니다.
        on_error(ec);
        stop();
        return;
    }

    if (bytes_transferred > 0) {
        on_read(read_buffer_.data(), bytes_transferred);
    }

    if (!is_stopped()) {
        read_loop(); // 계속해서 다음 데이터를 기다립니다.
    }
}

void Connection::handle_write(const boost::system::error_code& ec,
                              std::size_t bytes_transferred,
                              std::size_t packet_size) {
    if (ec) {
        on_error(ec);
        stop();
        return;
    }

    (void)packet_size;
    on_write(bytes_transferred);

    if (!write_queue_.empty()) {
        write_queue_.pop_front();
    }
    if (!write_queue_.empty()) {
        // 큐가 비워질 때까지 한 번에 하나의 async_write만 유지해 backpressure를 단순화합니다.
        // 즉, 보내는 속도가 너무 빠르면 큐에 쌓이게 되고, 메모리 사용량이 늘어납니다.
        do_write();
    }
}

void Connection::do_write() {
    if (write_queue_.empty()) {
        return;
    }

    auto self = shared_from_this();
    auto buffer = write_queue_.front();
    if (!buffer) {
        write_queue_.pop_front();
        if (!write_queue_.empty()) {
            do_write();
        }
        return;
    }
    const auto packet_size = buffer->data.size();
    boost::asio::async_write(socket_,
        boost::asio::buffer(buffer->data),
        boost::asio::bind_executor(self->strand_, [self, buffer, packet_size](const boost::system::error_code& ec, std::size_t bytes_transferred) {
            self->handle_write(ec, bytes_transferred, packet_size);
        }));
}

} // namespace server::core::net
