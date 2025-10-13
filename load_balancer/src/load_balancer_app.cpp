#include "load_balancer/load_balancer_app.hpp"

#include <chrono>
#include <thread>

#include "server/core/util/log.hpp"

namespace load_balancer {

using namespace std::chrono_literals;

LoadBalancerApp::LoadBalancerApp()
    : hive_(std::make_shared<server::core::net::Hive>(io_))
    , heartbeat_timer_(io_) {}

bool LoadBalancerApp::run_smoke_test() {
    heartbeat_executed_.store(false, std::memory_order_relaxed);
    schedule_heartbeat();

    std::thread worker([this]() { hive_->run(); });
    worker.join();

    auto records = state_backend_.list_instances();
    const bool wrote_state = !records.empty() && records.front().instance_id == "lb-smoke";
    const bool ok = heartbeat_executed_.load(std::memory_order_relaxed) && wrote_state;
    if (ok) {
        server::core::log::info("LoadBalancerApp state backend smoke test completed");
    } else {
        server::core::log::warn("LoadBalancerApp smoke test failed to record instance state");
    }
    return ok;
}

void LoadBalancerApp::schedule_heartbeat() {
    heartbeat_timer_.expires_after(5ms);
    heartbeat_timer_.async_wait([this](const boost::system::error_code& ec) {
        if (ec) {
            server::core::log::warn(std::string("LoadBalancerApp heartbeat timer error: ") + ec.message());
            hive_->stop();
            return;
        }

        server::state::InstanceRecord record{};
        record.instance_id = "lb-smoke";
        record.host = "127.0.0.1";
        record.port = 6100;
        record.role = "load_balancer";
        record.capacity = 1;
        record.active_sessions = 0;
        record.last_heartbeat_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());

        state_backend_.upsert(record);
        state_backend_.touch(record.instance_id, record.last_heartbeat_ms);
        heartbeat_executed_.store(true, std::memory_order_relaxed);
        hive_->stop();
    });
}

} // namespace load_balancer
