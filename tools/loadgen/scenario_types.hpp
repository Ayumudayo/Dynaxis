#pragma once

#include "session_client.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace loadgen {

inline constexpr std::uint32_t kCurrentScenarioSchemaVersion = 1;
inline constexpr std::size_t kTransportKindCount = 3;

enum class SessionMode {
    kLoginOnly,
    kChat,
    kPing,
    kFpsInput,
};

struct GroupConfig {
    std::string name;
    TransportKind transport{TransportKind::kTcp};
    SessionMode mode{SessionMode::kLoginOnly};
    std::uint32_t count{0};
    double rate_per_sec{0.0};
    bool join_room{false};
};

struct ScenarioConfig {
    std::uint32_t schema_version{0};
    std::string name;
    std::uint32_t sessions{0};
    std::uint32_t ramp_up_ms{0};
    std::uint32_t duration_ms{0};
    std::string room{"lobby"};
    std::string room_password;
    bool unique_room_per_run{true};
    std::uint32_t message_bytes{64};
    std::string login_prefix{"loadgen"};
    std::uint32_t connect_timeout_ms{5000};
    std::uint32_t read_timeout_ms{5000};
    std::vector<GroupConfig> groups;
};

struct CliOptions {
    std::string host;
    std::uint16_t port{0};
    std::uint16_t udp_port{0};
    std::filesystem::path scenario_path;
    std::filesystem::path report_path;
    std::uint32_t seed{0};
    bool verbose{false};
};

struct TransportBreakdown {
    TransportKind transport{TransportKind::kTcp};
    std::uint32_t sessions{0};
    std::uint32_t connected_sessions{0};
    std::uint32_t authenticated_sessions{0};
    std::uint32_t joined_sessions{0};
    std::uint64_t success_count{0};
    std::uint64_t error_count{0};
    TransportStats stats;
};

struct RunSummary {
    std::string scenario;
    std::string host;
    std::string room;
    std::uint16_t port{0};
    std::uint16_t udp_port{0};
    std::uint32_t seed{0};
    std::vector<std::string> transports;
    std::uint32_t sessions{0};
    std::uint32_t connected_sessions{0};
    std::uint32_t authenticated_sessions{0};
    std::uint32_t joined_sessions{0};
    std::uint64_t elapsed_ms{0};
    std::uint64_t success_count{0};
    std::uint64_t error_count{0};
    std::uint64_t connect_failures{0};
    std::uint64_t login_failures{0};
    std::uint64_t attach_failures{0};
    std::uint64_t join_failures{0};
    std::uint64_t chat_success{0};
    std::uint64_t chat_errors{0};
    std::uint64_t ping_success{0};
    std::uint64_t ping_errors{0};
    std::uint64_t fps_input_success{0};
    std::uint64_t fps_input_errors{0};
    std::uint64_t fps_snapshot_count{0};
    std::uint64_t fps_delta_count{0};
    double throughput_rps{0.0};
    double latency_p50_ms{0.0};
    double latency_p95_ms{0.0};
    double latency_p99_ms{0.0};
    double latency_max_ms{0.0};
    TransportStats transport;
    std::vector<TransportBreakdown> transport_breakdown;
};

inline std::size_t transport_index(TransportKind transport) noexcept {
    switch (transport) {
    case TransportKind::kTcp:
        return 0;
    case TransportKind::kUdp:
        return 1;
    case TransportKind::kRudp:
        return 2;
    }
    return 0;
}

inline std::string transport_name(TransportKind transport) {
    switch (transport) {
    case TransportKind::kTcp:
        return "tcp";
    case TransportKind::kUdp:
        return "udp";
    case TransportKind::kRudp:
        return "rudp";
    }
    return "unknown";
}

inline std::string mode_name(SessionMode mode) {
    switch (mode) {
    case SessionMode::kChat:
        return "chat";
    case SessionMode::kPing:
        return "ping";
    case SessionMode::kFpsInput:
        return "fps_input";
    case SessionMode::kLoginOnly:
        return "login_only";
    }
    return "unknown";
}

}  // namespace loadgen
