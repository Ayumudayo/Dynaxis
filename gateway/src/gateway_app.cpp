#include "gateway/gateway_app.hpp"

#include <thread>

#include "server/core/util/log.hpp"

namespace gateway {

GatewayApp::GatewayApp()
    : hive_(std::make_shared<server::core::net::Hive>(io_))
    , stop_timer_(io_) {}

bool GatewayApp::run_smoke_test() {
    timer_fired_.store(false, std::memory_order_relaxed);
    arm_stop_timer();

    std::thread worker([this]() { hive_->run(); });
    worker.join();

    if (!timer_fired_.load(std::memory_order_relaxed)) {
        server::core::log::warn("GatewayApp smoke test timer did not fire");
        return false;
    }

    server::core::log::info("GatewayApp Hive smoke test completed");
    return true;
}

void GatewayApp::arm_stop_timer() {
    using namespace std::chrono_literals;
    stop_timer_.expires_after(5ms);
    stop_timer_.async_wait([this](const boost::system::error_code& ec) {
        if (ec) {
            server::core::log::warn(std::string("GatewayApp timer error: ") + ec.message());
            return;
        }
        timer_fired_.store(true, std::memory_order_relaxed);
        hive_->stop();
    });
}

} // namespace gateway
