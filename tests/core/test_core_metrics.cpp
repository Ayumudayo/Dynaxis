#include <gtest/gtest.h>
#include <server/core/metrics/build_info.hpp>
#include <server/core/metrics/metrics.hpp>
#include <server/core/metrics/http_server.hpp>
#include <server/core/runtime_metrics.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <sstream>
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

std::optional<std::string> metric_line_value(const std::string& body, const std::string& metric_name) {
    std::size_t line_start = 0;
    while (line_start < body.size()) {
        auto line_end = body.find('\n', line_start);
        if (line_end == std::string::npos) {
            line_end = body.size();
        }

        const auto line = std::string_view(body).substr(line_start, line_end - line_start);
        if (!line.empty() && line.front() != '#') {
            const std::string needle = metric_name + " ";
            if (line.rfind(needle, 0) == 0) {
                return std::string(line.substr(needle.size()));
            }
        }

        line_start = line_end + 1;
    }
    return std::nullopt;
}

} // namespace

// Metrics API backend 동작 테스트
TEST(MetricsTest, BasicApi) {
    reset_for_tests();

    Counter& counter = get_counter("test_counter_total");
    counter.inc(1.5, {{"route", "login"}});
    counter.inc(2.0, {{"route", "login"}});

    Gauge& gauge = get_gauge("test_gauge");
    gauge.set(10.0);
    gauge.inc(2.0);
    gauge.dec(1.0);

    Histogram& histogram = get_histogram("test_histogram");
    histogram.observe(3.0, {{"route", "login"}});
    histogram.observe(11.0, {{"route", "login"}});

    std::ostringstream out;
    append_prometheus_metrics(out);
    const std::string text = out.str();

    EXPECT_NE(text.find("# TYPE test_counter_total counter"), std::string::npos);
    EXPECT_NE(text.find("test_counter_total{route=\"login\"} 3.5"), std::string::npos);

    EXPECT_NE(text.find("# TYPE test_gauge gauge"), std::string::npos);
    EXPECT_NE(text.find("test_gauge 11"), std::string::npos);

    EXPECT_NE(text.find("# TYPE test_histogram histogram"), std::string::npos);
    EXPECT_NE(text.find("test_histogram_count{route=\"login\"} 2"), std::string::npos);
    EXPECT_NE(text.find("test_histogram_sum{route=\"login\"} 14"), std::string::npos);
}

TEST(MetricsTest, RuntimeCoreMetricsExposeSnapshotValues) {
    const auto before = server::core::runtime_metrics::snapshot();

    server::core::runtime_metrics::record_session_start();
    server::core::runtime_metrics::record_dispatch_attempt(true, std::chrono::milliseconds(1));
    server::core::runtime_metrics::record_send_queue_drop();

    std::ostringstream out;
    append_runtime_core_metrics(out);
    const std::string text = out.str();

    const auto session_started_value = metric_line_value(text, "core_runtime_session_started_total");
    ASSERT_TRUE(session_started_value.has_value());
    EXPECT_EQ(*session_started_value, std::to_string(before.session_started_total + 1));

    const auto dispatch_total_value = metric_line_value(text, "core_runtime_dispatch_total");
    ASSERT_TRUE(dispatch_total_value.has_value());
    EXPECT_EQ(*dispatch_total_value, std::to_string(before.dispatch_total + 1));

    const auto send_queue_drop_value = metric_line_value(text, "core_runtime_send_queue_drop_total");
    ASSERT_TRUE(send_queue_drop_value.has_value());
    EXPECT_EQ(*send_queue_drop_value, std::to_string(before.send_queue_drop_total + 1));
}

TEST(RuntimeMetricsTest, WriteTimeoutCounterIncrements) {
    const auto before = server::core::runtime_metrics::snapshot();
    server::core::runtime_metrics::record_session_write_timeout();
    const auto after = server::core::runtime_metrics::snapshot();

    EXPECT_EQ(after.session_write_timeout_total, before.session_write_timeout_total + 1);
}

TEST(RuntimeMetricsTest, DispatchProcessingPlaceCountersIncrement) {
    const auto before = server::core::runtime_metrics::snapshot();

    server::core::runtime_metrics::record_dispatch_processing_place_call(1);
    server::core::runtime_metrics::record_dispatch_processing_place_reject(1);
    server::core::runtime_metrics::record_dispatch_processing_place_exception(1);

    const auto after = server::core::runtime_metrics::snapshot();

    EXPECT_EQ(after.dispatch_processing_place_calls_total[1],
              before.dispatch_processing_place_calls_total[1] + 1);
    EXPECT_EQ(after.dispatch_processing_place_reject_total[1],
              before.dispatch_processing_place_reject_total[1] + 1);
    EXPECT_EQ(after.dispatch_processing_place_exception_total[1],
              before.dispatch_processing_place_exception_total[1] + 1);
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

TEST(MetricsHttpServerTest, MetricsEndpointIncludesCommonAndApiMetrics) {
    reset_for_tests();

    get_counter("metrics_smoke_total").inc(2.0, {{"component", "core"}});
    server::core::runtime_metrics::record_session_start();

    const auto port = reserve_free_port();
    MetricsHttpServer server(
        port,
        []() {
            std::ostringstream stream;
            server::core::metrics::append_build_info(stream);
            server::core::metrics::append_runtime_core_metrics(stream);
            server::core::metrics::append_prometheus_metrics(stream);
            return stream.str();
        }
    );
    server.start();

    std::string response;
    std::runtime_error last_error("connect failed");
    for (int i = 0; i < 30; ++i) {
        try {
            response = request_http(port, "GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
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

    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(response.find("knights_build_info"), std::string::npos);
    EXPECT_NE(response.find("core_runtime_session_started_total"), std::string::npos);
    EXPECT_NE(response.find("metrics_smoke_total{component=\"core\"} 2"), std::string::npos);
}
