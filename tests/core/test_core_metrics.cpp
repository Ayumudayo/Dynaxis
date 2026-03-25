#include <gtest/gtest.h>
#include <server/core/metrics/build_info.hpp>
#include <server/core/metrics/metrics.hpp>
#include <server/core/metrics/http_server.hpp>
#include <server/core/runtime_metrics.hpp>
#include <server/core/trace/context.hpp>
#include <server/core/util/log.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

/**
 * @brief 메트릭 API/no-op 동작과 Metrics HTTP 라우트 동작을 검증합니다.
 */
using namespace server::core::metrics;

namespace {

void set_env_value(const char* key, const char* value) {
#if defined(_WIN32)
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}

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

std::string request_http_with_retry(unsigned short port, const std::string& raw_request) {
    std::runtime_error last_error("connect failed");
    for (int i = 0; i < 30; ++i) {
        try {
            return request_http(port, raw_request);
        } catch (const std::runtime_error& ex) {
            last_error = std::runtime_error(ex);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    throw last_error;
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

TEST(RuntimeMetricsTest, ExtendedRuntimeCountersAreSnapshotted) {
    const auto before = server::core::runtime_metrics::snapshot();

    server::core::runtime_metrics::record_exception_recoverable();
    server::core::runtime_metrics::record_exception_fatal();
    server::core::runtime_metrics::record_exception_ignored();
    server::core::runtime_metrics::record_log_async_queue_drop();
    server::core::runtime_metrics::record_http_bad_request();
    server::core::runtime_metrics::record_runtime_setting_reload_attempt();

    const auto after = server::core::runtime_metrics::snapshot();

    EXPECT_EQ(after.exception_recoverable_total, before.exception_recoverable_total + 1);
    EXPECT_EQ(after.exception_fatal_total, before.exception_fatal_total + 1);
    EXPECT_EQ(after.exception_ignored_total, before.exception_ignored_total + 1);
    EXPECT_EQ(after.log_async_queue_drop_total, before.log_async_queue_drop_total + 1);
    EXPECT_EQ(after.http_bad_request_total, before.http_bad_request_total + 1);
    EXPECT_EQ(after.runtime_setting_reload_attempt_total, before.runtime_setting_reload_attempt_total + 1);
}

TEST(RuntimeMetricsTest, WatchdogAndLivenessSignalsAreSnapshotted) {
    server::core::runtime_metrics::set_liveness_state(
        server::core::runtime_metrics::LivenessState::kRunning);
    server::core::runtime_metrics::record_watchdog_sample("scheduler-poll-loop", false, "scheduler-loop-stalled");
    server::core::runtime_metrics::record_watchdog_sample("scheduler-poll-loop", true, "scheduler-loop-recovered");

    const auto after = server::core::runtime_metrics::snapshot();
    const auto watchdogs = server::core::runtime_metrics::watchdog_snapshot();

    EXPECT_EQ(after.liveness_state, server::core::runtime_metrics::LivenessState::kRunning);
    EXPECT_GE(after.watchdog_total, 1u);
    EXPECT_GE(after.watchdog_transition_total, 1u);

    auto it = std::find_if(watchdogs.begin(), watchdogs.end(), [](const auto& watchdog) {
        return watchdog.name == "scheduler-poll-loop";
    });
    ASSERT_NE(it, watchdogs.end());
    EXPECT_TRUE(it->healthy);
    EXPECT_EQ(it->detail, "scheduler-loop-recovered");
    EXPECT_GE(it->unhealthy_samples_total, 1u);
}

TEST(RuntimeMetricsTest, FreezeSignalsTriggerDetailedTelemetryCapture) {
    server::core::runtime_metrics::set_liveness_state(
        server::core::runtime_metrics::LivenessState::kDegraded);
    server::core::runtime_metrics::record_watchdog_sample("scheduler-poll-loop", false, "late-1");
    server::core::runtime_metrics::record_watchdog_sample("scheduler-poll-loop", false, "late-2");
    server::core::runtime_metrics::record_watchdog_freeze_suspect(
        "scheduler-poll-loop",
        std::chrono::milliseconds(250),
        std::chrono::milliseconds(100),
        "scheduler-freeze");
    server::core::runtime_metrics::record_exception_recoverable();
    server::core::runtime_metrics::record_dispatch_attempt(true, std::chrono::milliseconds(3));

    const auto after = server::core::runtime_metrics::snapshot();
    const auto detail = server::core::runtime_metrics::detailed_telemetry_snapshot();

    EXPECT_EQ(after.liveness_state, server::core::runtime_metrics::LivenessState::kDegraded);
    EXPECT_GE(after.watchdog_freeze_suspect_total, 1u);
    EXPECT_GE(after.detailed_telemetry_activation_total, 1u);
    EXPECT_TRUE(detail.active);
    EXPECT_FALSE(detail.trigger.empty());
    EXPECT_GE(detail.captured_exception_total, 1u);
    EXPECT_GE(detail.captured_dispatch_latency_max_ns, static_cast<std::uint64_t>(3'000'000));
}

TEST(RuntimeMetricsTest, RudpCountersAreSnapshotted) {
    const auto before = server::core::runtime_metrics::snapshot();

    server::core::runtime_metrics::record_rudp_handshake_result(true);
    server::core::runtime_metrics::record_rudp_handshake_result(false);
    server::core::runtime_metrics::record_rudp_retransmit(2);
    server::core::runtime_metrics::set_rudp_inflight_packets(7);
    server::core::runtime_metrics::record_rudp_rtt_ms(42);
    server::core::runtime_metrics::record_rudp_fallback(server::core::runtime_metrics::RudpFallbackReason::kHandshakeTimeout);

    const auto after = server::core::runtime_metrics::snapshot();
    EXPECT_EQ(after.rudp_handshake_ok_total, before.rudp_handshake_ok_total + 1);
    EXPECT_EQ(after.rudp_handshake_fail_total, before.rudp_handshake_fail_total + 1);
    EXPECT_EQ(after.rudp_retransmit_total, before.rudp_retransmit_total + 2);
    EXPECT_EQ(after.rudp_inflight_packets, 7u);
    EXPECT_EQ(after.rudp_rtt_ms_count, before.rudp_rtt_ms_count + 1);
    EXPECT_GE(after.rudp_rtt_ms_sum, before.rudp_rtt_ms_sum + 42);
    EXPECT_EQ(
        after.rudp_fallback_total[static_cast<std::size_t>(server::core::runtime_metrics::RudpFallbackReason::kHandshakeTimeout)],
        before.rudp_fallback_total[static_cast<std::size_t>(server::core::runtime_metrics::RudpFallbackReason::kHandshakeTimeout)] + 1);
}

TEST(MetricsTest, RuntimeCoreMetricsIncludeExtendedSignals) {
    server::core::runtime_metrics::record_exception_recoverable();
    server::core::runtime_metrics::record_log_async_queue_drop();
    server::core::runtime_metrics::record_log_async_flush_latency(std::chrono::microseconds(100));
    server::core::runtime_metrics::record_http_auth_reject();
    server::core::runtime_metrics::record_runtime_setting_reload_attempt();
    server::core::runtime_metrics::record_runtime_setting_reload_success();

    std::ostringstream out;
    append_runtime_core_metrics(out);
    const std::string text = out.str();

    EXPECT_NE(text.find("core_runtime_exception_recoverable_total"), std::string::npos);
    EXPECT_NE(text.find("core_runtime_log_async_queue_drop_total"), std::string::npos);
    EXPECT_NE(text.find("core_runtime_log_async_flush_total"), std::string::npos);
    EXPECT_NE(text.find("core_runtime_http_auth_reject_total"), std::string::npos);
    EXPECT_NE(text.find("core_runtime_setting_reload_success_total"), std::string::npos);
}

TEST(MetricsTest, RuntimeCoreMetricsIncludeWatchdogAndDetailedTelemetrySignals) {
    server::core::runtime_metrics::set_liveness_state(
        server::core::runtime_metrics::LivenessState::kRunning);
    server::core::runtime_metrics::record_watchdog_sample("scheduler-poll-loop", false, "late-1");
    server::core::runtime_metrics::record_watchdog_sample("scheduler-poll-loop", false, "late-2");
    server::core::runtime_metrics::record_watchdog_freeze_suspect(
        "scheduler-poll-loop",
        std::chrono::milliseconds(250),
        std::chrono::milliseconds(100),
        "scheduler-freeze");
    server::core::runtime_metrics::record_exception_recoverable();

    std::ostringstream out;
    append_runtime_core_metrics(out);
    const std::string text = out.str();

    EXPECT_NE(text.find("core_runtime_liveness_state{state=\"degraded\"} 1"), std::string::npos);
    EXPECT_NE(text.find("core_runtime_watchdog_total"), std::string::npos);
    EXPECT_NE(text.find("core_runtime_watchdog_freeze_suspect_total"), std::string::npos);
    EXPECT_NE(text.find("core_runtime_watchdog_status{watchdog=\"scheduler-poll-loop\"} 0"), std::string::npos);
    EXPECT_NE(text.find("core_runtime_detailed_telemetry_activation_total"), std::string::npos);
    EXPECT_NE(text.find("core_runtime_detailed_telemetry_active 1"), std::string::npos);
}

TEST(MetricsTest, RuntimeCoreMetricsIncludeRudpSignals) {
    server::core::runtime_metrics::record_rudp_handshake_result(true);
    server::core::runtime_metrics::record_rudp_handshake_result(false);
    server::core::runtime_metrics::record_rudp_retransmit();
    server::core::runtime_metrics::set_rudp_inflight_packets(3);
    server::core::runtime_metrics::record_rudp_rtt_ms(10);
    server::core::runtime_metrics::record_rudp_fallback(server::core::runtime_metrics::RudpFallbackReason::kProtocolError);

    std::ostringstream out;
    append_runtime_core_metrics(out);
    const std::string text = out.str();

    EXPECT_NE(text.find("core_runtime_rudp_handshake_total{result=\"ok\"}"), std::string::npos);
    EXPECT_NE(text.find("core_runtime_rudp_handshake_total{result=\"fail\"}"), std::string::npos);
    EXPECT_NE(text.find("core_runtime_rudp_retransmit_total"), std::string::npos);
    EXPECT_NE(text.find("core_runtime_rudp_inflight_packets"), std::string::npos);
    EXPECT_NE(text.find("core_runtime_rudp_rtt_ms_bucket"), std::string::npos);
    EXPECT_NE(text.find("core_runtime_rudp_fallback_total{reason=\"protocol_error\"}"), std::string::npos);
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

TEST(BuildInfoMetricsTest, AppendBuildInfoEmitsRuntimeBuildInfoMetric) {
    std::ostringstream out;
    append_build_info(out);
    const std::string text = out.str();

    EXPECT_NE(text.find("# TYPE runtime_build_info gauge"), std::string::npos);
    EXPECT_NE(text.find("runtime_build_info{git_hash=\""), std::string::npos);
    EXPECT_NE(text.find(",git_describe=\""), std::string::npos);
    EXPECT_NE(text.find(",build_time_utc=\""), std::string::npos);
    EXPECT_NE(text.find("} 1\n"), std::string::npos);
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
    EXPECT_NE(response.find("runtime_build_info"), std::string::npos);
    EXPECT_NE(response.find("core_runtime_session_started_total"), std::string::npos);
    EXPECT_NE(response.find("metrics_smoke_total{component=\"core\"} 2"), std::string::npos);
}

TEST(MetricsHttpServerTest, HealthAndReadyEndpointsUseConfiguredCallbacksAndBodies) {
    set_env_value("METRICS_HTTP_AUTH_TOKEN", "");
    set_env_value("METRICS_HTTP_ALLOWLIST", "");

    const auto port = reserve_free_port();
    MetricsHttpServer server(
        port,
        []() { return std::string("test_metric 1\n"); },
        []() { return false; },
        []() { return true; },
        {},
        [](bool ok) { return ok ? std::string("healthy\n") : std::string("unhealthy\n"); },
        [](bool ok) { return ok ? std::string("ready\n") : std::string("not ready\n"); });
    server.start();

    const std::string health = request_http_with_retry(
        port,
        "GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_NE(health.find("HTTP/1.1 503 Service Unavailable"), std::string::npos);
    EXPECT_NE(health.find("unhealthy\n"), std::string::npos);

    const std::string ready = request_http_with_retry(
        port,
        "GET /readyz HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_NE(ready.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(ready.find("ready\n"), std::string::npos);

    server.stop();
}

TEST(MetricsHttpServerTest, CustomRouteCanReadRequestBody) {
    const auto port = reserve_free_port();
    MetricsHttpServer server(
        port,
        []() { return std::string("test_metric 1\n"); },
        {},
        {},
        {},
        {},
        {},
        [](const MetricsHttpServer::HttpRequest& request)
            -> std::optional<MetricsHttpServer::RouteResponse> {
            if (request.target != "/custom") {
                return std::nullopt;
            }
            return MetricsHttpServer::RouteResponse{
                "200 OK",
                "text/plain; charset=utf-8",
                request.body.empty() ? std::string("(empty)") : request.body,
            };
        });
    server.start();

    std::string response;
    std::runtime_error last_error("connect failed");
    for (int i = 0; i < 30; ++i) {
        try {
            response = request_http(
                port,
                "POST /custom HTTP/1.1\r\nHost: localhost\r\nContent-Length: 11\r\n\r\nhello world");
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
    EXPECT_NE(response.find("hello world"), std::string::npos);
}

TEST(MetricsHttpServerTest, RejectsUnauthorizedWhenTokenConfigured) {
    set_env_value("METRICS_HTTP_AUTH_TOKEN", "metrics-secret-token");
    set_env_value("METRICS_HTTP_ALLOWLIST", "");

    const auto port = reserve_free_port();
    MetricsHttpServer server(
        port,
        []() { return std::string("test_metric 1\n"); }
    );
    server.start();

    const std::string unauthorized = request_http_with_retry(
        port,
        "GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_NE(unauthorized.find("HTTP/1.1 401 Unauthorized"), std::string::npos);
    EXPECT_NE(unauthorized.find("WWW-Authenticate: Bearer"), std::string::npos);

    const std::string authorized = request_http_with_retry(
        port,
        "GET /metrics HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer metrics-secret-token\r\n\r\n");
    EXPECT_NE(authorized.find("HTTP/1.1 200 OK"), std::string::npos);

    server.stop();
    set_env_value("METRICS_HTTP_AUTH_TOKEN", "");
}

TEST(MetricsHttpServerTest, RejectsRequestOutsideAllowlist) {
    set_env_value("METRICS_HTTP_AUTH_TOKEN", "");
    set_env_value("METRICS_HTTP_ALLOWLIST", "192.0.2.10");

    const auto port = reserve_free_port();
    MetricsHttpServer server(
        port,
        []() { return std::string("test_metric 1\n"); }
    );
    server.start();

    const std::string forbidden = request_http_with_retry(
        port,
        "GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_NE(forbidden.find("HTTP/1.1 403 Forbidden"), std::string::npos);

    server.stop();
    set_env_value("METRICS_HTTP_ALLOWLIST", "");
}

TEST(MetricsHttpServerTest, RejectsBodyLargerThanConfiguredLimit) {
    set_env_value("METRICS_HTTP_MAX_BODY_BYTES", "4");
    set_env_value("METRICS_HTTP_AUTH_TOKEN", "");
    set_env_value("METRICS_HTTP_ALLOWLIST", "");

    const auto port = reserve_free_port();
    MetricsHttpServer server(
        port,
        []() { return std::string("test_metric 1\n"); },
        {},
        {},
        {},
        {},
        {},
        [](const MetricsHttpServer::HttpRequest& request)
            -> std::optional<MetricsHttpServer::RouteResponse> {
            if (request.target != "/custom") {
                return std::nullopt;
            }
            return MetricsHttpServer::RouteResponse{
                "200 OK",
                "text/plain; charset=utf-8",
                request.body,
            };
        });
    server.start();

    const std::string response = request_http_with_retry(
        port,
        "POST /custom HTTP/1.1\r\nHost: localhost\r\nContent-Length: 11\r\n\r\nhello world");
    EXPECT_NE(response.find("HTTP/1.1 413 Payload Too Large"), std::string::npos);

    server.stop();
    set_env_value("METRICS_HTTP_MAX_BODY_BYTES", "65536");
}

TEST(LogSchemaMetricsTest, JsonSchemaMetricsExposeParseAndFillSignals) {
    reset_for_tests();

    set_env_value("LOG_FORMAT", "json");
    set_env_value("RUNTIME_TRACING_ENABLED", "1");
    set_env_value("RUNTIME_TRACING_SAMPLE_PERCENT", "100");
    server::core::trace::reset_for_tests();

    server::core::log::set_level(server::core::log::level::debug);
    server::core::log::set_buffer_capacity(64);

    const auto before = server::core::runtime_metrics::snapshot();

    const std::string trace_id = server::core::trace::make_trace_id();
    const std::string correlation_id = "log-schema-correlation";
    server::core::trace::ScopedContext scope(trace_id, correlation_id, true);
    ASSERT_TRUE(scope.active());

    server::core::log::info("component=metrics_test error_code=E_SCHEMA token=secret-value log-schema-metric-marker");

    const std::vector<std::string> lines = server::core::log::recent(64);
    bool found_marker = false;
    for (const auto& line : lines) {
        if (line.find("log-schema-metric-marker") == std::string::npos) {
            continue;
        }
        found_marker = true;
        ASSERT_FALSE(line.empty());
        EXPECT_EQ(line.front(), '{');
        EXPECT_EQ(line.back(), '}');
        EXPECT_NE(line.find("\"component\":\"metrics_test\""), std::string::npos);
        EXPECT_NE(line.find("\"trace_id\":\"" + trace_id + "\""), std::string::npos);
        EXPECT_NE(line.find("\"correlation_id\":\"" + correlation_id + "\""), std::string::npos);
        EXPECT_NE(line.find("\"error_code\":\"E_SCHEMA\""), std::string::npos);
        EXPECT_NE(line.find("token=***"), std::string::npos);
        EXPECT_EQ(line.find("secret-value"), std::string::npos);
        break;
    }
    EXPECT_TRUE(found_marker);

    std::ostringstream out;
    append_prometheus_metrics(out);
    const std::string text = out.str();

    EXPECT_NE(text.find("core_log_schema_records_total{format=\"json\"} 1"), std::string::npos);
    EXPECT_NE(text.find("core_log_schema_parse_success_total{format=\"json\"} 1"), std::string::npos);
    EXPECT_NE(text.find("core_log_schema_field_total{field=\"trace_id\",format=\"json\"} 1"), std::string::npos);
    EXPECT_NE(text.find("core_log_schema_field_filled_total{field=\"trace_id\",format=\"json\"} 1"), std::string::npos);
    EXPECT_NE(text.find("core_log_schema_field_total{field=\"error_code\",format=\"json\"} 1"), std::string::npos);
    EXPECT_NE(text.find("core_log_schema_field_filled_total{field=\"error_code\",format=\"json\"} 1"), std::string::npos);

    const auto after = server::core::runtime_metrics::snapshot();
    EXPECT_GE(after.log_masked_fields_total, before.log_masked_fields_total + 1);

    set_env_value("LOG_FORMAT", "text");
    set_env_value("RUNTIME_TRACING_ENABLED", "0");
    server::core::trace::reset_for_tests();
}
