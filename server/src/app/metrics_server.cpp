#include "server/app/metrics_server.hpp"
#include "chat_metrics_text.hpp"
#include "runtime_metrics_text.hpp"
#include "server_runtime_state.hpp"

#include "server/core/metrics/build_info.hpp"
#include "server/core/metrics/metrics.hpp"
#include "server/core/runtime_metrics.hpp"
#include "server/core/util/log.hpp"

#include <sstream>
#include <string>

/**
 * @brief `server_app`의 `/metrics`, `/healthz`, `/readyz` 렌더링 구현입니다.
 *
 * 런타임 카운터를 Prometheus 텍스트로 변환하고,
 * 준비 상태(readiness)와 health 상태를 AppHost와 동기화해 오케스트레이터 판정을 일관화합니다.
 * 이 파일이 별도로 존재해야 메트릭 이름, probe 의미, 서비스별 추가 계수기를
 * 한곳에서 관리할 수 있습니다. 각 기능 모듈이 자기 메트릭을 제각각 문자열로 출력하면
 * 대시보드와 알람 규칙이 서비스 개편 때마다 쉽게 깨집니다.
 */
namespace server::app {

namespace corelog = server::core::log;
namespace {

std::string render_logs_impl() {
    auto logs = corelog::recent(200);
    std::ostringstream body_stream;
    if (logs.empty()) {
        body_stream << "(no log entries)\n";
    } else {
        for (const auto& line : logs) {
            body_stream << line << '\n';
        }
    }
    return body_stream.str();
}

std::string render_metrics_impl() {
    auto snap = server::core::runtime_metrics::snapshot();
    std::ostringstream stream;
    MetricsTextWriter writer{stream};

    // 빌드 메타데이터(git hash/describe + build time)를 같이 내보내야
    // 같은 메트릭 이름을 보더라도 어느 바이너리에서 나온 값인지 현장에서 바로 구분할 수 있다.
    // 이 표식이 없으면 롤링 배포 중 "이상한 수치가 어느 버전에서 나온 것인가"를 추적하기가 급격히 어려워진다.
    server::core::metrics::append_build_info(stream);
    server::core::metrics::append_runtime_core_metrics(stream);
    server::core::metrics::append_prometheus_metrics(stream);

    append_chat_bootstrap_metrics(writer, bootstrap_metrics_snapshot());
    append_chat_continuity_metrics(writer, stream, server_chat_continuity_metrics());
    append_chat_runtime_metrics(writer, stream, snap);

    append_chat_hook_plugin_metrics(stream, server_chat_hook_plugin_metrics());
    append_chat_lua_hook_metrics(stream, server_chat_lua_hooks_metrics());

    stream << server_dependency_metrics_text();
    stream << server_lifecycle_metrics_text();
    const auto runtime_snapshot = server_runtime_snapshot();
    const auto modules = server_runtime_module_snapshot();
    append_server_runtime_module_metrics(writer, stream, runtime_snapshot, modules);
    return stream.str();
}

} // namespace

std::string render_metrics_text() {
    return render_metrics_impl();
}

std::string render_logs_text() {
    return render_logs_impl();
}

void start_server_admin_http(server::core::app::EngineRuntime& runtime, unsigned short port) {
    runtime.start_admin_http(
        port,
        []() { return render_metrics_text(); },
        []() { return render_logs_text(); });
}

MetricsServer::MetricsServer(unsigned short port)
    : port_(port) {
}

MetricsServer::~MetricsServer() {
    stop();
}

void MetricsServer::start() {
    if (http_server_) {
        return;
    }

    http_server_ = std::make_unique<server::core::metrics::MetricsHttpServer>(
        port_,
        []() { return render_metrics_text(); },
        []() { return server_health_ok(); },
        []() { return server_ready_ok(); },
        []() { return render_logs_text(); },
        [](bool ok) { return server_health_body(ok); },
        [](bool ok) { return server_readiness_body(ok); });
    http_server_->start();
}

void MetricsServer::stop() {
    if (!http_server_) {
        return;
    }
    http_server_->stop();
    http_server_.reset();
}

} // namespace server::app
