#include "server/app/metrics_server.hpp"
#include "server/core/util/log.hpp"
#include "server/core/metrics/build_info.hpp"
#include "server/core/app/app_host.hpp"
#include "server/core/runtime_metrics.hpp"
#include "server/core/protocol/system_opcodes.hpp"
#include "server/protocol/game_opcodes.hpp"
#include "server/core/util/service_registry.hpp"
#include "server/chat/chat_service.hpp"
#include <boost/asio/streambuf.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <string_view>

namespace server::app {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
namespace corelog = server::core::log;
namespace services = server::core::util::services;

// 전역 메트릭 변수 (bootstrap.cpp에서 정의됨, extern으로 참조)
extern std::atomic<std::uint64_t> g_subscribe_total;
extern std::atomic<std::uint64_t> g_self_echo_drop_total;
extern std::atomic<long long>     g_subscribe_last_lag_ms;

MetricsServer::MetricsServer(unsigned short port)
    : port_(port) {
    io_context_ = std::make_shared<asio::io_context>();
    acceptor_ = std::make_shared<tcp::acceptor>(*io_context_, tcp::endpoint(tcp::v4(), port_));
}

MetricsServer::~MetricsServer() {
    stop();
}

// 메트릭 서버 시작
// 별도의 스레드에서 HTTP 요청을 처리하여 메인 로직에 영향을 주지 않습니다.
// Prometheus가 주기적으로 /metrics 엔드포인트를 긁어갈 수 있도록 합니다.
void MetricsServer::start() {
    corelog::info(std::string("Metrics listening on :") + std::to_string(port_));
    do_accept();
    thread_ = std::make_unique<std::thread>([this]() {
        try {
            io_context_->run();
        } catch (const std::exception& e) {
            corelog::error(std::string("Metrics server exception: ") + e.what());
        }
    });
}

void MetricsServer::stop() {
    if (io_context_) {
        io_context_->stop();
    }
    if (thread_ && thread_->joinable()) {
        thread_->join();
    }
}

void MetricsServer::do_accept() {
    auto sock = std::make_shared<tcp::socket>(*io_context_);
    acceptor_->async_accept(*sock, [this, sock](const boost::system::error_code& ec) {
        if (!ec) {
            asio::post(io_context_->get_executor(), [sock]() {
                try {
                    asio::streambuf request_buf;
                    boost::system::error_code read_ec;
                    asio::read_until(*sock, request_buf, "\r\n\r\n", read_ec);
                    if (read_ec && read_ec != asio::error::eof) {
                        boost::system::error_code ec;
                        sock->close(ec);
                        return;
                    }

                    std::istream request_stream(&request_buf);
                    std::string request_line;
                    std::getline(request_stream, request_line);
                    if (!request_line.empty() && request_line.back() == '\r') request_line.pop_back();

                    std::string method;
                    std::string target;
                    {
                        std::istringstream line_stream(request_line);
                        line_stream >> method >> target;
                    }
                    if (target.empty()) target = "/metrics";

                    std::string body;
                    std::string status = "200 OK";
                    std::string content_type = "text/plain; version=0.0.4";

                    if (target == "/logs") {
                        content_type = "text/plain; charset=utf-8";
                        auto logs = corelog::recent(200);
                        std::ostringstream body_stream;
                        if (logs.empty()) {
                            body_stream << "(no log entries)\n";
                        } else {
                            for (const auto& line : logs) {
                                body_stream << line << '\n';
                            }
                        }
                        body = body_stream.str();
                    } else if (target == "/healthz" || target == "/health") {
                        bool ok = true;
                        if (auto host = services::get<server::core::app::AppHost>()) {
                            ok = host->healthy() && !host->stop_requested();
                        }
                        status = ok ? "200 OK" : "503 Service Unavailable";
                        content_type = "text/plain; charset=utf-8";
                        body = ok ? "ok\n" : "unhealthy\n";
                    } else if (target == "/readyz" || target == "/ready") {
                        bool ok = false;
                        if (auto host = services::get<server::core::app::AppHost>()) {
                            ok = host->ready() && host->healthy() && !host->stop_requested();
                        } else {
                            ok = (services::get<server::app::chat::ChatService>() != nullptr);
                        }
                        status = ok ? "200 OK" : "503 Service Unavailable";
                        content_type = "text/plain; charset=utf-8";
                        body = ok ? "ready\n" : "not ready\n";
                    } else if (target == "/metrics" || target == "/") {
                        auto snap = server::core::runtime_metrics::snapshot();
                        std::ostringstream stream;

                        // Build metadata (git hash/describe + build time)
                        server::core::metrics::append_build_info(stream);

                        auto append_counter = [&](const char* name, std::uint64_t value) {
                            stream << "# TYPE " << name << " counter\n" << name << ' ' << value << '\n';
                        };
                        auto append_gauge = [&](const char* name, long double value) {
                            stream << "# TYPE " << name << " gauge\n" << name << ' ' << std::fixed << std::setprecision(3) << value << '\n';
                            stream << std::defaultfloat << std::setprecision(6);
                        };

                        append_counter("chat_subscribe_total", g_subscribe_total.load());
                        append_counter("chat_self_echo_drop_total", g_self_echo_drop_total.load());
                        append_gauge("chat_subscribe_last_lag_ms", static_cast<long double>(g_subscribe_last_lag_ms.load()));

                        append_counter("chat_accept_total", snap.accept_total);
                        append_counter("chat_session_started_total", snap.session_started_total);
                        append_counter("chat_session_stopped_total", snap.session_stopped_total);
                        append_counter("chat_session_timeout_total", snap.session_timeout_total);
                        append_counter("chat_heartbeat_timeout_total", snap.heartbeat_timeout_total);
                        append_counter("chat_send_queue_drop_total", snap.send_queue_drop_total);
                        append_gauge("chat_session_active", static_cast<long double>(snap.session_active));
                        append_counter("chat_frame_total", snap.packet_total);
                        append_counter("chat_frame_error_total", snap.packet_error_total);
                        append_counter("chat_frame_payload_sum_bytes", snap.packet_payload_sum_bytes);
                        append_counter("chat_frame_payload_count", snap.packet_payload_count);
                        auto payload_avg = snap.packet_payload_count ? (static_cast<long double>(snap.packet_payload_sum_bytes) / static_cast<long double>(snap.packet_payload_count)) : 0.0L;
                        append_gauge("chat_frame_payload_avg_bytes", payload_avg);
                        append_gauge("chat_frame_payload_max_bytes", static_cast<long double>(snap.packet_payload_max_bytes));
                        append_counter("chat_dispatch_total", snap.dispatch_total);
                        append_counter("chat_dispatch_unknown_total", snap.dispatch_unknown_total);
                        append_counter("chat_dispatch_exception_total", snap.dispatch_exception_total);

                        auto last_ms = static_cast<long double>(snap.dispatch_latency_last_ns) / 1'000'000.0L;
                        auto max_ms = static_cast<long double>(snap.dispatch_latency_max_ns) / 1'000'000.0L;
                        auto sum_ms = static_cast<long double>(snap.dispatch_latency_sum_ns) / 1'000'000.0L;
                        auto avg_ms = snap.dispatch_latency_count ? (sum_ms / static_cast<long double>(snap.dispatch_latency_count)) : 0.0L;
                        append_gauge("chat_dispatch_last_latency_ms", last_ms);
                        append_gauge("chat_dispatch_max_latency_ms", max_ms);
                        append_gauge("chat_dispatch_latency_sum_ms", sum_ms);
                        append_gauge("chat_dispatch_latency_avg_ms", avg_ms);
                        append_counter("chat_dispatch_latency_count", snap.dispatch_latency_count);

                        // Dispatch latency histogram (for p95/p99 etc.)
                        stream << "# TYPE chat_dispatch_latency_ms histogram\n";
                        std::uint64_t bucket_cumulative = 0;
                        for (std::size_t i = 0; i < snap.dispatch_latency_bucket_counts.size(); ++i) {
                            bucket_cumulative += snap.dispatch_latency_bucket_counts[i];
                            auto le_ms = static_cast<long double>(server::core::runtime_metrics::kDispatchLatencyBucketUpperBoundsNs[i]) / 1'000'000.0L;
                            stream << "chat_dispatch_latency_ms_bucket{le=\"";
                            stream << std::fixed << std::setprecision(3) << le_ms;
                            stream << "\"} " << bucket_cumulative << "\n";
                            stream << std::defaultfloat << std::setprecision(6);
                        }
                        stream << "chat_dispatch_latency_ms_bucket{le=\"+Inf\"} " << snap.dispatch_latency_count << "\n";
                        stream << "chat_dispatch_latency_ms_sum " << std::fixed << std::setprecision(3) << sum_ms << "\n";
                        stream << std::defaultfloat << std::setprecision(6);
                        stream << "chat_dispatch_latency_ms_count " << snap.dispatch_latency_count << "\n";
                        append_gauge("chat_job_queue_depth", static_cast<long double>(snap.job_queue_depth));
                        append_gauge("chat_job_queue_depth_peak", static_cast<long double>(snap.job_queue_depth_peak));
                        append_gauge("chat_job_queue_capacity", static_cast<long double>(snap.job_queue_capacity));
                        append_counter("chat_job_queue_reject_total", snap.job_queue_reject_total);
                        append_counter("chat_job_queue_push_wait_ns_total", snap.job_queue_push_wait_sum_ns);
                        append_counter("chat_job_queue_push_wait_total", snap.job_queue_push_wait_count);
                        append_gauge("chat_job_queue_push_wait_max_ms", static_cast<long double>(snap.job_queue_push_wait_max_ns) / 1'000'000.0L);
                        append_gauge("chat_db_job_queue_depth", static_cast<long double>(snap.db_job_queue_depth));
                        append_gauge("chat_db_job_queue_depth_peak", static_cast<long double>(snap.db_job_queue_depth_peak));
                        append_gauge("chat_db_job_queue_capacity", static_cast<long double>(snap.db_job_queue_capacity));
                        append_counter("chat_db_job_queue_reject_total", snap.db_job_queue_reject_total);
                        append_counter("chat_db_job_queue_push_wait_ns_total", snap.db_job_queue_push_wait_sum_ns);
                        append_counter("chat_db_job_queue_push_wait_total", snap.db_job_queue_push_wait_count);
                        append_gauge("chat_db_job_queue_push_wait_max_ms", static_cast<long double>(snap.db_job_queue_push_wait_max_ns) / 1'000'000.0L);
                        append_counter("chat_db_job_processed_total", snap.db_job_processed_total);
                        append_counter("chat_db_job_failed_total", snap.db_job_failed_total);
                        append_gauge("chat_memory_pool_capacity", static_cast<long double>(snap.memory_pool_capacity));
                        append_gauge("chat_memory_pool_in_use", static_cast<long double>(snap.memory_pool_in_use));
                        append_gauge("chat_memory_pool_in_use_peak", static_cast<long double>(snap.memory_pool_in_use_peak));

                        if (!snap.opcode_counts.empty()) {
                            stream << "# TYPE chat_dispatch_opcode_total counter\n";
                            for (const auto& [opcode, count] : snap.opcode_counts) {
                                std::ostringstream label;
                                label << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << opcode;
                                stream << "chat_dispatch_opcode_total{opcode=\"0x" << label.str() << "\"} " << count << "\n";
                            }

                            // Same counter, but with stable, human-readable opcode names.
                            // Keep the original metric intact to avoid breaking existing dashboards.
                            stream << "# TYPE chat_dispatch_opcode_named_total counter\n";
                            for (const auto& [opcode, count] : snap.opcode_counts) {
                                std::string_view name = server::protocol::opcode_name(opcode);
                                if (name.empty()) {
                                    name = server::core::protocol::opcode_name(opcode);
                                }
                                if (name.empty()) {
                                    name = "unknown";
                                }

                                std::ostringstream label;
                                label << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << opcode;
                                stream << "chat_dispatch_opcode_named_total{opcode=\"0x" << label.str() << "\",name=\"" << name << "\"} " << count << "\n";
                            }
                        }

                        // Chat hook plugins (hot-reloadable shared libraries)
                        {
                            const auto escape_label_value = [](std::string_view s) {
                                std::string out;
                                out.reserve(s.size());
                                for (const char c : s) {
                                    switch (c) {
                                    case '\\': out += "\\\\"; break;
                                    case '"': out += "\\\""; break;
                                    case '\n': out += "\\n"; break;
                                    default: out.push_back(c); break;
                                    }
                                }
                                return out;
                            };

                            const auto append_gauge_labeled = [&](const char* name,
                                                                  std::string_view labels,
                                                                  long double value) {
                                stream << name << '{' << labels << "} ";
                                stream << std::fixed << std::setprecision(3) << value << "\n";
                                stream << std::defaultfloat << std::setprecision(6);
                            };

                            const auto append_counter_labeled = [&](const char* name,
                                                                    std::string_view labels,
                                                                    std::uint64_t value) {
                                stream << name << '{' << labels << "} " << value << "\n";
                            };

                            server::app::chat::ChatService::ChatHookPluginsMetrics pm;
                            bool have_pm = false;
                            if (auto chat = services::get<server::app::chat::ChatService>()) {
                                pm = chat->chat_hook_plugins_metrics();
                                have_pm = true;
                            } else {
                                pm.enabled = false;
                                pm.mode = "none";
                            }

                            stream << "# TYPE chat_hook_plugins_enabled gauge\n";
                            stream << "chat_hook_plugins_enabled{mode=\"";
                            stream << escape_label_value(pm.mode);
                            stream << "\"} " << (pm.enabled ? 1 : 0) << "\n";

                            stream << "# TYPE chat_hook_plugins_count gauge\n";
                            stream << "chat_hook_plugins_count " << static_cast<long double>(pm.plugins.size()) << "\n";

                            std::size_t loaded = 0;
                            for (const auto& p : pm.plugins) {
                                if (p.loaded) {
                                    ++loaded;
                                }
                            }
                            stream << "# TYPE chat_hook_plugins_loaded gauge\n";
                            stream << "chat_hook_plugins_loaded " << static_cast<long double>(loaded) << "\n";

                            if (have_pm && !pm.plugins.empty()) {
                                stream << "# TYPE chat_hook_plugin_loaded gauge\n";
                                stream << "# TYPE chat_hook_plugin_order gauge\n";
                                stream << "# TYPE chat_hook_plugin_info gauge\n";
                                stream << "# TYPE chat_hook_plugin_reload_attempt_total counter\n";
                                stream << "# TYPE chat_hook_plugin_reload_success_total counter\n";
                                stream << "# TYPE chat_hook_plugin_reload_failure_total counter\n";

                                for (std::size_t i = 0; i < pm.plugins.size(); ++i) {
                                    const auto& p = pm.plugins[i];
                                    if (p.file.empty()) {
                                        continue;
                                    }

                                    const auto file = escape_label_value(p.file);
                                    std::string labels = std::string("file=\"") + file + "\"";

                                    append_gauge_labeled("chat_hook_plugin_loaded", labels, p.loaded ? 1.0L : 0.0L);
                                    append_gauge_labeled("chat_hook_plugin_order", labels, static_cast<long double>(i + 1));

                                    append_counter_labeled("chat_hook_plugin_reload_attempt_total", labels, p.reload_attempt_total);
                                    append_counter_labeled("chat_hook_plugin_reload_success_total", labels, p.reload_success_total);
                                    append_counter_labeled("chat_hook_plugin_reload_failure_total", labels, p.reload_failure_total);

                                    if (p.loaded) {
                                        std::string info_labels = labels;
                                        info_labels += ",name=\"" + escape_label_value(p.name) + "\"";
                                        info_labels += ",version=\"" + escape_label_value(p.version) + "\"";
                                        append_gauge_labeled("chat_hook_plugin_info", info_labels, 1.0L);
                                    }
                                }
                            }
                        }

                        stream << std::setfill(' ') << std::dec << std::nouppercase;
                        body = stream.str();
                    } else {
                        status = "404 Not Found";
                        content_type = "text/plain; charset=utf-8";
                        body = "Not Found\r\n";
                    }

                    std::string hdr = "HTTP/1.1 " + status + "\r\nContent-Type: " + content_type +
                        "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                    std::vector<asio::const_buffer> bufs;
                    bufs.emplace_back(asio::buffer(hdr));
                    if (!body.empty()) {
                        bufs.emplace_back(asio::buffer(body));
                    }
                    asio::write(*sock, bufs);
                    boost::system::error_code ec;
                    sock->shutdown(tcp::socket::shutdown_both, ec);
                    if (ec) ec.clear();
                    sock->close(ec);
                } catch (...) {}
            });
        }
        // 다음 accept
        do_accept();
    });
}

} // namespace server::app
