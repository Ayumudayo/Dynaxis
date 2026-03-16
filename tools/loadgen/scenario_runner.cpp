#include "scenario_runner.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

namespace loadgen {

namespace {

struct GroupAssignment {
    std::string name;
    TransportKind transport{TransportKind::kTcp};
    SessionMode mode{SessionMode::kLoginOnly};
    double rate_per_sec{0.0};
    bool join_room{false};
};

class MetricsCollector {
public:
    MetricsCollector() {
        for (std::size_t i = 0; i < breakdown_.size(); ++i) {
            breakdown_[i].transport = static_cast<TransportKind>(i);
        }
    }

    void record_session_planned(TransportKind transport) {
        std::lock_guard<std::mutex> lock(mu_);
        ++breakdown_[transport_index(transport)].sessions;
    }

    void record_connected(TransportKind transport) {
        std::lock_guard<std::mutex> lock(mu_);
        ++connected_sessions_;
        ++breakdown_[transport_index(transport)].connected_sessions;
    }

    void record_authenticated(TransportKind transport) {
        std::lock_guard<std::mutex> lock(mu_);
        ++authenticated_sessions_;
        ++breakdown_[transport_index(transport)].authenticated_sessions;
    }

    void record_joined(TransportKind transport) {
        std::lock_guard<std::mutex> lock(mu_);
        ++joined_sessions_;
        ++breakdown_[transport_index(transport)].joined_sessions;
    }

    void record_connect_failure(TransportKind transport) {
        std::lock_guard<std::mutex> lock(mu_);
        ++error_count_;
        ++connect_failures_;
        ++breakdown_[transport_index(transport)].error_count;
    }

    void record_login_failure(TransportKind transport) {
        std::lock_guard<std::mutex> lock(mu_);
        ++error_count_;
        ++login_failures_;
        ++breakdown_[transport_index(transport)].error_count;
    }

    void record_attach_failure(TransportKind transport) {
        std::lock_guard<std::mutex> lock(mu_);
        ++error_count_;
        ++attach_failures_;
        ++breakdown_[transport_index(transport)].error_count;
    }

    void record_join_failure(TransportKind transport) {
        std::lock_guard<std::mutex> lock(mu_);
        ++error_count_;
        ++join_failures_;
        ++breakdown_[transport_index(transport)].error_count;
    }

    void record_chat_success(TransportKind transport, double latency_ms) {
        std::lock_guard<std::mutex> lock(mu_);
        ++success_count_;
        ++chat_success_;
        ++breakdown_[transport_index(transport)].success_count;
        latencies_ms_.push_back(latency_ms);
    }

    void record_chat_failure(TransportKind transport) {
        std::lock_guard<std::mutex> lock(mu_);
        ++error_count_;
        ++chat_errors_;
        ++breakdown_[transport_index(transport)].error_count;
    }

    void record_ping_success(TransportKind transport, double latency_ms) {
        std::lock_guard<std::mutex> lock(mu_);
        ++success_count_;
        ++ping_success_;
        ++breakdown_[transport_index(transport)].success_count;
        latencies_ms_.push_back(latency_ms);
    }

    void record_ping_failure(TransportKind transport) {
        std::lock_guard<std::mutex> lock(mu_);
        ++error_count_;
        ++ping_errors_;
        ++breakdown_[transport_index(transport)].error_count;
    }

    void record_fps_update(const FpsUpdateResult& update) {
        std::lock_guard<std::mutex> lock(mu_);
        if (update.is_snapshot) {
            ++fps_snapshot_count_;
        } else {
            ++fps_delta_count_;
        }
    }

    void record_fps_input_success(TransportKind transport, double latency_ms, const FpsUpdateResult& update) {
        std::lock_guard<std::mutex> lock(mu_);
        ++success_count_;
        ++fps_input_success_;
        ++breakdown_[transport_index(transport)].success_count;
        if (update.is_snapshot) {
            ++fps_snapshot_count_;
        } else {
            ++fps_delta_count_;
        }
        latencies_ms_.push_back(latency_ms);
    }

    void record_fps_input_failure(TransportKind transport) {
        std::lock_guard<std::mutex> lock(mu_);
        ++error_count_;
        ++fps_input_errors_;
        ++breakdown_[transport_index(transport)].error_count;
    }

    void record_runtime_failure(TransportKind transport) {
        std::lock_guard<std::mutex> lock(mu_);
        ++error_count_;
        ++breakdown_[transport_index(transport)].error_count;
    }

    RunSummary finalize(const ScenarioConfig& scenario,
                        const CliOptions& cli,
                        const std::string& resolved_room,
                        std::uint64_t elapsed_ms,
                        const std::vector<std::unique_ptr<SessionClient>>& clients) const {
        RunSummary summary;
        summary.scenario = scenario.name;
        summary.host = cli.host;
        summary.room = resolved_room;
        summary.port = cli.port;
        summary.udp_port = cli.udp_port == 0 ? cli.port : cli.udp_port;
        summary.seed = cli.seed;
        summary.sessions = scenario.sessions;
        summary.elapsed_ms = elapsed_ms;

        std::vector<double> latencies;
        {
            std::lock_guard<std::mutex> lock(mu_);
            summary.connected_sessions = connected_sessions_;
            summary.authenticated_sessions = authenticated_sessions_;
            summary.joined_sessions = joined_sessions_;
            summary.success_count = success_count_;
            summary.error_count = error_count_;
            summary.connect_failures = connect_failures_;
            summary.login_failures = login_failures_;
            summary.attach_failures = attach_failures_;
            summary.join_failures = join_failures_;
            summary.chat_success = chat_success_;
            summary.chat_errors = chat_errors_;
            summary.ping_success = ping_success_;
            summary.ping_errors = ping_errors_;
            summary.fps_input_success = fps_input_success_;
            summary.fps_input_errors = fps_input_errors_;
            summary.fps_snapshot_count = fps_snapshot_count_;
            summary.fps_delta_count = fps_delta_count_;
            latencies = latencies_ms_;
            summary.transport_breakdown.assign(breakdown_.begin(), breakdown_.end());
        }

        for (const auto& client : clients) {
            if (client == nullptr) {
                continue;
            }
            const auto stats = client->transport_stats();
            accumulate_transport_stats(summary.transport, stats);
            accumulate_transport_stats(
                summary.transport_breakdown[transport_index(client->transport_kind())].stats,
                stats);
        }

        const auto elapsed_seconds = std::max(0.001, static_cast<double>(elapsed_ms) / 1000.0);
        summary.throughput_rps = static_cast<double>(summary.success_count) / elapsed_seconds;

        if (!latencies.empty()) {
            std::sort(latencies.begin(), latencies.end());
            summary.latency_p50_ms = percentile(latencies, 50.0);
            summary.latency_p95_ms = percentile(latencies, 95.0);
            summary.latency_p99_ms = percentile(latencies, 99.0);
            summary.latency_max_ms = latencies.back();
        }

        std::vector<TransportBreakdown> filtered;
        filtered.reserve(summary.transport_breakdown.size());
        for (const auto& breakdown : summary.transport_breakdown) {
            if (breakdown.sessions == 0) {
                continue;
            }
            filtered.push_back(breakdown);
        }
        summary.transport_breakdown = std::move(filtered);
        return summary;
    }

private:
    static double percentile(const std::vector<double>& sorted_values, double p) {
        if (sorted_values.empty()) {
            return 0.0;
        }
        const auto rank = static_cast<std::size_t>(
            std::llround((p / 100.0) * static_cast<double>(sorted_values.size() - 1)));
        return sorted_values[rank];
    }

    mutable std::mutex mu_;
    std::uint32_t connected_sessions_{0};
    std::uint32_t authenticated_sessions_{0};
    std::uint32_t joined_sessions_{0};
    std::uint64_t success_count_{0};
    std::uint64_t error_count_{0};
    std::uint64_t connect_failures_{0};
    std::uint64_t login_failures_{0};
    std::uint64_t attach_failures_{0};
    std::uint64_t join_failures_{0};
    std::uint64_t chat_success_{0};
    std::uint64_t chat_errors_{0};
    std::uint64_t ping_success_{0};
    std::uint64_t ping_errors_{0};
    std::uint64_t fps_input_success_{0};
    std::uint64_t fps_input_errors_{0};
    std::uint64_t fps_snapshot_count_{0};
    std::uint64_t fps_delta_count_{0};
    std::vector<double> latencies_ms_;
    std::array<TransportBreakdown, kTransportKindCount> breakdown_{};
};

std::string make_chat_message(const ScenarioConfig& scenario,
                              std::uint32_t session_index,
                              std::uint64_t iteration) {
    std::ostringstream stream;
    stream << scenario.name << "|session=" << session_index << "|iteration=" << iteration << "|";
    std::string message = stream.str();
    if (message.size() < scenario.message_bytes) {
        message.append(scenario.message_bytes - message.size(), 'x');
    } else if (message.size() > scenario.message_bytes) {
        message.resize(scenario.message_bytes);
    }
    return message;
}

std::vector<GroupAssignment> expand_assignments(const ScenarioConfig& scenario) {
    std::vector<GroupAssignment> assignments;
    assignments.reserve(scenario.sessions);
    for (const auto& group : scenario.groups) {
        for (std::uint32_t i = 0; i < group.count; ++i) {
            assignments.push_back(GroupAssignment{
                .name = group.name,
                .transport = group.transport,
                .mode = group.mode,
                .rate_per_sec = group.rate_per_sec,
                .join_room = group.join_room,
            });
        }
    }
    return assignments;
}

void maybe_log(bool verbose, const std::string& message) {
    if (verbose) {
        std::cout << message << '\n';
    }
}

void run_session_workload(SessionClient& client,
                          const ScenarioConfig& scenario,
                          const std::string& run_room,
                          const GroupAssignment& assignment,
                          std::uint32_t session_index,
                          std::uint32_t seed,
                          const std::chrono::steady_clock::time_point deadline,
                          MetricsCollector& metrics,
                          bool verbose) {
    if (assignment.mode == SessionMode::kLoginOnly) {
        while (std::chrono::steady_clock::now() < deadline) {
            if (!client.is_connected()) {
                metrics.record_runtime_failure(assignment.transport);
                maybe_log(verbose,
                          "session " + std::to_string(session_index) + " disconnected during login_only");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        return;
    }

    const auto interval = std::chrono::duration<double>(1.0 / assignment.rate_per_sec);
    const auto interval_duration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval);
    std::mt19937 rng(seed ^ (session_index * 2654435761u));
    const auto initial_jitter_ms = std::uniform_int_distribution<int>(
        0,
        std::max(0, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(interval).count())))
                                       (rng);
    auto next_action = std::chrono::steady_clock::now() + std::chrono::milliseconds(initial_jitter_ms);
    std::uint64_t iteration = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        if (next_action > now) {
            std::this_thread::sleep_until(std::min(next_action, deadline));
            continue;
        }

        if (assignment.mode == SessionMode::kChat) {
            const auto message = make_chat_message(scenario, session_index, iteration++);
            const auto started = std::chrono::steady_clock::now();
            if (!client.send_chat_and_wait_echo(run_room, message)) {
                metrics.record_chat_failure(assignment.transport);
                maybe_log(verbose,
                          "session " + std::to_string(session_index) + " chat failed: " + client.last_error());
                return;
            }
            const auto elapsed_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            metrics.record_chat_success(assignment.transport, elapsed_ms);
        } else if (assignment.mode == SessionMode::kPing) {
            const auto started = std::chrono::steady_clock::now();
            if (!client.send_ping_and_wait_pong()) {
                metrics.record_ping_failure(assignment.transport);
                maybe_log(verbose,
                          "session " + std::to_string(session_index) + " ping failed: " + client.last_error());
                return;
            }
            const auto elapsed_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            metrics.record_ping_success(assignment.transport, elapsed_ms);
        } else if (assignment.mode == SessionMode::kFpsInput) {
            FpsUpdateResult update;
            const auto started = std::chrono::steady_clock::now();
            if (!client.send_fps_input_and_wait_state(&update)) {
                metrics.record_fps_input_failure(assignment.transport);
                maybe_log(verbose,
                          "session " + std::to_string(session_index) + " fps_input failed: " + client.last_error());
                return;
            }
            const auto elapsed_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            metrics.record_fps_input_success(assignment.transport, elapsed_ms, update);
        }

        next_action = std::chrono::steady_clock::now() + interval_duration;
    }
}

}  // namespace

RunSummary run_scenario(const ScenarioConfig& scenario, const CliOptions& cli) {
    MetricsCollector metrics;
    auto assignments = expand_assignments(scenario);
    std::vector<std::unique_ptr<SessionClient>> clients;
    clients.reserve(assignments.size());
    std::vector<std::thread> workers;
    workers.reserve(assignments.size());
    const auto resolved_room =
        (!scenario.unique_room_per_run || scenario.room == "lobby")
            ? scenario.room
            : scenario.room + "_" + std::to_string(cli.seed);

    const auto run_started = std::chrono::steady_clock::now();
    const auto run_deadline =
        run_started + std::chrono::milliseconds(scenario.ramp_up_ms + scenario.duration_ms);
    const auto ramp_delay = scenario.sessions > 0
                                ? std::chrono::milliseconds(scenario.ramp_up_ms / scenario.sessions)
                                : std::chrono::milliseconds(0);
    std::vector<std::string> transports;
    transports.reserve(assignments.size());
    for (const auto& assignment : assignments) {
        metrics.record_session_planned(assignment.transport);
        transports.push_back(transport_name(assignment.transport));
    }
    std::sort(transports.begin(), transports.end());
    transports.erase(std::unique(transports.begin(), transports.end()), transports.end());

    for (std::uint32_t session_index = 0; session_index < assignments.size(); ++session_index) {
        const auto& assignment = assignments[session_index];
        auto client = make_session_client(assignment.transport, ClientOptions{
            .connect_timeout_ms = scenario.connect_timeout_ms,
            .read_timeout_ms = scenario.read_timeout_ms,
            .udp_port = cli.udp_port == 0 ? cli.port : cli.udp_port,
            .trace_udp_attach = cli.verbose,
        });

        maybe_log(cli.verbose,
                  "setup session=" + std::to_string(session_index)
                      + " transport=" + transport_name(assignment.transport)
                      + " mode=" + mode_name(assignment.mode));

        if (!client->connect(cli.host, cli.port)) {
            metrics.record_connect_failure(assignment.transport);
            maybe_log(cli.verbose,
                      "connect failed for session " + std::to_string(session_index) + ": " + client->last_error());
            clients.push_back(std::move(client));
            if (ramp_delay.count() > 0) {
                std::this_thread::sleep_for(ramp_delay);
            }
            continue;
        }
        metrics.record_connected(assignment.transport);

        LoginResult login_result;
        // Concurrent runs must not reuse the same login IDs, otherwise duplicate-name
        // rejection masquerades as an overload/login-collapse signal.
        const auto user = scenario.login_prefix + "_" + std::to_string(cli.seed) + "_" + std::to_string(session_index);
        if (!client->login(user, {}, &login_result)) {
            const bool authenticated = client->authentication_completed();
            if (authenticated) {
                metrics.record_authenticated(assignment.transport);
            }
            if (authenticated && assignment.transport != TransportKind::kTcp) {
                metrics.record_attach_failure(assignment.transport);
            } else {
                metrics.record_login_failure(assignment.transport);
            }
            maybe_log(cli.verbose,
                      std::string(authenticated && assignment.transport != TransportKind::kTcp
                                      ? "attach failed for session "
                                      : "login failed for session ")
                          + std::to_string(session_index) + ": " + client->last_error());
            client->close();
            clients.push_back(std::move(client));
            if (ramp_delay.count() > 0) {
                std::this_thread::sleep_for(ramp_delay);
            }
            continue;
        }
        metrics.record_authenticated(assignment.transport);

        if (assignment.join_room) {
            SnapshotResult snapshot;
            if (!client->join(resolved_room, scenario.room_password, &snapshot)) {
                metrics.record_join_failure(assignment.transport);
                maybe_log(cli.verbose,
                          "join failed for session " + std::to_string(session_index) + ": " + client->last_error());
                client->close();
                clients.push_back(std::move(client));
                if (ramp_delay.count() > 0) {
                    std::this_thread::sleep_for(ramp_delay);
                }
                continue;
            }
            metrics.record_joined(assignment.transport);
        }

        if (assignment.mode == SessionMode::kFpsInput) {
            FpsUpdateResult initial_update;
            if (!client->prepare_fps_session(&initial_update)) {
                metrics.record_fps_input_failure(assignment.transport);
                maybe_log(cli.verbose,
                          "fps session prime failed for session " + std::to_string(session_index) + ": "
                              + client->last_error());
                client->close();
                clients.push_back(std::move(client));
                if (ramp_delay.count() > 0) {
                    std::this_thread::sleep_for(ramp_delay);
                }
                continue;
            }
            metrics.record_fps_update(initial_update);
        }

        auto* client_ptr = client.get();
        workers.emplace_back([client_ptr,
                              &scenario,
                              &resolved_room,
                              assignment,
                              session_index,
                              seed = cli.seed,
                              run_deadline,
                              &metrics,
                              verbose = cli.verbose]() {
            run_session_workload(
                *client_ptr, scenario, resolved_room, assignment, session_index, seed, run_deadline, metrics, verbose);
        });
        clients.push_back(std::move(client));

        if (ramp_delay.count() > 0) {
            std::this_thread::sleep_for(ramp_delay);
        }
    }

    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    for (auto& client : clients) {
        if (client != nullptr) {
            client->close();
        }
    }

    const auto elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - run_started)
            .count());
    auto summary = metrics.finalize(scenario, cli, resolved_room, elapsed_ms, clients);
    summary.transports = std::move(transports);
    return summary;
}

}  // namespace loadgen
