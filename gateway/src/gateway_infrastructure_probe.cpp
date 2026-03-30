/**
 * @file gateway_infrastructure_probe.cpp
 * @brief GatewayApp Redis 의존성 probe lifecycle 구현입니다.
 */
#include "gateway/gateway_app.hpp"

#include <chrono>
#include <exception>
#include <string>
#include <thread>

#include "gateway_app_access.hpp"
#include "gateway_app_state.hpp"
#include "server/core/util/log.hpp"

namespace gateway {

void GatewayAppAccess::start_infrastructure_probe(GatewayApp& app) {
    if (app.impl_->infra_probe_thread_.joinable()) {
        return;
    }

    app.impl_->infra_probe_stop_.store(false, std::memory_order_relaxed);
    app.impl_->infra_probe_thread_ = std::thread([&app]() {
        bool last_ok = true;
        while (!app.impl_->infra_probe_stop_.load(std::memory_order_relaxed)
               && !app.impl_->app_host_.stop_requested()) {
            bool ok = false;
            try {
                if (app.impl_->redis_client_) {
                    ok = app.impl_->redis_client_->health_check();
                }
            } catch (const std::exception& e) {
                server::core::log::warn(std::string("GatewayApp Redis health_check exception: ") + e.what());
                ok = false;
            } catch (...) {
                server::core::log::warn("GatewayApp Redis health_check unknown exception");
                ok = false;
            }

            app.impl_->app_host_.set_dependency_ok("redis", ok);

            if (ok != last_ok) {
                if (ok) {
                    server::core::log::info("GatewayApp Redis health_check OK");
                } else {
                    server::core::log::warn("GatewayApp Redis health_check FAILED");
                }
                last_ok = ok;
            }

            for (int i = 0; i < 20; ++i) {
                if (app.impl_->infra_probe_stop_.load(std::memory_order_relaxed)
                    || app.impl_->app_host_.stop_requested()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        app.impl_->app_host_.set_dependency_ok("redis", false);
    });
}

void GatewayAppAccess::stop_infrastructure_probe(GatewayApp& app) {
    app.impl_->infra_probe_stop_.store(true, std::memory_order_relaxed);
    if (app.impl_->infra_probe_thread_.joinable()
        && app.impl_->infra_probe_thread_.get_id() != std::this_thread::get_id()) {
        app.impl_->infra_probe_thread_.join();
    }
}

} // namespace gateway
