#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace dynaxis::admin {

/**
 * @brief `AdminHttpServer`가 파싱한 단일 HTTP 요청 정보입니다.
 */
struct AdminHttpRequest {
    std::string method;
    std::string target;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    std::string source_ip;
};

/**
 * @brief `AdminHttpServer` 핸들러가 반환하는 HTTP 응답 구조체입니다.
 */
struct AdminHttpResponse {
    std::string status{"200 OK"};
    std::string content_type{"application/json; charset=utf-8"};
    std::string body;
    std::string extra_headers;
};

/**
 * @brief 웹 관리 콘솔 제어면 전용 경량 Boost.Asio HTTP 서버입니다.
 *
 * `server_core`의 `MetricsHttpServer`와 달리 정적 UI 자산, API v1 엔드포인트, `/healthz`,
 * `/readyz`, `/metrics`를 모두 처리하며 `core/`를 배포·웹 UI 개념으로 오염시키지 않기 위해
 * `admin_console_core` 내부에 독립 구현합니다.
 */
class AdminHttpServer {
public:
    /** @brief 요청을 받아 `AdminHttpResponse`를 생성하는 라우터 콜백 시그니처입니다. */
    using RouteHandler = std::function<AdminHttpResponse(const AdminHttpRequest& request)>;

    /**
     * @brief 수신 포트와 요청 핸들러를 받아 HTTP 서버를 생성합니다.
     * @param port 바인드할 TCP 포트 (`0`이면 운영체제가 임시 포트를 할당)
     * @param handler 수신 요청을 처리할 콜백
     */
    AdminHttpServer(unsigned short port, RouteHandler handler);

    /** @brief 열려 있는 리스너와 I/O 스레드를 정리하고 종료합니다. */
    ~AdminHttpServer();

    /** @brief TCP 리스너를 바인드하고 백그라운드 수신 스레드를 시작합니다. */
    void start();

    /** @brief 수신 루프를 중지하고 I/O 스레드가 종료될 때까지 대기합니다. */
    void stop();

    /**
     * @brief 실제로 바인드된 로컬 TCP 포트 번호를 반환합니다.
     * @return 바인드된 포트 (시작 전 또는 실패 시 생성자 인자 `port`)
     */
    unsigned short bound_port() const noexcept;

private:
    void do_accept();

    unsigned short port_;
    std::atomic<unsigned short> bound_port_{0};
    RouteHandler handler_;
    std::shared_ptr<boost::asio::io_context> io_context_;
    std::shared_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
    std::unique_ptr<std::thread> io_thread_;
    std::atomic<bool> stopped_{false};
};

} // namespace dynaxis::admin
