#include <gtest/gtest.h>
#include <server/core/metrics/metrics.hpp>
#include <server/core/metrics/http_server.hpp>
#include <server/core/runtime_metrics.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <array>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

/**
 * @brief 메트릭 API/no-op 동작과 Metrics HTTP 라우트 동작을 검증합니다.
 */
using namespace server::core::metrics;

namespace {

unsigned short reserve_free_port() {
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor(io, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 0));
    const auto port = acceptor.local_endpoint().port();
    acceptor.close();
    return port;
}

std::string request_http(unsigned short port, const std::string& raw_request) {
    boost::asio::io_context io;
    boost::asio::ip::tcp::socket socket(io);
    boost::asio::connect(
        socket,
        boost::asio::ip::tcp::resolver(io).resolve("127.0.0.1", std::to_string(port))
    );

    boost::asio::write(socket, boost::asio::buffer(raw_request));

    std::string response;
    std::array<char, 1024> chunk{};
    boost::system::error_code ec;
    while (true) {
        const auto n = socket.read_some(boost::asio::buffer(chunk), ec);
        if (n > 0) {
            response.append(chunk.data(), n);
        }
        if (ec == boost::asio::error::eof) {
            break;
        }
        if (ec) {
            throw std::runtime_error("HTTP read failed: " + ec.message());
        }
    }

    return response;
}

} // namespace

// Metrics API 기본 동작 테스트
TEST(MetricsTest, BasicApi) {
    // 카운터 가져오기 (구현체가 없으면 no-op 객체 반환)
    Counter& counter = get_counter("test_counter");
    counter.inc(1.0); // 충돌 없이 호출되는지 확인

    // 게이지 가져오기
    Gauge& gauge = get_gauge("test_gauge");
    gauge.set(10.0);
    gauge.inc(1.0);
    gauge.dec(1.0);

    // 히스토그램 가져오기
    Histogram& histogram = get_histogram("test_histogram");
    histogram.observe(5.0);
    
    // 현재는 실제 메트릭 수집 여부를 확인할 방법이 없으므로(no-op),
    // API 호출 시 크래시가 발생하지 않는지 위주로 검증합니다.
    SUCCEED();
}

TEST(RuntimeMetricsTest, WriteTimeoutCounterIncrements) {
    const auto before = server::core::runtime_metrics::snapshot();
    server::core::runtime_metrics::record_session_write_timeout();
    const auto after = server::core::runtime_metrics::snapshot();

    EXPECT_EQ(after.session_write_timeout_total, before.session_write_timeout_total + 1);
}

TEST(MetricsHttpServerTest, BuiltInRoutesRejectNonGetMethods) {
    const auto port = reserve_free_port();
    MetricsHttpServer server(
        port,
        []() { return std::string("test_metric 1\n"); }
    );
    server.start();

    std::string response;
    std::runtime_error last_error("connect failed");
    for (int i = 0; i < 30; ++i) {
        try {
            response = request_http(port, "POST /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
            break;
        } catch (const std::runtime_error& ex) {
            last_error = std::runtime_error(ex);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    server.stop();

    if (response.empty()) {
        throw last_error;
    }
    EXPECT_NE(response.find("HTTP/1.1 405 Method Not Allowed"), std::string::npos);
    EXPECT_NE(response.find("Allow: GET, HEAD"), std::string::npos);
}
