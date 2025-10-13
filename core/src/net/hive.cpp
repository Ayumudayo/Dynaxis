#include "server/core/net/hive.hpp"

namespace server::core::net {

Hive::Hive(io_context& io)
    : io_(io)
    , guard_(boost::asio::make_work_guard(io_)) {}

Hive::~Hive() {
    stop();
}

Hive::io_context& Hive::context() {
    return io_;
}

void Hive::run() {
    stopped_.store(false, std::memory_order_relaxed);
    io_.run();
}

void Hive::stop() {
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    guard_.reset();
    io_.stop();
}

bool Hive::is_stopped() const {
    return stopped_.load(std::memory_order_relaxed);
}

} // namespace server::core::net
