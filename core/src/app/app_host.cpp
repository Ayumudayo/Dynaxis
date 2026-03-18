#include "server/core/app/app_host.hpp"

#include "server/core/app/termination_signals.hpp"

#include "server/core/util/log.hpp"

#include <mutex>
#include <utility>
#include <vector>
#include <sstream>

/**
 * @brief AppHost의 생명주기/readiness/admin HTTP 구현입니다.
 *
 * 공통 프로세스 제어 규칙을 구현 레벨에서 일관되게 유지해,
 * server/gateway/tools 간 운영 동작 편차를 줄입니다.
 */
namespace server::core::app {

namespace corelog = server::core::log;

namespace {

constexpr std::uint8_t to_phase_code(AppHost::LifecyclePhase phase) noexcept {
    return static_cast<std::uint8_t>(phase);
}

AppHost::LifecyclePhase from_phase_code(std::uint8_t phase) noexcept {
    switch (phase) {
    case to_phase_code(AppHost::LifecyclePhase::kInit):
        return AppHost::LifecyclePhase::kInit;
    case to_phase_code(AppHost::LifecyclePhase::kBootstrapping):
        return AppHost::LifecyclePhase::kBootstrapping;
    case to_phase_code(AppHost::LifecyclePhase::kRunning):
        return AppHost::LifecyclePhase::kRunning;
    case to_phase_code(AppHost::LifecyclePhase::kStopping):
        return AppHost::LifecyclePhase::kStopping;
    case to_phase_code(AppHost::LifecyclePhase::kStopped):
        return AppHost::LifecyclePhase::kStopped;
    case to_phase_code(AppHost::LifecyclePhase::kFailed):
        return AppHost::LifecyclePhase::kFailed;
    default:
        return AppHost::LifecyclePhase::kFailed;
    }
}

} // namespace

struct AppHost::DependencyRegistry {
    struct Entry {
        std::string name;
        DependencyRequirement requirement{DependencyRequirement::kRequired};
        bool ok{false};
    };

    std::mutex mutex;
    std::vector<Entry> entries;

    // readiness는 "필수 의존성(required) 전체가 OK인가"로 계산합니다.
    // optional 의존성은 관측 대상에는 남기되 준비 판정을 막지는 않습니다.
    bool compute_ok() const {
        for (const auto& e : entries) {
            if (e.requirement == DependencyRequirement::kRequired && !e.ok) {
                return false;
            }
        }
        return true;
    }
};

struct AppHost::ShutdownRegistry {
    using Step = std::pair<std::string, std::function<void()>>;

    std::mutex mutex;
    std::vector<Step> steps;
};

AppHost::AppHost(std::string name)
    : name_(std::move(name)) {
}

AppHost::~AppHost() {
    stop_admin_http();
}

bool AppHost::request_stop() noexcept {
    const bool first = !stop_requested_.exchange(true, std::memory_order_acq_rel);
    if (first) {
        set_lifecycle_phase(LifecyclePhase::kStopping);
    }
    return first;
}

bool AppHost::stop_requested() const noexcept {
    // 로컬 플래그와 프로세스 전역 termination 플래그를 함께 본다.
    // 이렇게 하면 비-asio 루프/다른 모듈에서도 동일한 종료 신호를 공유할 수 있다.
    return stop_requested_.load(std::memory_order_relaxed) || termination_signal_received();
}

void AppHost::set_lifecycle_phase(LifecyclePhase phase) noexcept {
    lifecycle_phase_.store(to_phase_code(phase), std::memory_order_relaxed);
}

AppHost::LifecyclePhase AppHost::lifecycle_phase() const noexcept {
    const auto code = lifecycle_phase_.load(std::memory_order_relaxed);
    return from_phase_code(code);
}

const char* AppHost::lifecycle_phase_name(LifecyclePhase phase) noexcept {
    switch (phase) {
    case LifecyclePhase::kInit:
        return "init";
    case LifecyclePhase::kBootstrapping:
        return "bootstrapping";
    case LifecyclePhase::kRunning:
        return "running";
    case LifecyclePhase::kStopping:
        return "stopping";
    case LifecyclePhase::kStopped:
        return "stopped";
    case LifecyclePhase::kFailed:
        return "failed";
    default:
        return "failed";
    }
}

void AppHost::set_healthy(bool healthy) noexcept {
    healthy_.store(healthy, std::memory_order_relaxed);
}

bool AppHost::healthy() const noexcept {
    return healthy_.load(std::memory_order_relaxed);
}

void AppHost::set_ready(bool ready) noexcept {
    startup_ready_.store(ready, std::memory_order_relaxed);
}

bool AppHost::ready() const noexcept {
    return startup_ready_.load(std::memory_order_relaxed) && deps_ok_.load(std::memory_order_relaxed);
}

void AppHost::declare_dependency(std::string name, DependencyRequirement requirement) {
    if (name.empty()) {
        corelog::warn(name_ + " declare_dependency called with empty name");
        return;
    }
    if (!deps_) {
        deps_ = std::make_unique<DependencyRegistry>();
    }

    std::lock_guard<std::mutex> lock(deps_->mutex);

    // 동일 이름이 이미 있으면 requirement만 갱신한다.
    // 운영 중 설정 전환(required<->optional)을 반영할 수 있어 유연하다.
    for (auto& e : deps_->entries) {
        if (e.name == name) {
            e.requirement = requirement;
            deps_ok_.store(deps_->compute_ok(), std::memory_order_relaxed);
            return;
        }
    }

    DependencyRegistry::Entry entry;
    entry.name = std::move(name);
    entry.requirement = requirement;
    entry.ok = false;
    deps_->entries.emplace_back(std::move(entry));
    deps_ok_.store(deps_->compute_ok(), std::memory_order_relaxed);
}

void AppHost::set_dependency_ok(std::string_view name, bool ok) {
    if (name.empty()) {
        corelog::warn(name_ + " set_dependency_ok called with empty name");
        return;
    }
    if (!deps_) {
        deps_ = std::make_unique<DependencyRegistry>();
    }

    std::lock_guard<std::mutex> lock(deps_->mutex);

    for (auto& e : deps_->entries) {
        if (e.name == name) {
            e.ok = ok;
            deps_ok_.store(deps_->compute_ok(), std::memory_order_relaxed);
            return;
        }
    }

    // 선언 누락은 구성 오류 신호이므로 경고를 남긴다.
    // 동시에 런타임 복구를 위해 즉시 required 의존성으로 생성해 상태를 반영한다.
    corelog::warn(name_ + " set_dependency_ok used before declare_dependency: " + std::string(name));
    DependencyRegistry::Entry entry;
    entry.name = std::string(name);
    entry.requirement = DependencyRequirement::kRequired;
    entry.ok = ok;
    deps_->entries.emplace_back(std::move(entry));
    deps_ok_.store(deps_->compute_ok(), std::memory_order_relaxed);
}

bool AppHost::dependencies_ok() const noexcept {
    return deps_ok_.load(std::memory_order_relaxed);
}

std::string AppHost::health_body(bool ok) const {
    if (ok) {
        return "ok\n";
    }
    if (stop_requested()) {
        return "stopping\n";
    }
    return "unhealthy\n";
}

std::string AppHost::readiness_body(bool ok) const {
    if (ok) {
        return "ready\n";
    }

    std::ostringstream oss;
    oss << "not ready";

    // ready 실패 이유를 한 줄로 축약해 반환한다.
    // K8s probe, 운영 대시보드, 사람이 직접 curl 하는 상황 모두에서 즉시 원인을 파악하도록 설계했다.
    std::vector<std::string> reasons;
    if (stop_requested()) {
        reasons.emplace_back("stopping");
    }
    if (!healthy()) {
        reasons.emplace_back("unhealthy");
    }
    if (!startup_ready_.load(std::memory_order_relaxed)) {
        reasons.emplace_back("starting");
    }

    std::vector<std::string> missing;
    if (deps_ && !dependencies_ok()) {
        std::lock_guard<std::mutex> lock(deps_->mutex);
        for (const auto& e : deps_->entries) {
            if (e.requirement == DependencyRequirement::kRequired && !e.ok) {
                missing.emplace_back(e.name);
            }
        }
    }
    if (!missing.empty()) {
        std::ostringstream dep;
        dep << "deps=";
        for (std::size_t i = 0; i < missing.size(); ++i) {
            if (i != 0) dep << ',';
            dep << missing[i];
        }
        reasons.emplace_back(dep.str());
    }

    if (!reasons.empty()) {
        oss << ": ";
        for (std::size_t i = 0; i < reasons.size(); ++i) {
            if (i != 0) oss << ", ";
            oss << reasons[i];
        }
    }
    oss << "\n";
    return oss.str();
}

std::string AppHost::dependency_metrics_text() const {
    std::ostringstream out;
    out << "# TYPE runtime_dependency_ready gauge\n";

    if (deps_) {
        std::lock_guard<std::mutex> lock(deps_->mutex);
        for (const auto& e : deps_->entries) {
            const char* required = (e.requirement == DependencyRequirement::kRequired) ? "true" : "false";
            out << "runtime_dependency_ready{name=\"" << e.name << "\",required=\"" << required << "\"} "
                << (e.ok ? 1 : 0) << "\n";
        }
    }

    // 전체 집계 지표를 함께 노출해 알람 규칙을 단순화한다.
    out << "# TYPE runtime_dependencies_ok gauge\n";
    out << "runtime_dependencies_ok " << (dependencies_ok() ? 1 : 0) << "\n";
    return out.str();
}

std::string AppHost::lifecycle_metrics_text() const {
    std::ostringstream out;

    const auto phase = lifecycle_phase();
    out << "# TYPE runtime_lifecycle_phase_code gauge\n";
    out << "runtime_lifecycle_phase_code " << static_cast<unsigned>(to_phase_code(phase)) << "\n";

    out << "# TYPE runtime_lifecycle_phase gauge\n";
    out << "runtime_lifecycle_phase{phase=\"" << lifecycle_phase_name(phase) << "\"} 1\n";

    return out.str();
}

void AppHost::start_admin_http(unsigned short port,
                               server::core::metrics::MetricsHttpServer::MetricsCallback metrics_callback,
                               server::core::metrics::MetricsHttpServer::LogsCallback logs_callback,
                               server::core::metrics::MetricsHttpServer::RouteCallback route_callback) {
    if (port == 0) {
        return;
    }
    if (admin_http_) {
        return;
    }

    // health/readiness 콜백은 의도적으로 "가볍고 빠르게" 유지한다.
    // metrics HTTP 스레드에서 무거운 작업(DB 조회 등)을 하면 관측 자체가 병목이 되기 때문이다.
    admin_http_ = std::make_unique<server::core::metrics::MetricsHttpServer>(
        port,
        std::move(metrics_callback),
        [this]() {
            // healthz: 프로세스 생존성과 종료 상태만 확인한다.
            // "의존성 준비 여부"까지 넣으면 장애 분류가 혼동될 수 있어 분리한다.
            return healthy() && !stop_requested();
        },
        [this]() {
            // readyz: 트래픽 수신 가능 상태를 나타낸다.
            // 기본 ready 플래그 + 필수 의존성 + health/stop 조건을 함께 본다.
            return ready() && healthy() && !stop_requested();
        },
        std::move(logs_callback),
        [this](bool ok) { return health_body(ok); },
        [this](bool ok) { return readiness_body(ok); },
        std::move(route_callback));
    admin_http_->start();

    corelog::info(name_ + " admin http enabled on :" + std::to_string(port));

    if (!admin_http_shutdown_step_registered_) {
        add_shutdown_step("stop admin http", [this]() { stop_admin_http(); });
        admin_http_shutdown_step_registered_ = true;
    }
}

void AppHost::stop_admin_http() {
    if (!admin_http_) {
        return;
    }
    admin_http_->stop();
    admin_http_.reset();
}

void AppHost::add_shutdown_step(std::string name, std::function<void()> step) {
    if (!step) {
        return;
    }
    if (name.empty()) {
        name = "(unnamed)";
    }
    if (!shutdown_) {
        shutdown_ = std::make_unique<ShutdownRegistry>();
    }

    std::lock_guard<std::mutex> lock(shutdown_->mutex);
    shutdown_->steps.emplace_back(std::move(name), std::move(step));
}

void AppHost::run_shutdown_steps() noexcept {
    if (shutdown_ran_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (!shutdown_ || shutdown_->steps.empty()) {
        return;
    }

    std::vector<ShutdownRegistry::Step> steps;
    {
        std::lock_guard<std::mutex> lock(shutdown_->mutex);
        steps.swap(shutdown_->steps);
    }

    // 등록 역순(LIFO) 실행: 나중에 붙은 상위 레이어를 먼저 내리고,
    // 마지막에 하위 공용 자원을 정리하는 패턴을 자연스럽게 만든다.
    for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
        const auto& name = it->first;
        try {
            it->second();
        } catch (const std::exception& ex) {
            corelog::error(name_ + " shutdown step failed: " + name + ": " + ex.what());
        } catch (...) {
            corelog::error(name_ + " shutdown step failed: " + name + ": unknown exception");
        }
    }
}

void AppHost::install_asio_termination_signals(boost::asio::io_context& io,
                                               std::function<void()> on_shutdown) {
    if (signals_) {
        return;
    }

    // 프로세스 전역 폴링 플래그 핸들러도 함께 설치한다.
    // Asio 이벤트 루프 밖에서 도는 루프(예: 별도 워커 스레드)가 있어도
    // 동일한 종료 신호를 관찰해 일관된 종료 경로를 탈 수 있게 한다.
    install_termination_signal_handlers();

    signals_ = std::make_unique<boost::asio::signal_set>(io);
#if defined(SIGINT)
    signals_->add(SIGINT);
#endif
#if defined(SIGTERM)
    signals_->add(SIGTERM);
#endif

    signals_->async_wait([this, on_shutdown = std::move(on_shutdown)](const boost::system::error_code& ec, int) {
        if (ec == boost::asio::error::operation_aborted) {
            return;
        }
        if (ec) {
            return;
        }
        if (!request_stop()) {
            // 중복 시그널(SIGINT 연타 등)은 첫 번째만 처리한다.
            return;
        }

        set_ready(false);
        corelog::info(name_ + " received shutdown signal");

        run_shutdown_steps();
        try {
            if (on_shutdown) {
                on_shutdown();
            }
        } catch (const std::exception& ex) {
            corelog::error(name_ + " shutdown callback exception: " + std::string(ex.what()));
        } catch (...) {
            corelog::error(name_ + " shutdown callback unknown exception");
        }
    });
}

} // namespace server::core::app
