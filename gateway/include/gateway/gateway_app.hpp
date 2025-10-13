#pragma once

#include <atomic>
#include <memory>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include "server/core/net/hive.hpp"

namespace gateway {

class GatewayApp {
public:
    GatewayApp();
    ~GatewayApp() = default;

    // 기본 smoke 테스트: Hive 실행 → 타이머 이벤트 → graceful stop
    bool run_smoke_test();

private:
    void arm_stop_timer();

    boost::asio::io_context io_;
    std::shared_ptr<server::core::net::Hive> hive_;
    boost::asio::steady_timer stop_timer_;
    std::atomic<bool> timer_fired_{false};
};

} // namespace gateway
