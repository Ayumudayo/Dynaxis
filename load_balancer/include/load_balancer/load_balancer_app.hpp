#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include "server/core/net/hive.hpp"
#include "server/state/instance_registry.hpp"

namespace load_balancer {

class LoadBalancerApp {
public:
    LoadBalancerApp();
    ~LoadBalancerApp() = default;

    // 상태 저장소 연동 및 Hive run smoke 테스트
    bool run_smoke_test();

private:
    void schedule_heartbeat();

    boost::asio::io_context io_;
    std::shared_ptr<server::core::net::Hive> hive_;
    boost::asio::steady_timer heartbeat_timer_;
    server::state::InMemoryStateBackend state_backend_;
    std::atomic<bool> heartbeat_executed_{false};
};

} // namespace load_balancer
