#include "report_writer.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace loadgen {

namespace {

json transport_stats_json(const TransportStats& stats) {
    return json{
        {"connect_failures", stats.connect_failures},
        {"read_timeouts", stats.read_timeouts},
        {"disconnects", stats.disconnects},
        {"unsupported_operations", stats.unsupported_operations},
        {"bind_ticket_timeouts", stats.bind_ticket_timeouts},
        {"udp_bind_attempts", stats.udp_bind_attempts},
        {"udp_bind_successes", stats.udp_bind_successes},
        {"udp_bind_failures", stats.udp_bind_failures},
        {"rudp_attach_attempts", stats.rudp_attach_attempts},
        {"rudp_attach_successes", stats.rudp_attach_successes},
        {"rudp_attach_fallbacks", stats.rudp_attach_fallbacks},
    };
}

}  // namespace

json to_json(const RunSummary& summary) {
    json transport_breakdown = json::object();
    for (const auto& breakdown : summary.transport_breakdown) {
        transport_breakdown[transport_name(breakdown.transport)] = json{
            {"sessions", breakdown.sessions},
            {"connected_sessions", breakdown.connected_sessions},
            {"authenticated_sessions", breakdown.authenticated_sessions},
            {"joined_sessions", breakdown.joined_sessions},
            {"success_count", breakdown.success_count},
            {"error_count", breakdown.error_count},
            {"stats", transport_stats_json(breakdown.stats)},
        };
    }

    return json{
        {"scenario", summary.scenario},
        {"host", summary.host},
        {"port", summary.port},
        {"udp_port", summary.udp_port},
        {"room", summary.room},
        {"seed", summary.seed},
        {"transports", summary.transports},
        {"sessions", summary.sessions},
        {"connected_sessions", summary.connected_sessions},
        {"authenticated_sessions", summary.authenticated_sessions},
        {"joined_sessions", summary.joined_sessions},
        {"elapsed_ms", summary.elapsed_ms},
        {"success_count", summary.success_count},
        {"error_count", summary.error_count},
        {"setup",
         {
             {"connect_failures", summary.connect_failures},
             {"login_failures", summary.login_failures},
             {"attach_failures", summary.attach_failures},
             {"join_failures", summary.join_failures},
         }},
        {"operations",
         {
             {"chat_success", summary.chat_success},
             {"chat_errors", summary.chat_errors},
             {"ping_success", summary.ping_success},
             {"ping_errors", summary.ping_errors},
         }},
        {"throughput_rps", summary.throughput_rps},
        {"latency_ms",
         {
             {"p50", summary.latency_p50_ms},
             {"p95", summary.latency_p95_ms},
             {"p99", summary.latency_p99_ms},
             {"max", summary.latency_max_ms},
         }},
        {"transport", transport_stats_json(summary.transport)},
        {"transport_breakdown", std::move(transport_breakdown)},
    };
}

void write_report(const fs::path& report_path, const json& report) {
    if (!report_path.parent_path().empty()) {
        fs::create_directories(report_path.parent_path());
    }

    std::ofstream output(report_path);
    if (!output.is_open()) {
        throw std::runtime_error("failed to open report path: " + report_path.string());
    }
    output << std::setw(2) << report << '\n';
}

void print_summary(const RunSummary& summary, const fs::path& report_path) {
    std::cout << "loadgen_summary"
              << " scenario=" << summary.scenario
              << " transports=";
    for (std::size_t i = 0; i < summary.transports.size(); ++i) {
        if (i != 0) {
            std::cout << ",";
        }
        std::cout << summary.transports[i];
    }

    std::cout << " sessions=" << summary.sessions
              << " connected=" << summary.connected_sessions
              << " authenticated=" << summary.authenticated_sessions
              << " joined=" << summary.joined_sessions
              << " success=" << summary.success_count
              << " errors=" << summary.error_count
              << " attach_failures=" << summary.attach_failures
              << " udp_bind_ok=" << summary.transport.udp_bind_successes
              << " udp_bind_fail=" << summary.transport.udp_bind_failures
              << " rudp_attach_ok=" << summary.transport.rudp_attach_successes
              << " rudp_attach_fallback=" << summary.transport.rudp_attach_fallbacks
              << " throughput_rps=" << std::fixed << std::setprecision(2) << summary.throughput_rps
              << " p95_ms=" << summary.latency_p95_ms
              << " report=" << report_path.string()
              << '\n';
}

}  // namespace loadgen
