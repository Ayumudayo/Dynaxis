#include "admin/inventory_poller.hpp"

#include "admin/bounded_http_probe.hpp"
#include "admin/prometheus_parser.hpp"
#include "admin/resp_probe.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>

namespace dynaxis::admin {
namespace {

std::uint64_t now_unix_ms() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return ms < 0 ? 0ULL : static_cast<std::uint64_t>(ms);
}

bool is_timeout_or_connect_error(const BoundedHttpProbeResult& res) {
    if (res.status_code > 0) {
        return false;
    }
    return !res.error.empty();
}

ServiceObservation probe_http_target(const ServiceTargetConfig& target,
                                     const AdminConfig& config,
                                     std::uint64_t observed_ms) {
    ServiceObservation obs;
    obs.service_id = target.service_id;
    obs.display_name = target.display_name;
    obs.kind = target.kind;
    obs.layer = target.layer;
    obs.desired_count = target.desired_count;
    obs.observed_at_unix_ms = observed_ms;
    obs.grafana_url = target.grafana_url;
    obs.external_url = target.external_url;

    const auto health_res = bounded_http_get(
        target.host,
        target.port,
        target.health_path.empty() ? std::string("/healthz") : target.health_path,
        config.probe_timeout_ms);
    obs.probe_latency_ms = health_res.latency_ms;

    if (!health_res.ok) {
        obs.health_ok = false;
        obs.ready_ok = false;
        obs.ready_count = 0;
        if (is_timeout_or_connect_error(health_res)) {
            obs.status = ServiceStatus::UNKNOWN;
            obs.detail = health_res.error.empty() ? "probe unreachable" : health_res.error;
        } else {
            obs.status = ServiceStatus::NOT_READY;
            obs.detail = "HTTP " + std::to_string(health_res.status_code) + " on health check";
        }
        return obs;
    }

    obs.health_ok = true;
    if (target.probe_type == ProbeType::HTTP_OK) {
        obs.ready_ok = true;
        obs.ready_count = target.desired_count;
        obs.status = ServiceStatus::READY;
        obs.detail = "HTTP " + std::to_string(health_res.status_code);
        if (target.layer == TopologyLayer::EDGE) {
            if (const auto sess = extract_prometheus_int64(health_res.body, "haproxy_frontend_current_sessions")) {
                obs.active_sessions = *sess;
            }
        }
        return obs;
    }

    const auto ready_res = bounded_http_get(
        target.host,
        target.port,
        target.ready_path.empty() ? std::string("/readyz") : target.ready_path,
        config.probe_timeout_ms);
    obs.probe_latency_ms = std::max(obs.probe_latency_ms, ready_res.latency_ms);

    if (!ready_res.ok) {
        obs.ready_ok = false;
        obs.ready_count = 0;
        if (is_timeout_or_connect_error(ready_res)) {
            obs.status = ServiceStatus::UNKNOWN;
            obs.detail = ready_res.error.empty() ? "ready probe unreachable" : ready_res.error;
        } else {
            obs.status = ServiceStatus::NOT_READY;
            obs.detail = "HTTP " + std::to_string(ready_res.status_code) + " on /readyz";
        }
        return obs;
    }

    obs.ready_ok = true;
    obs.ready_count = target.desired_count;
    obs.status = ServiceStatus::READY;
    obs.detail = "healthy & ready";

    if (!target.metrics_path.empty()) {
        const auto metrics_res = bounded_http_get(
            target.host,
            target.port,
            target.metrics_path,
            config.probe_timeout_ms);
        if (metrics_res.ok) {
            if (target.layer == TopologyLayer::GATEWAY) {
                if (const auto sess = extract_prometheus_int64(metrics_res.body, "gateway_sessions_active")) {
                    obs.active_sessions = *sess;
                }
            } else if (target.layer == TopologyLayer::SERVER) {
                if (const auto sess = extract_prometheus_int64(metrics_res.body, "chat_session_active")) {
                    obs.active_sessions = *sess;
                }
            } else if (target.layer == TopologyLayer::WORKER) {
                if (const auto pending = extract_prometheus_int64(metrics_res.body, "wb_pending")) {
                    obs.wb_pending = *pending;
                    if (config.wb_pending_warn > 0 && *pending >= config.wb_pending_warn) {
                        obs.status = ServiceStatus::DEGRADED;
                        obs.detail = "wb_pending above threshold (" + std::to_string(*pending) + ")";
                    }
                }
                if (const auto dlq_total = extract_prometheus_int64(metrics_res.body, "wb_flush_dlq_total")) {
                    if (*dlq_total >= 0) {
                        obs.wb_dlq_total = static_cast<std::uint64_t>(*dlq_total);
                    }
                }
            }
        }
    }

    return obs;
}

ServiceObservation build_pg_observation(const ServiceTargetConfig& target,
                                        const PostgresProbeResult& pg,
                                        std::uint64_t observed_ms) {
    ServiceObservation obs;
    obs.service_id = target.service_id;
    obs.display_name = target.display_name;
    obs.kind = target.kind;
    obs.layer = target.layer;
    obs.desired_count = target.desired_count;
    obs.observed_at_unix_ms = observed_ms;
    obs.probe_latency_ms = pg.latency_ms;
    obs.grafana_url = target.grafana_url;
    obs.external_url = target.external_url;

    if (target.probe_type == ProbeType::POSTGRES_MIGRATOR) {
        obs.expected_migrations = pg.expected_migrations;
        obs.applied_migrations = pg.applied_migrations;
        if (!pg.healthy) {
            obs.status = ServiceStatus::UNKNOWN;
            obs.detail = pg.error.empty() ? "database unavailable for migration check" : pg.error;
            return obs;
        }
        obs.health_ok = true;
        obs.ready_ok = pg.all_migrations_applied;
        if (pg.all_migrations_applied) {
            obs.status = ServiceStatus::COMPLETED;
            obs.detail = "migrations applied (" + std::to_string(pg.applied_migrations) + "/" +
                         std::to_string(pg.expected_migrations) + ")";
        } else {
            obs.status = ServiceStatus::NOT_READY;
            obs.detail = "pending migrations (" + std::to_string(std::max<std::int64_t>(0, pg.applied_migrations)) +
                         "/" + std::to_string(pg.expected_migrations) + ")";
        }
        return obs;
    }

    obs.active_rooms = pg.active_rooms;
    obs.applied_migrations = pg.applied_migrations;
    obs.expected_migrations = pg.expected_migrations;
    if (!pg.healthy) {
        obs.health_ok = false;
        obs.ready_ok = false;
        obs.ready_count = 0;
        obs.status = (pg.error == "DB_URI is not configured") ? ServiceStatus::UNKNOWN : ServiceStatus::NOT_READY;
        obs.detail = pg.error.empty() ? "postgres probe failed" : pg.error;
        return obs;
    }

    obs.health_ok = true;
    obs.ready_ok = true;
    obs.ready_count = target.desired_count;
    obs.status = ServiceStatus::READY;
    obs.detail = "connected (active_rooms=" + std::to_string(std::max<std::int64_t>(0, pg.active_rooms)) + ")";
    return obs;
}

} // namespace

InventoryPoller::InventoryPoller(AdminConfig config,
                                 std::vector<ServiceTargetConfig> catalog,
                                 std::shared_ptr<IPostgresProbe> pg_probe)
    : config_(std::move(config)),
      catalog_(std::move(catalog)),
      pg_probe_(std::move(pg_probe)),
      snapshot_(std::make_shared<InventorySnapshot>()),
      poll_interval_ms_(std::clamp<std::uint32_t>(config_.poll_interval_ms, 500u, 60'000u)),
      stale_ttl_ms_(static_cast<std::uint32_t>(std::clamp<std::uint64_t>(config_.stale_ttl_ms, 1000ULL, 120'000ULL))) {
    if (!pg_probe_) {
        pg_probe_ = std::make_shared<LibpqxxPostgresProbe>(config_.db_uri, config_.migrations_dir);
    }
}

InventoryPoller::~InventoryPoller() {
    stop();
}

void InventoryPoller::start() {
    if (worker_thread_.joinable()) {
        return;
    }
    stop_requested_.store(false);
    worker_thread_ = std::thread([this]() {
        while (!stop_requested_.load()) {
            poll_once();
            const std::uint32_t sleep_ms = poll_interval_ms();
            std::uint32_t waited = 0;
            while (!stop_requested_.load() && waited < sleep_ms) {
                const std::uint32_t chunk = std::min<std::uint32_t>(100u, sleep_ms - waited);
                std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
                waited += chunk;
            }
        }
    });
}

void InventoryPoller::stop() {
    stop_requested_.store(true);
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

std::shared_ptr<const InventorySnapshot> InventoryPoller::poll_once(std::uint64_t now_unix_ms_override) {
    const std::uint64_t captured_ms = (now_unix_ms_override > 0) ? now_unix_ms_override : now_unix_ms();

    std::vector<ServiceObservation> results(catalog_.size());

    const bool needs_pg = std::any_of(catalog_.begin(), catalog_.end(), [](const auto& t) {
        return t.probe_type == ProbeType::POSTGRES_SQL || t.probe_type == ProbeType::POSTGRES_MIGRATOR;
    });

    PostgresProbeResult shared_pg;
    std::vector<std::size_t> network_indices;
    network_indices.reserve(catalog_.size());
    for (std::size_t i = 0; i < catalog_.size(); ++i) {
        if (catalog_[i].probe_type != ProbeType::POSTGRES_SQL &&
            catalog_[i].probe_type != ProbeType::POSTGRES_MIGRATOR) {
            network_indices.push_back(i);
        }
    }

    std::thread pg_thread;
    if (needs_pg && pg_probe_) {
        pg_thread = std::thread([&]() {
            shared_pg = pg_probe_->probe(config_.probe_timeout_ms);
        });
    }

    const std::size_t worker_count = std::min<std::size_t>(6, network_indices.size());
    std::atomic<std::size_t> cursor{0};
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    for (std::size_t w = 0; w < worker_count; ++w) {
        workers.emplace_back([&]() {
            while (true) {
                const std::size_t slot = cursor.fetch_add(1, std::memory_order_relaxed);
                if (slot >= network_indices.size()) {
                    break;
                }
                const std::size_t idx = network_indices[slot];
                const auto& target = catalog_[idx];
                const std::uint64_t obs_ms = (now_unix_ms_override > 0) ? now_unix_ms_override : now_unix_ms();

                if (target.probe_type == ProbeType::REDIS_RESP) {
                    const auto redis_res = probe_redis_resp(
                        config_.redis_uri,
                        target.host,
                        target.port,
                        config_.dlq_stream_key,
                        config_.probe_timeout_ms);
                    ServiceObservation obs;
                    obs.service_id = target.service_id;
                    obs.display_name = target.display_name;
                    obs.kind = target.kind;
                    obs.layer = target.layer;
                    obs.desired_count = target.desired_count;
                    obs.observed_at_unix_ms = obs_ms;
                    obs.probe_latency_ms = redis_res.latency_ms;
                    obs.wb_dlq_count = redis_res.dlq_length;
                    obs.grafana_url = target.grafana_url;
                    obs.external_url = target.external_url;
                    if (redis_res.healthy) {
                        obs.health_ok = true;
                        obs.ready_ok = true;
                        obs.ready_count = target.desired_count;
                        obs.status = ServiceStatus::READY;
                        obs.detail = "PONG (dlq=" + std::to_string(std::max<long long>(0, redis_res.dlq_length)) + ")";
                    } else {
                        obs.status = ServiceStatus::UNKNOWN;
                        obs.detail = redis_res.error.empty() ? "redis probe unreachable" : redis_res.error;
                    }
                    results[idx] = std::move(obs);
                } else {
                    results[idx] = probe_http_target(target, config_, obs_ms);
                }
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }
    if (pg_thread.joinable()) {
        pg_thread.join();
    }

    for (std::size_t i = 0; i < catalog_.size(); ++i) {
        if (catalog_[i].probe_type == ProbeType::POSTGRES_SQL ||
            catalog_[i].probe_type == ProbeType::POSTGRES_MIGRATOR) {
            results[i] = build_pg_observation(catalog_[i], shared_pg, captured_ms);
        }
    }

    auto next = std::make_shared<InventorySnapshot>();
    next->captured_at_unix_ms = captured_ms;

    std::int64_t redis_dlq_count = -1;
    for (const auto& obs : results) {
        if (obs.service_id == "redis" && obs.wb_dlq_count >= 0) {
            redis_dlq_count = obs.wb_dlq_count;
            break;
        }
    }

    for (auto& obs : results) {
        if (obs.layer == TopologyLayer::WORKER && redis_dlq_count > 0 && obs.status == ServiceStatus::READY) {
            obs.status = ServiceStatus::DEGRADED;
            obs.wb_dlq_count = redis_dlq_count;
            obs.detail = "DLQ items present (" + std::to_string(redis_dlq_count) + ")";
        }

        if (obs.kind != ServiceKind::ONE_SHOT) {
            next->desired_long_running += obs.desired_count;
            if (obs.status == ServiceStatus::READY || obs.status == ServiceStatus::DEGRADED) {
                next->ready_long_running += obs.ready_count;
            }
        }
        if (obs.layer == TopologyLayer::GATEWAY && obs.active_sessions > 0) {
            next->gateway_sessions += static_cast<std::uint64_t>(obs.active_sessions);
        } else if (obs.layer == TopologyLayer::SERVER && obs.active_sessions > 0) {
            next->server_sessions += static_cast<std::uint64_t>(obs.active_sessions);
        } else if (obs.layer == TopologyLayer::WORKER) {
            if (obs.wb_pending >= 0) {
                next->wb_pending = obs.wb_pending;
            }
            next->wb_dlq_total += obs.wb_dlq_total;
        } else if (obs.service_id == "postgres" && obs.active_rooms >= 0) {
            next->active_rooms = obs.active_rooms;
        }
    }
    next->wb_dlq_count = redis_dlq_count;
    next->services = std::move(results);

    {
        std::lock_guard<std::mutex> lock(snapshot_mu_);
        snapshot_ = next;
    }
    initial_poll_done_.store(true);
    return next;
}

std::shared_ptr<const InventorySnapshot> InventoryPoller::snapshot() const {
    std::lock_guard<std::mutex> lock(snapshot_mu_);
    return snapshot_;
}

bool InventoryPoller::has_completed_initial_poll() const noexcept {
    return initial_poll_done_.load();
}

void InventoryPoller::update_runtime_tuning(std::uint32_t poll_interval_ms, std::uint32_t stale_ttl_ms) noexcept {
    poll_interval_ms_.store(std::clamp<std::uint32_t>(poll_interval_ms, 500u, 60'000u));
    stale_ttl_ms_.store(std::clamp<std::uint32_t>(stale_ttl_ms, 1000u, 120'000u));
}

std::uint32_t InventoryPoller::poll_interval_ms() const noexcept {
    return poll_interval_ms_.load();
}

std::uint32_t InventoryPoller::stale_ttl_ms() const noexcept {
    return stale_ttl_ms_.load();
}

const std::vector<ServiceTargetConfig>& InventoryPoller::catalog() const noexcept {
    return catalog_;
}

} // namespace dynaxis::admin
