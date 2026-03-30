#pragma once

#include <memory>

#include "server/core/app/engine_runtime.hpp"
#include "server/core/metrics/http_server.hpp"

namespace server::app {

std::string render_metrics_text();
std::string render_logs_text();
void start_server_admin_http(server::core::app::EngineRuntime& runtime, unsigned short port);

/**
 * @brief Prometheus 메트릭 및 로그를 제공하는 간단한 HTTP 서버입니다.
 * /metrics, /logs 엔드포인트를 지원합니다.
 */
class MetricsServer {
public:
    /**
     * @brief 생성자
     * @param port 리스닝할 포트 번호
     */
    explicit MetricsServer(unsigned short port);
    ~MetricsServer();

    /**
     * @brief 서버를 시작합니다. (별도 스레드에서 실행됨)
     */
    void start();

    /**
     * @brief 서버를 중지합니다.
     */
    void stop();

private:
    unsigned short port_;
    std::unique_ptr<server::core::metrics::MetricsHttpServer> http_server_;
};

} // namespace server::app
