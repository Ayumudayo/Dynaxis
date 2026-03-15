#include "scenario_loader.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace loadgen {

namespace {

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

SessionMode parse_mode(const std::string& raw) {
    const auto mode = to_lower(raw);
    if (mode == "chat") {
        return SessionMode::kChat;
    }
    if (mode == "ping") {
        return SessionMode::kPing;
    }
    if (mode == "login_only" || mode == "idle") {
        return SessionMode::kLoginOnly;
    }
    throw std::runtime_error("unsupported session mode: " + raw);
}

TransportKind parse_transport(const std::string& raw) {
    const auto transport = to_lower(raw);
    if (transport == "tcp") {
        return TransportKind::kTcp;
    }
    if (transport == "udp") {
        return TransportKind::kUdp;
    }
    if (transport == "rudp") {
        return TransportKind::kRudp;
    }
    throw std::runtime_error("unsupported transport: " + raw);
}

void print_usage() {
    std::cerr
        << "Usage: stack_loadgen --host <host> --port <port> [--udp-port <port>] "
        << "--scenario <path> --report <path> [--seed <u32>] [--verbose]\n";
}

void validate_group(const GroupConfig& group) {
    if (group.count == 0) {
        throw std::runtime_error("group '" + group.name + "' count must be > 0");
    }

    if ((group.mode == SessionMode::kChat || group.mode == SessionMode::kPing) && group.rate_per_sec <= 0.0) {
        throw std::runtime_error("chat/ping groups require rate_per_sec > 0");
    }

    if (group.mode == SessionMode::kLoginOnly && group.rate_per_sec != 0.0) {
        throw std::runtime_error("login_only groups must not set rate_per_sec");
    }

    if (group.transport != TransportKind::kTcp) {
        if (group.mode == SessionMode::kChat) {
            throw std::runtime_error(
                "transport '" + transport_name(group.transport)
                + "' currently supports only login_only or ping groups");
        }
        if (group.join_room) {
            throw std::runtime_error(
                "transport '" + transport_name(group.transport) + "' does not support join_room yet");
        }
    }
}

}  // namespace

CliOptions parse_cli(int argc, char** argv) {
    CliOptions options;
    options.seed = static_cast<std::uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count() & 0xFFFFFFFFu);

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto require_value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + name);
            }
            return argv[++i];
        };

        if (arg == "--host") {
            options.host = require_value("--host");
            continue;
        }
        if (arg == "--port") {
            const auto parsed = std::stoul(require_value("--port"));
            if (parsed > 65535) {
                throw std::runtime_error("port out of range");
            }
            options.port = static_cast<std::uint16_t>(parsed);
            continue;
        }
        if (arg == "--udp-port") {
            const auto parsed = std::stoul(require_value("--udp-port"));
            if (parsed > 65535) {
                throw std::runtime_error("udp port out of range");
            }
            options.udp_port = static_cast<std::uint16_t>(parsed);
            continue;
        }
        if (arg == "--scenario") {
            options.scenario_path = require_value("--scenario");
            continue;
        }
        if (arg == "--report") {
            options.report_path = require_value("--report");
            continue;
        }
        if (arg == "--seed") {
            options.seed = static_cast<std::uint32_t>(std::stoul(require_value("--seed")));
            continue;
        }
        if (arg == "--verbose") {
            options.verbose = true;
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(0);
        }
        throw std::runtime_error("unknown argument: " + std::string(arg));
    }

    if (options.host.empty() || options.port == 0 || options.scenario_path.empty() || options.report_path.empty()) {
        throw std::runtime_error("missing required arguments");
    }
    return options;
}

ScenarioConfig load_scenario(const fs::path& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        throw std::runtime_error("failed to open scenario: " + path.string());
    }

    json document;
    input >> document;

    if (!document.contains("schema_version")) {
        throw std::runtime_error("scenario schema_version is required");
    }

    ScenarioConfig scenario;
    scenario.schema_version = document.at("schema_version").get<std::uint32_t>();
    if (scenario.schema_version != kCurrentScenarioSchemaVersion) {
        throw std::runtime_error(
            "unsupported scenario schema_version: " + std::to_string(scenario.schema_version));
    }

    scenario.name = document.value("name", path.stem().string());
    scenario.sessions = document.at("sessions").get<std::uint32_t>();
    scenario.ramp_up_ms = document.value("ramp_up_ms", 0u);
    scenario.duration_ms = document.at("duration_ms").get<std::uint32_t>();
    scenario.room = document.value("room", std::string("lobby"));
    scenario.room_password = document.value("room_password", std::string());
    scenario.unique_room_per_run = document.value("unique_room_per_run", true);
    scenario.message_bytes = document.value("message_bytes", 64u);
    scenario.login_prefix = document.value("login_prefix", std::string("loadgen"));
    scenario.connect_timeout_ms = document.value("connect_timeout_ms", 5000u);
    scenario.read_timeout_ms = document.value("read_timeout_ms", 5000u);
    const auto default_transport = parse_transport(document.value("transport", std::string("tcp")));

    if (document.contains("groups")) {
        std::size_t group_index = 0;
        for (const auto& group_json : document.at("groups")) {
            GroupConfig group;
            group.name = group_json.value("name", "group_" + std::to_string(group_index));
            group.transport = parse_transport(group_json.value("transport", transport_name(default_transport)));
            group.mode = parse_mode(group_json.at("mode").get<std::string>());
            group.count = group_json.at("count").get<std::uint32_t>();
            group.rate_per_sec = group_json.value("rate_per_sec", 0.0);
            group.join_room = group_json.value("join_room", group.mode == SessionMode::kChat);
            validate_group(group);
            scenario.groups.push_back(std::move(group));
            ++group_index;
        }
    } else {
        GroupConfig group;
        group.name = "default";
        group.transport = default_transport;
        group.mode = SessionMode::kChat;
        group.count = scenario.sessions;
        group.rate_per_sec = document.value("message_rate_per_sec", 1.0);
        group.join_room = true;
        validate_group(group);
        scenario.groups.push_back(std::move(group));
    }

    if (scenario.sessions == 0) {
        throw std::runtime_error("scenario sessions must be > 0");
    }
    if (scenario.duration_ms == 0) {
        throw std::runtime_error("scenario duration_ms must be > 0");
    }
    if (scenario.message_bytes == 0) {
        throw std::runtime_error("scenario message_bytes must be > 0");
    }
    if (scenario.connect_timeout_ms == 0) {
        throw std::runtime_error("scenario connect_timeout_ms must be > 0");
    }
    if (scenario.read_timeout_ms == 0) {
        throw std::runtime_error("scenario read_timeout_ms must be > 0");
    }
    if (scenario.groups.empty()) {
        throw std::runtime_error("scenario groups must not be empty");
    }

    std::uint32_t total_group_sessions = 0;
    for (const auto& group : scenario.groups) {
        total_group_sessions += group.count;
    }
    if (total_group_sessions != scenario.sessions) {
        throw std::runtime_error("sum(groups.count) must equal sessions");
    }

    return scenario;
}

}  // namespace loadgen
