/**
 * @file gateway_lifecycle.cpp
 * @brief GatewayApp의 bootstrap/config/probe/listener lifecycle TU입니다.
 */
#include "gateway/gateway_app.hpp"

#include <cstdlib>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>
#include <string>

#include "gateway_app_access.hpp"
#include "gateway_app_state.hpp"
#include "server/core/app/engine_builder.hpp"
#include "server/core/runtime_metrics.hpp"
#include "server/core/util/log.hpp"

/**
 * @brief GatewayApp의 생성자/run/stop 순서와 모듈 wiring을 담당하는 lifecycle TU입니다.
 *
 * 이 구현은 게이트웨이의 startup/shutdown 순서, readiness wiring, metrics endpoint,
 * 그리고 module bootstrap 연결만 담당합니다. 설정 파싱, TCP/UDP ingress, infra probe,
 * 라우팅/백엔드 선택 정책은 별도 구현 파일로 분리해 이 파일은 순수한 lifecycle 흐름에
 * 집중합니다.
 */
namespace gateway {

namespace {

std::string make_boot_id() {
    std::random_device rd;
    const std::uint64_t v = (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd());
    std::ostringstream oss;
    oss << std::hex << std::setw(8) << std::setfill('0') << static_cast<std::uint32_t>(v & 0xFFFFFFFFu);
    return oss.str();
}

} // namespace

// --- GatewayApp Implementation ---

GatewayApp::GatewayApp()
    : impl_(std::make_unique<Impl>()) {
    server::core::runtime_metrics::set_liveness_state(
        server::core::runtime_metrics::LivenessState::kBootstrapping);
    GatewayAppAccess::configure_gateway(*this);

    impl_->boot_id_ = make_boot_id();
    server::core::log::info("GatewayApp boot_id=" + impl_->boot_id_);

    if (impl_->udp_listen_port_ != 0 && impl_->udp_bind_secret_.empty()) {
        impl_->udp_bind_secret_ = impl_->boot_id_;
        server::core::log::warn("GatewayApp GATEWAY_UDP_BIND_SECRET not set; using boot_id derived secret");
    }

    GatewayAppAccess::configure_infrastructure(*this);
    impl_->engine_.set_service(impl_->hive_);
    impl_->engine_.set_service(impl_->authenticator_);

    // Redis는 백엔드 탐색(discovery)과 고정 라우팅(sticky routing)에 필수다.
    // 이 의존성을 readiness에 반영해야, 엣지가 백엔드를 찾지 못하는 상태를 "정상 준비"로 오해하지 않는다.
    impl_->engine_.declare_dependency("redis", server::core::app::AppHost::DependencyRequirement::kRequired);
    impl_->engine_.set_ready(false);

    if (const char* port_env = std::getenv("METRICS_PORT")) {
        try {
            impl_->metrics_port_ = static_cast<std::uint16_t>(std::stoul(port_env));
        } catch (...) {
            server::core::log::warn("GatewayApp invalid METRICS_PORT; using default");
        }
    }

    impl_->engine_.start_admin_http(impl_->metrics_port_, [this]() {
        return GatewayAppAccess::render_metrics_text(*this);
    });

    impl_->engine_.register_module(
        "gateway-runtime",
        [this](server::core::app::EngineRuntime&) {
            GatewayAppAccess::start_listener(*this);
            GatewayAppAccess::start_udp_listener(*this);
            GatewayAppAccess::start_infrastructure_probe(*this);
        },
        [this]() { stop(); },
        [this]() {
            server::core::app::EngineRuntime::WatchdogStatus status;
            const bool listener_ready = static_cast<bool>(impl_->listener_);
            const bool udp_ready = (impl_->udp_listen_port_ == 0) || static_cast<bool>(impl_->udp_socket_);
            const bool probe_ready = impl_->redis_client_ == nullptr || impl_->infra_probe_thread_.joinable();
            status.healthy = listener_ready && udp_ready && probe_ready;
            if (!listener_ready) {
                status.detail = "listener-missing";
            } else if (!udp_ready) {
                status.detail = "udp-listener-missing";
            } else if (!probe_ready) {
                status.detail = "infra-probe-missing";
            } else {
                status.detail = "gateway-runtime-ready";
            }
            return status;
        });
}

GatewayApp::~GatewayApp() {
    stop();
}

boost::asio::io_context& GatewayAppAccess::io_context(GatewayApp& app) {
    return app.impl_->io_;
}

std::string GatewayAppAccess::gateway_id(const GatewayApp& app) {
    return app.impl_->gateway_id_;
}

bool GatewayAppAccess::allow_anonymous(const GatewayApp& app) noexcept {
    return app.impl_->allow_anonymous_;
}

int GatewayApp::run() {
    impl_->engine_.start_modules();
    impl_->engine_.mark_running();
    server::core::runtime_metrics::set_liveness_state(
        server::core::runtime_metrics::LivenessState::kRunning);
    impl_->engine_.install_asio_termination_signals(impl_->io_, {});

    server::core::log::info("GatewayApp starting main loop");
    impl_->hive_->run();

    server::core::runtime_metrics::set_liveness_state(
        server::core::runtime_metrics::LivenessState::kStopping);
    impl_->engine_.run_shutdown();
    impl_->engine_.mark_stopped();
    server::core::log::info("GatewayApp stopped");
    return 0;
}

void GatewayApp::stop() {
    impl_->app_host_.request_stop();
    impl_->app_host_.set_ready(false);
    GatewayAppAccess::stop_infrastructure_probe(*this);
    impl_->app_host_.stop_admin_http();

    if (impl_->listener_) {
        impl_->listener_->stop();
    }

    GatewayAppAccess::stop_udp_listener(*this);

    {
        std::lock_guard<std::mutex> lock(impl_->session_mutex_);
        for (auto& [_, state] : impl_->sessions_) {
            if (state && state->session) {
                state->session->close();
            }
        }
        impl_->sessions_.clear();
    }

    if (impl_->hive_) {
        impl_->hive_->stop();
    }
    impl_->io_.stop();
}

} // namespace gateway


