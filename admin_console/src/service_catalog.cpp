#include "admin/service_catalog.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace dynaxis::admin {
namespace {

std::string trim_trailing_slash(std::string url) {
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url;
}

bool read_file_text(const std::filesystem::path& path, std::string& out) {
    std::error_code ec;
    if (path.empty() || !std::filesystem::is_regular_file(path, ec)) {
        return false;
    }
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.is_open()) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

} // namespace

std::vector<ServiceTargetConfig> parse_topology_servers_json(std::string_view json_text,
                                                             std::string_view target_mode,
                                                             std::string_view grafana_base_url) {
    std::vector<ServiceTargetConfig> out;
    if (json_text.empty()) {
        return out;
    }

    try {
        const auto root = nlohmann::json::parse(json_text);
        if (!root.is_object() || !root.contains("servers") || !root["servers"].is_array()) {
            return out;
        }

        const bool use_docker_dns = (target_mode == "docker");
        const std::string grafana = trim_trailing_slash(std::string(grafana_base_url));
        const std::string server_grafana =
            grafana.empty() ? std::string() : (grafana + "/dashboards");

        for (const auto& srv : root["servers"]) {
            if (!srv.is_object() || !srv.contains("instance_id") || !srv["instance_id"].is_string()) {
                continue;
            }
            const std::string id = srv["instance_id"].get<std::string>();
            if (id.empty()) {
                continue;
            }

            unsigned short port = 9090;
            if (!use_docker_dns) {
                if (srv.contains("metrics_host_port") && srv["metrics_host_port"].is_number_integer()) {
                    const int p = srv["metrics_host_port"].get<int>();
                    if (p > 0 && p <= 65535) {
                        port = static_cast<unsigned short>(p);
                    }
                }
            }

            ServiceTargetConfig cfg;
            cfg.service_id = id;
            cfg.display_name = id;
            cfg.kind = ServiceKind::LONG_RUNNING;
            cfg.layer = TopologyLayer::SERVER;
            cfg.probe_type = ProbeType::HTTP_HEALTH_READY;
            cfg.host = use_docker_dns ? id : std::string("127.0.0.1");
            cfg.port = port;
            cfg.health_path = "/healthz";
            cfg.ready_path = "/readyz";
            cfg.metrics_path = "/metrics";
            cfg.desired_count = 1;
            cfg.grafana_url = server_grafana;
            out.push_back(std::move(cfg));
        }
    } catch (...) {
        out.clear();
    }

    return out;
}

std::vector<ServiceTargetConfig> build_service_catalog(const AdminConfig& config,
                                                       std::vector<std::string>* out_warnings) {
    const bool use_docker_dns = (config.target_mode == "docker");
    const std::string grafana = trim_trailing_slash(config.grafana_base_url);
    const std::string prom = trim_trailing_slash(config.prometheus_base_url);
    const std::string haproxy_url = config.haproxy_stats_url;

    auto host_for = [&](const char* docker_name) -> std::string {
        return use_docker_dns ? std::string(docker_name) : std::string("127.0.0.1");
    };

    std::vector<ServiceTargetConfig> catalog;

    // 1. Edge (HAProxy)
    {
        ServiceTargetConfig cfg;
        cfg.service_id = "haproxy";
        cfg.display_name = "HAProxy";
        cfg.kind = ServiceKind::INFRASTRUCTURE;
        cfg.layer = TopologyLayer::EDGE;
        cfg.probe_type = ProbeType::HTTP_OK;
        cfg.host = host_for("haproxy");
        cfg.port = use_docker_dns ? 8404 : config.haproxy_stats_host_port;
        cfg.health_path = "/metrics";
        cfg.ready_path = "/metrics";
        cfg.metrics_path = "/metrics";
        cfg.grafana_url = grafana.empty() ? std::string() : (grafana + "/dashboards");
        cfg.external_url = haproxy_url;
        catalog.push_back(std::move(cfg));
    }

    // 2. Gateways
    for (int idx = 1; idx <= 2; ++idx) {
        ServiceTargetConfig cfg;
        cfg.service_id = "gateway-" + std::to_string(idx);
        cfg.display_name = "Gateway " + std::to_string(idx);
        cfg.kind = ServiceKind::LONG_RUNNING;
        cfg.layer = TopologyLayer::GATEWAY;
        cfg.probe_type = ProbeType::HTTP_HEALTH_READY;
        cfg.host = use_docker_dns ? cfg.service_id : std::string("127.0.0.1");
        cfg.port = use_docker_dns ? 6001
                                  : (idx == 1 ? config.gateway1_metrics_host_port : config.gateway2_metrics_host_port);
        cfg.health_path = "/healthz";
        cfg.ready_path = "/readyz";
        cfg.metrics_path = "/metrics";
        cfg.grafana_url = grafana.empty() ? std::string() : (grafana + "/dashboards");
        catalog.push_back(std::move(cfg));
    }

    // 3. Chat Servers from topology file or fallback server-1/server-2
    std::vector<ServiceTargetConfig> server_targets;
    std::vector<std::filesystem::path> topology_candidates;
    if (!config.topology_path.empty()) {
        topology_candidates.push_back(config.topology_path);
    } else {
        topology_candidates.push_back("docker/stack/topology.active.json");
        topology_candidates.push_back("docker/stack/topologies/default.json");
    }

    std::string topo_json;
    for (const auto& cand : topology_candidates) {
        if (read_file_text(cand, topo_json)) {
            server_targets = parse_topology_servers_json(topo_json, config.target_mode, config.grafana_base_url);
            if (!server_targets.empty()) {
                break;
            }
        }
    }

    if (server_targets.empty()) {
        if (out_warnings) {
            out_warnings->push_back("Topology file unavailable; using default server-1 and server-2 catalog entries.");
        }
        for (int idx = 1; idx <= 2; ++idx) {
            ServiceTargetConfig cfg;
            cfg.service_id = "server-" + std::to_string(idx);
            cfg.display_name = cfg.service_id;
            cfg.kind = ServiceKind::LONG_RUNNING;
            cfg.layer = TopologyLayer::SERVER;
            cfg.probe_type = ProbeType::HTTP_HEALTH_READY;
            cfg.host = use_docker_dns ? cfg.service_id : std::string("127.0.0.1");
            cfg.port = use_docker_dns ? 9090 : static_cast<unsigned short>(39090 + idx);
            cfg.grafana_url = grafana.empty() ? std::string() : (grafana + "/dashboards");
            server_targets.push_back(std::move(cfg));
        }
    }
    catalog.insert(catalog.end(), server_targets.begin(), server_targets.end());

    // 4. Write-Behind Worker
    {
        ServiceTargetConfig cfg;
        cfg.service_id = "wb_worker";
        cfg.display_name = "Write-Behind Worker";
        cfg.kind = ServiceKind::LONG_RUNNING;
        cfg.layer = TopologyLayer::WORKER;
        cfg.probe_type = ProbeType::HTTP_HEALTH_READY;
        cfg.host = host_for("wb_worker");
        cfg.port = use_docker_dns ? 9090 : config.wb_worker_metrics_host_port;
        cfg.grafana_url = grafana.empty() ? std::string() : (grafana + "/dashboards");
        catalog.push_back(std::move(cfg));
    }

    // 5. Data Dependencies & Migrator
    {
        ServiceTargetConfig redis_cfg;
        redis_cfg.service_id = "redis";
        redis_cfg.display_name = "Redis";
        redis_cfg.kind = ServiceKind::INFRASTRUCTURE;
        redis_cfg.layer = TopologyLayer::DATA;
        redis_cfg.probe_type = ProbeType::REDIS_RESP;
        redis_cfg.host = host_for("redis");
        redis_cfg.port = use_docker_dns ? 6379 : config.redis_host_port;
        redis_cfg.grafana_url = grafana.empty() ? std::string() : (grafana + "/dashboards");
        catalog.push_back(std::move(redis_cfg));

        ServiceTargetConfig pg_cfg;
        pg_cfg.service_id = "postgres";
        pg_cfg.display_name = "PostgreSQL";
        pg_cfg.kind = ServiceKind::INFRASTRUCTURE;
        pg_cfg.layer = TopologyLayer::DATA;
        pg_cfg.probe_type = ProbeType::POSTGRES_SQL;
        pg_cfg.host = host_for("postgres");
        pg_cfg.port = use_docker_dns ? 5432 : config.postgres_host_port;
        pg_cfg.grafana_url = grafana.empty() ? std::string() : (grafana + "/dashboards");
        catalog.push_back(std::move(pg_cfg));

        ServiceTargetConfig mig_cfg;
        mig_cfg.service_id = "migrator";
        mig_cfg.display_name = "Schema Migrator";
        mig_cfg.kind = ServiceKind::ONE_SHOT;
        mig_cfg.layer = TopologyLayer::DATA;
        mig_cfg.probe_type = ProbeType::POSTGRES_MIGRATOR;
        mig_cfg.host = host_for("postgres");
        mig_cfg.port = use_docker_dns ? 5432 : config.postgres_host_port;
        mig_cfg.desired_count = 0;
        catalog.push_back(std::move(mig_cfg));
    }

    // 6. Optional Observability Services
    if (config.observability_enabled) {
        ServiceTargetConfig prom_cfg;
        prom_cfg.service_id = "prometheus";
        prom_cfg.display_name = "Prometheus";
        prom_cfg.kind = ServiceKind::INFRASTRUCTURE;
        prom_cfg.layer = TopologyLayer::OBSERVABILITY;
        prom_cfg.probe_type = ProbeType::HTTP_OK;
        prom_cfg.host = host_for("prometheus");
        prom_cfg.port = use_docker_dns ? 9090 : config.prometheus_host_port;
        prom_cfg.health_path = "/-/ready";
        prom_cfg.ready_path = "/-/ready";
        prom_cfg.metrics_path.clear();
        prom_cfg.optional_profile = true;
        prom_cfg.external_url = prom;
        catalog.push_back(std::move(prom_cfg));

        ServiceTargetConfig graf_cfg;
        graf_cfg.service_id = "grafana";
        graf_cfg.display_name = "Grafana";
        graf_cfg.kind = ServiceKind::INFRASTRUCTURE;
        graf_cfg.layer = TopologyLayer::OBSERVABILITY;
        graf_cfg.probe_type = ProbeType::HTTP_OK;
        graf_cfg.host = host_for("grafana");
        graf_cfg.port = use_docker_dns ? 3000 : config.grafana_host_port;
        graf_cfg.health_path = "/api/health";
        graf_cfg.ready_path = "/api/health";
        graf_cfg.metrics_path.clear();
        graf_cfg.optional_profile = true;
        graf_cfg.external_url = grafana;
        catalog.push_back(std::move(graf_cfg));

        ServiceTargetConfig r_exp_cfg;
        r_exp_cfg.service_id = "redis_exporter";
        r_exp_cfg.display_name = "Redis Exporter";
        r_exp_cfg.kind = ServiceKind::INFRASTRUCTURE;
        r_exp_cfg.layer = TopologyLayer::OBSERVABILITY;
        r_exp_cfg.probe_type = ProbeType::HTTP_OK;
        r_exp_cfg.host = host_for("redis_exporter");
        r_exp_cfg.port = use_docker_dns ? 9121 : config.redis_exporter_host_port;
        r_exp_cfg.health_path = "/metrics";
        r_exp_cfg.ready_path = "/metrics";
        r_exp_cfg.metrics_path.clear();
        r_exp_cfg.optional_profile = true;
        catalog.push_back(std::move(r_exp_cfg));

        ServiceTargetConfig pg_exp_cfg;
        pg_exp_cfg.service_id = "postgres_exporter";
        pg_exp_cfg.display_name = "Postgres Exporter";
        pg_exp_cfg.kind = ServiceKind::INFRASTRUCTURE;
        pg_exp_cfg.layer = TopologyLayer::OBSERVABILITY;
        pg_exp_cfg.probe_type = ProbeType::HTTP_OK;
        pg_exp_cfg.host = host_for("postgres_exporter");
        pg_exp_cfg.port = use_docker_dns ? 9187 : config.postgres_exporter_host_port;
        pg_exp_cfg.health_path = "/metrics";
        pg_exp_cfg.ready_path = "/metrics";
        pg_exp_cfg.metrics_path.clear();
        pg_exp_cfg.optional_profile = true;
        catalog.push_back(std::move(pg_exp_cfg));
    }

    return catalog;
}

} // namespace dynaxis::admin
