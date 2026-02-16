#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace server::core::metrics {

/**
 * @brief Prometheus/운영 상태 노출용 경량 HTTP 서버입니다.
 *
 * 왜 별도 서버가 필요한가?
 * - 게임/채팅 트래픽 포트와 운영 관측 포트를 분리하면,
 *   문제 분석 중에도 데이터 경로에 영향을 최소화할 수 있습니다.
 */
class MetricsHttpServer {
public:
    /** @brief 수신 HTTP 요청의 최소 파싱 결과입니다. */
    struct HttpRequest {
        std::string method;
        std::string target;
        std::unordered_map<std::string, std::string> headers;
        std::string source_ip;
    };

    /** @brief 라우트 콜백이 반환하는 HTTP 응답 모델입니다. */
    struct RouteResponse {
        std::string status;
        std::string content_type;
        std::string body;
    };

    /** @brief `/metrics` 본문 텍스트를 생성하는 콜백 타입입니다. */
    using MetricsCallback = std::function<std::string()>;
    /** @brief health/ready 판정을 반환하는 콜백 타입입니다. */
    using StatusCallback = std::function<bool()>;
    /** @brief health/ready 응답 본문을 생성하는 콜백 타입입니다. */
    using StatusBodyCallback = std::function<std::string(bool ok)>;
    /** @brief 최근 로그 텍스트를 제공하는 콜백 타입입니다. */
    using LogsCallback = std::function<std::string()>;
    /** @brief 사용자 정의 라우팅 응답 콜백 타입입니다. */
    using RouteCallback = std::function<std::optional<RouteResponse>(const HttpRequest& request)>;

    /**
     * @brief 메트릭 HTTP 서버를 구성합니다.
     * @param port 수신 포트
     * @param metrics_callback `/metrics` 본문 생성 콜백
     * @param health_callback `/healthz` 상태 콜백
     * @param ready_callback `/readyz` 상태 콜백
     * @param logs_callback `/logs` 본문 콜백
     * @param health_body_callback `/healthz` 본문 콜백
     * @param ready_body_callback `/readyz` 본문 콜백
     * @param route_callback 기본 라우트 외 사용자 정의 라우트 콜백
     */
    MetricsHttpServer(unsigned short port,
                      MetricsCallback metrics_callback,
                      StatusCallback health_callback = {},
                      StatusCallback ready_callback = {},
                      LogsCallback logs_callback = {},
                      StatusBodyCallback health_body_callback = {},
                      StatusBodyCallback ready_body_callback = {},
                      RouteCallback route_callback = {});
    ~MetricsHttpServer();

    /** @brief HTTP 수신 루프를 시작합니다. */
    void start();
    /** @brief HTTP 수신 루프를 중지합니다. */
    void stop();

private:
    void do_accept();

    unsigned short port_;
    MetricsCallback callback_;
    StatusCallback health_callback_;
    StatusCallback ready_callback_;
    LogsCallback logs_callback_;
    StatusBodyCallback health_body_callback_;
    StatusBodyCallback ready_body_callback_;
    RouteCallback route_callback_;
    std::shared_ptr<boost::asio::io_context> io_context_;
    std::shared_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
    std::unique_ptr<std::thread> thread_;
    std::atomic<bool> stopped_{false};
};

} // namespace server::core::metrics
