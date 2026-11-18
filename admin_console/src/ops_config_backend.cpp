#include "admin/ops_config_backend.hpp"

#include "admin/postgres_probe.hpp"
#include "ops_pg_helpers.hpp"

#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace dynaxis::admin {
namespace {

using json = nlohmann::json;

struct ConfigFieldMeta {
    std::string_view key;
    std::string_view label;
    std::string_view description;
    std::string_view value_type;
    std::string_view apply_mode;
    std::string_view scope;
    std::int64_t min_val{0};
    std::int64_t max_val{0};
};

constexpr std::array<ConfigFieldMeta, 7> kConfigSchema{{
    {"admin.poll_interval_ms", "Inventory Poll Interval (ms)", "Admin background poller interval.", "integer", "runtime", "admin", 1000, 60000},
    {"admin.stale_ttl_ms", "Stale Observation TTL (ms)", "Threshold before unrefreshed probe degrades to unknown.", "integer", "runtime", "admin", 3000, 120000},
    {"SESSION_CONTINUITY_ENABLED", "Session Continuity Lease", "Enable DB lease issuance and resume token continuity on chat servers.", "boolean", "restart_required", "servers", 0, 1},
    {"SESSION_CONTINUITY_LEASE_TTL_SEC", "Lease TTL (seconds)", "PG continuity lease expiration window across gateways and servers.", "integer", "restart_required", "gateway_servers", 60, 86400},
    {"SERVER_DRAIN_TIMEOUT_MS", "Graceful Drain Timeout (ms)", "Shutdown drain grace period per server instance.", "integer", "restart_required", "servers", 1000, 60000},
    {"RUNTIME_TRACING_ENABLED", "Runtime Correlation Tracing", "Emit runtime trace flags across servers and wb_worker.", "boolean", "restart_required", "servers_worker", 0, 1},
    {"RUNTIME_TRACING_SAMPLE_PERCENT", "Tracing Sample Rate (%)", "Percentage of sessions/events sampled when tracing is enabled.", "integer", "restart_required", "servers_worker", 1, 100},
}};

const ConfigFieldMeta* find_schema_field(std::string_view key) {
    for (const auto& f : kConfigSchema) {
        if (f.key == key) {
            return &f;
        }
    }
    return nullptr;
}

bool is_forbidden_secret_key(std::string_view key) {
    return key == "CHAT_AUTH_SIGNING_SECRET" || key == "DB_URI" || key == "REDIS_URI" ||
           key == "ADMIN_TOKEN" || key == "ADMIN_VIEWER_TOKEN" || key == "ADMIN_OPERATOR_TOKEN";
}

bool is_restart_allowlisted_target(const ServiceTargetConfig& target) noexcept {
    if (target.kind != ServiceKind::LONG_RUNNING) {
        return false;
    }
    return target.layer == TopologyLayer::GATEWAY ||
           target.layer == TopologyLayer::SERVER ||
           target.layer == TopologyLayer::WORKER;
}

std::vector<std::string> collect_scope_services(const InventoryPoller& poller, std::string_view scope) {
    std::vector<std::string> out;
    if (scope == "admin") {
        out.push_back("admin");
        return out;
    }
    for (const auto& t : poller.catalog()) {
        if (t.kind != ServiceKind::LONG_RUNNING) {
            continue;
        }
        if (scope == "servers" && t.layer == TopologyLayer::SERVER) {
            out.push_back(t.service_id);
        } else if (scope == "gateway_servers" &&
                   (t.layer == TopologyLayer::GATEWAY || t.layer == TopologyLayer::SERVER)) {
            out.push_back(t.service_id);
        } else if (scope == "servers_worker" &&
                   (t.layer == TopologyLayer::SERVER || t.layer == TopologyLayer::WORKER)) {
            out.push_back(t.service_id);
        }
    }
    return out;
}

json build_managed_env_json(const json& values) {
    auto bool_str = [&](const char* k) -> std::string {
        return values.value(k, false) ? "1" : "0";
    };
    return json{
        {"SESSION_CONTINUITY_ENABLED", bool_str("SESSION_CONTINUITY_ENABLED")},
        {"SESSION_CONTINUITY_LEASE_TTL_SEC", std::to_string(values.value("SESSION_CONTINUITY_LEASE_TTL_SEC", 900LL))},
        {"SERVER_DRAIN_TIMEOUT_MS", std::to_string(values.value("SERVER_DRAIN_TIMEOUT_MS", 15000LL))},
        {"RUNTIME_TRACING_ENABLED", bool_str("RUNTIME_TRACING_ENABLED")},
        {"RUNTIME_TRACING_SAMPLE_PERCENT", std::to_string(values.value("RUNTIME_TRACING_SAMPLE_PERCENT", 100LL))},
    };
}

void lock_operations_ledger(pqxx::work& tx) {
    tx.exec_params("select pg_advisory_xact_lock(hashtextextended($1, 0))", "admin:v3:operations");
}

json step_log_entry(std::string_view stage, std::string_view detail_msg, std::uint64_t ts_ms) {
    return json{
        {"stage", std::string(stage)},
        {"detail", std::string(detail_msg)},
        {"at_unix_ms", ts_ms},
    };
}

json operation_row_to_json(const pqxx::row& r) {
    json params = json::object();
    json blast = json::object();
    json steps = json::array();
    try {
        params = json::parse(r[7].c_str());
    } catch (...) {
    }
    try {
        blast = json::parse(r[8].c_str());
    } catch (...) {
    }
    try {
        steps = json::parse(r[9].c_str());
    } catch (...) {
    }

    return json{
        {"operation_id", r[0].c_str()},
        {"operation_type", r[1].c_str()},
        {"target_service", r[2].c_str()},
        {"status", r[3].c_str()},
        {"actor_role", r[4].c_str()},
        {"reason", r[5].c_str()},
        {"idempotency_key", r[6].c_str()},
        {"params", std::move(params)},
        {"blast_radius", std::move(blast)},
        {"step_log", std::move(steps)},
        {"error_message", r[10].c_str()},
        {"verify_deadline_unix_ms", r[11].as<std::int64_t>()},
        {"created_at_unix_ms", r[12].as<std::int64_t>()},
        {"started_at_unix_ms", r[13].as<std::int64_t>()},
        {"verifying_at_unix_ms", r[14].as<std::int64_t>()},
        {"finished_at_unix_ms", r[15].as<std::int64_t>()},
        {"updated_at_unix_ms", r[16].as<std::int64_t>()},
    };
}

constexpr const char* kSelectOperationSql =
    "select id::text, operation_type, target_service, status, actor_role, reason, idempotency_key, "
    "params::text, blast_radius::text, step_log::text, error_message, "
    "coalesce((extract(epoch from verify_deadline_at) * 1000)::bigint, 0), "
    "coalesce((extract(epoch from created_at) * 1000)::bigint, 0), "
    "coalesce((extract(epoch from started_at) * 1000)::bigint, 0), "
    "coalesce((extract(epoch from verifying_at) * 1000)::bigint, 0), "
    "coalesce((extract(epoch from finished_at) * 1000)::bigint, 0), "
    "coalesce((extract(epoch from updated_at) * 1000)::bigint, 0) "
    "from admin_operations ";

std::string ensure_bootstrap_revision(pqxx::work& tx,
                                      const AdminConfig& cfg,
                                      std::int64_t& out_revision) {
    const auto existing = tx.exec(
        "select revision, values::text from admin_config_revisions order by revision desc limit 1");
    if (!existing.empty()) {
        out_revision = existing[0][0].as<std::int64_t>();
        return existing[0][1].c_str();
    }

    const std::string initial_values = build_default_config_values_json(cfg);
    const auto inserted = tx.exec_params(
        "insert into admin_config_revisions(values, diff_summary, apply_mode, actor_role, reason) "
        "values ($1::jsonb, '[]'::jsonb, 'bootstrap', 'system', 'Initial stack schema defaults') "
        "returning revision",
        initial_values);
    out_revision = inserted[0][0].as<std::int64_t>();
    return initial_values;
}

void sync_poller_runtime_from_values(InventoryPoller& poller, const std::string& values_json) {
    try {
        const json v = json::parse(values_json);
        const std::uint32_t poll_ms = static_cast<std::uint32_t>(
            std::clamp<std::int64_t>(v.value("admin.poll_interval_ms", 5000LL), 1000, 60000));
        const std::uint32_t stale_ms = static_cast<std::uint32_t>(
            std::clamp<std::int64_t>(v.value("admin.stale_ttl_ms", 15000LL), 3000, 120000));
        poller.update_runtime_tuning(poll_ms, stale_ms);
    } catch (...) {
    }
}

void reconcile_operations_tx(pqxx::work& tx, const InventoryPoller& poller) {
    const std::uint64_t now_ms = detail::current_unix_ms();

    // 1. Timeout unclaimed queued ops (> 60s)
    const auto queued_expired = tx.exec(
        "select id::text, target_service, actor_role, reason, idempotency_key "
        "from admin_operations where status = 'queued' and created_at < now() - interval '60 seconds'");
    for (const auto& row : queued_expired) {
        const std::string op_id = row[0].c_str();
        const json step = json::array({step_log_entry("timed_out", "Queued operation timed out awaiting host adapter.", now_ms)});
        tx.exec_params(
            "update admin_operations set status = 'timed_out', "
            "error_message = 'Host execution adapter did not claim operation within 60s.', "
            "finished_at = now(), updated_at = now(), step_log = step_log || $2::jsonb "
            "where id = $1::uuid",
            op_id,
            step.dump());
        detail::insert_audit_log(
            tx, AdminRole::ADMIN, "ops.lifecycle", "operation_timed_out", row[1].c_str(), row[3].c_str(), row[4].c_str(),
            "timed_out", json{{"operation_id", op_id}, {"stage", "queued"}});
    }

    // 2. Timeout stuck running ops (> 120s)
    const auto running_expired = tx.exec(
        "select id::text, target_service, actor_role, reason, idempotency_key "
        "from admin_operations where status = 'running' and coalesce(started_at, created_at) < now() - interval '120 seconds'");
    for (const auto& row : running_expired) {
        const std::string op_id = row[0].c_str();
        const json step = json::array({step_log_entry("timed_out", "Host execution adapter command timed out.", now_ms)});
        tx.exec_params(
            "update admin_operations set status = 'timed_out', "
            "error_message = 'Host execution adapter command timed out after 120s.', "
            "finished_at = now(), updated_at = now(), step_log = step_log || $2::jsonb "
            "where id = $1::uuid",
            op_id,
            step.dump());
        detail::insert_audit_log(
            tx, AdminRole::ADMIN, "ops.lifecycle", "operation_timed_out", row[1].c_str(), row[3].c_str(), row[4].c_str(),
            "timed_out", json{{"operation_id", op_id}, {"stage", "running"}});
    }

    // 3. Reconcile active verifying operations against InventoryPoller /readyz snapshot
    const auto verifying_rows = tx.exec(
        "select id::text, target_service, params::text, reason, idempotency_key, "
        "(verify_deadline_at is not null and verify_deadline_at < now()) as pg_expired "
        "from admin_operations where status = 'verifying'");
    for (const auto& row : verifying_rows) {
        const std::string op_id = row[0].c_str();
        json params = json::object();
        try {
            params = json::parse(row[2].c_str());
        } catch (...) {
        }

        std::vector<std::string> affected;
        if (params.contains("affected_services") && params["affected_services"].is_array()) {
            for (const auto& item : params["affected_services"]) {
                if (item.is_string()) {
                    affected.push_back(item.get<std::string>());
                }
            }
        }
        if (affected.empty()) {
            affected.push_back(row[1].c_str());
        }

        const std::uint64_t since_ms = params.value("verifying_since_unix_ms", 0ULL);
        const std::uint64_t deadline_ms = params.value("verify_deadline_unix_ms", 0ULL);
        const bool pg_expired = row[5].as<bool>();

        VerifyProbeState probe_state = evaluate_verifying_services(poller, affected, since_ms, deadline_ms, now_ms);
        if (probe_state == VerifyProbeState::PENDING && pg_expired) {
            probe_state = VerifyProbeState::TIMED_OUT;
        }

        if (probe_state == VerifyProbeState::SUCCEEDED) {
            const json step = json::array({step_log_entry("succeeded", "All target services passed fresh /readyz verification.", now_ms)});
            tx.exec_params(
                "update admin_operations set status = 'succeeded', finished_at = now(), updated_at = now(), "
                "step_log = step_log || $2::jsonb where id = $1::uuid",
                op_id,
                step.dump());
            detail::insert_audit_log(
                tx, AdminRole::ADMIN, "ops.lifecycle", "operation_succeeded", row[1].c_str(), row[3].c_str(), row[4].c_str(),
                "succeeded", json{{"operation_id", op_id}, {"affected_services", affected}});
        } else if (probe_state == VerifyProbeState::TIMED_OUT) {
            const json step = json::array({step_log_entry("timed_out", "Target services did not recover /readyz before deadline.", now_ms)});
            tx.exec_params(
                "update admin_operations set status = 'timed_out', "
                "error_message = 'Target service failed /readyz verification before verify deadline.', "
                "finished_at = now(), updated_at = now(), step_log = step_log || $2::jsonb where id = $1::uuid",
                op_id,
                step.dump());
            detail::insert_audit_log(
                tx, AdminRole::ADMIN, "ops.lifecycle", "operation_timed_out", row[1].c_str(), row[3].c_str(), row[4].c_str(),
                "timed_out", json{{"operation_id", op_id}, {"affected_services", affected}});
        }
    }
}

bool has_active_operation_tx(pqxx::work& tx) {
    const auto rows = tx.exec(
        "select count(*) from admin_operations where status in ('queued', 'running', 'verifying')");
    return !rows.empty() && rows[0][0].as<std::int64_t>() > 0;
}

ChatOpHttpResult commit_config_mutation_tx(pqxx::work& tx,
                                           InventoryPoller& poller,
                                           const ConfigValidationResult& check,
                                           std::string_view op_type,
                                           std::optional<std::int64_t> rollback_of_revision,
                                           AdminRole actor_role,
                                           const std::string& trimmed_reason,
                                           const std::string& trimmed_idem,
                                           std::string_view endpoint,
                                           const std::string& req_hash) {
    const std::uint64_t now_ms = detail::current_unix_ms();
    json blast{
        {"affected_services", check.affected_services},
        {"apply_mode", check.apply_mode},
        {"diff_count", json::parse(check.diff_summary_json).size()},
    };
    if (rollback_of_revision.has_value()) {
        blast["rollback_of_revision"] = *rollback_of_revision;
    }

    std::string op_id;
    if (check.touches_restart_required) {
        json params{
            {"managed_env", json::parse(check.managed_env_json)},
            {"affected_services", check.affected_services},
            {"apply_mode", check.apply_mode},
        };
        if (rollback_of_revision.has_value()) {
            params["rollback_of_revision"] = *rollback_of_revision;
        }
        const json step = json::array({step_log_entry("queued", "Config revision created; restart operation queued for host adapter.", now_ms)});
        const auto ins_op = tx.exec_params(
            "insert into admin_operations(operation_type, target_service, status, actor_role, reason, "
            "idempotency_key, params, blast_radius, step_log) "
            "values ($1, 'stack-config', 'queued', $2, $3, $4, $5::jsonb, $6::jsonb, $7::jsonb) "
            "returning id::text",
            std::string(op_type),
            std::string(admin_role_to_string(actor_role)),
            trimmed_reason,
            trimmed_idem,
            params.dump(),
            blast.dump(),
            step.dump());
        op_id = ins_op[0][0].c_str();
    } else {
        json params{{"affected_services", json::array()}, {"apply_mode", "runtime"}};
        if (rollback_of_revision.has_value()) {
            params["rollback_of_revision"] = *rollback_of_revision;
        }
        const json step = json::array({
            step_log_entry("queued", "Runtime configuration validated.", now_ms),
            step_log_entry("running", "Applied runtime parameters in memory.", now_ms),
            step_log_entry("verifying", "Verified immediate runtime tuning reflection.", now_ms),
            step_log_entry("succeeded", "Runtime configuration update succeeded.", now_ms),
        });
        const auto ins_op = tx.exec_params(
            "insert into admin_operations(operation_type, target_service, status, actor_role, reason, "
            "idempotency_key, params, blast_radius, step_log, started_at, verifying_at, finished_at) "
            "values ($1, 'stack-config', 'succeeded', $2, $3, $4, $5::jsonb, $6::jsonb, $7::jsonb, now(), now(), now()) "
            "returning id::text",
            std::string(op_type),
            std::string(admin_role_to_string(actor_role)),
            trimmed_reason,
            trimmed_idem,
            params.dump(),
            blast.dump(),
            step.dump());
        op_id = ins_op[0][0].c_str();
    }

    pqxx::result ins_rev;
    if (rollback_of_revision.has_value()) {
        ins_rev = tx.exec_params(
            "insert into admin_config_revisions(values, diff_summary, apply_mode, actor_role, reason, operation_id, rollback_of_revision) "
            "values ($1::jsonb, $2::jsonb, $3, $4, $5, $6::uuid, $7) returning revision",
            check.next_values_json,
            check.diff_summary_json,
            check.apply_mode,
            std::string(admin_role_to_string(actor_role)),
            trimmed_reason,
            op_id,
            *rollback_of_revision);
    } else {
        ins_rev = tx.exec_params(
            "insert into admin_config_revisions(values, diff_summary, apply_mode, actor_role, reason, operation_id) "
            "values ($1::jsonb, $2::jsonb, $3, $4, $5, $6::uuid) returning revision",
            check.next_values_json,
            check.diff_summary_json,
            check.apply_mode,
            std::string(admin_role_to_string(actor_role)),
            trimmed_reason,
            op_id);
    }
    const std::int64_t next_rev = ins_rev[0][0].as<std::int64_t>();

    const auto op_rows = tx.exec_params(std::string(kSelectOperationSql) + "where id = $1::uuid", op_id);
    json resp_json{
        {"revision", next_rev},
        {"apply_mode", check.apply_mode},
        {"diff_summary", json::parse(check.diff_summary_json)},
        {"operation", operation_row_to_json(op_rows[0])},
        {"idempotency_key", trimmed_idem},
        {"idempotent_replay", false},
    };
    if (rollback_of_revision.has_value()) {
        resp_json["rollback_of_revision"] = *rollback_of_revision;
    }

    json audit_summary{{"revision", next_rev}, {"operation_id", op_id}, {"apply_mode", check.apply_mode}};
    if (rollback_of_revision.has_value()) {
        audit_summary["rollback_of_revision"] = *rollback_of_revision;
    }
    detail::insert_audit_log(
        tx, actor_role, "ops.config", op_type, "stack-config", trimmed_reason, trimmed_idem,
        check.touches_restart_required ? "accepted" : "succeeded",
        audit_summary);
    detail::save_idempotency(tx, trimmed_idem, endpoint, req_hash, 202, resp_json);
    tx.commit();

    sync_poller_runtime_from_values(poller, check.next_values_json);
    return detail::make_json_result(202, resp_json);
}

} // namespace

RestartValidationResult evaluate_restart_request(const InventoryPoller& poller,
                                                 std::string_view target_service,
                                                 std::string_view confirmation_target,
                                                 std::uint64_t now_unix_ms) {
    RestartValidationResult res;
    const std::string trimmed_target = detail::trim_ascii(target_service);
    const std::string trimmed_confirm = detail::trim_ascii(confirmation_target);

    if (!detail::is_valid_token_identifier(trimmed_target, 64)) {
        res.http_status = 400;
        res.error_code = "target_not_allowlisted";
        res.message = "Invalid target service identifier.";
        return res;
    }

    const ServiceTargetConfig* matched = nullptr;
    for (const auto& t : poller.catalog()) {
        if (t.service_id == trimmed_target) {
            matched = &t;
            break;
        }
    }

    if (!matched || !is_restart_allowlisted_target(*matched)) {
        res.http_status = 400;
        res.error_code = "target_not_allowlisted";
        res.message = "Only allowlisted gateway, server, and wb_worker services support console restart.";
        return res;
    }

    if (trimmed_confirm != trimmed_target) {
        res.http_status = 400;
        res.error_code = "confirmation_mismatch";
        res.message = "confirmation_target must match the exact target service identifier.";
        return res;
    }

    const std::uint64_t effective_now = (now_unix_ms > 0) ? now_unix_ms : detail::current_unix_ms();
    const std::uint32_t stale_ttl = poller.stale_ttl_ms();
    const auto snap = poller.snapshot();

    std::size_t total_siblings = 0;
    std::size_t ready_siblings = 0;
    std::int64_t estimated_sessions = 0;

    auto find_obs = [&](std::string_view id) -> const ServiceObservation* {
        if (!snap) {
            return nullptr;
        }
        for (const auto& obs : snap->services) {
            if (obs.service_id == id) {
                return &obs;
            }
        }
        return nullptr;
    };

    if (const auto* target_obs = find_obs(trimmed_target)) {
        estimated_sessions = std::max<std::int64_t>(0, target_obs->active_sessions);
    }

    for (const auto& sib : poller.catalog()) {
        if (sib.kind != ServiceKind::LONG_RUNNING || sib.layer != matched->layer || sib.service_id == trimmed_target) {
            continue;
        }
        ++total_siblings;
        if (const auto* sib_obs = find_obs(sib.service_id)) {
            const ServiceStatus eff = evaluate_effective_status(*sib_obs, effective_now, stale_ttl);
            if (eff == ServiceStatus::READY && sib_obs->ready_ok) {
                ++ready_siblings;
            }
        }
    }

    if (total_siblings > 0 && ready_siblings == 0) {
        res.http_status = 409;
        res.error_code = "peer_not_ready";
        res.message = "At least one ready sibling instance in the same layer is required before restart.";
        return res;
    }

    res.ok = true;
    res.http_status = 202;
    res.affected_services = {trimmed_target};
    res.blast_radius_json = json{
        {"target_service", trimmed_target},
        {"layer", std::string(topology_layer_to_string(matched->layer))},
        {"estimated_sessions", estimated_sessions},
        {"ready_siblings", ready_siblings},
        {"total_siblings", total_siblings},
        {"affected_services", res.affected_services},
    }.dump();
    return res;
}

std::string build_allowlisted_targets_json(const InventoryPoller& poller, bool has_active_operation) {
    const std::uint64_t effective_now = detail::current_unix_ms();
    const std::uint32_t stale_ttl = poller.stale_ttl_ms();
    const auto snap = poller.snapshot();

    auto find_obs = [&](std::string_view id) -> const ServiceObservation* {
        if (!snap) {
            return nullptr;
        }
        for (const auto& obs : snap->services) {
            if (obs.service_id == id) {
                return &obs;
            }
        }
        return nullptr;
    };

    json targets = json::array();
    for (const auto& t : poller.catalog()) {
        if (!is_restart_allowlisted_target(t)) {
            continue;
        }
        std::size_t total_siblings = 0;
        std::size_t ready_siblings = 0;
        for (const auto& sib : poller.catalog()) {
            if (sib.kind != ServiceKind::LONG_RUNNING || sib.layer != t.layer || sib.service_id == t.service_id) {
                continue;
            }
            ++total_siblings;
            if (const auto* sib_obs = find_obs(sib.service_id)) {
                const ServiceStatus eff = evaluate_effective_status(*sib_obs, effective_now, stale_ttl);
                if (eff == ServiceStatus::READY && sib_obs->ready_ok) {
                    ++ready_siblings;
                }
            }
        }

        ServiceStatus self_status = ServiceStatus::UNKNOWN;
        std::int64_t estimated_sessions = 0;
        if (const auto* self_obs = find_obs(t.service_id)) {
            self_status = evaluate_effective_status(*self_obs, effective_now, stale_ttl);
            estimated_sessions = std::max<std::int64_t>(0, self_obs->active_sessions);
        }

        const bool peer_ok = (total_siblings == 0 || ready_siblings > 0);
        const bool restart_allowed = (!has_active_operation && peer_ok);
        std::string block_reason;
        if (has_active_operation) {
            block_reason = "operation_in_progress";
        } else if (!peer_ok) {
            block_reason = "peer_not_ready";
        }

        targets.push_back(json{
            {"service_id", t.service_id},
            {"display_name", t.display_name},
            {"layer", std::string(topology_layer_to_string(t.layer))},
            {"status", std::string(service_status_to_string(self_status))},
            {"active_sessions", estimated_sessions},
            {"ready_siblings", ready_siblings},
            {"total_siblings", total_siblings},
            {"restart_allowed", restart_allowed},
            {"block_reason", block_reason},
        });
    }
    return targets.dump();
}

std::string build_default_config_values_json(const AdminConfig& config) {
    return json{
        {"admin.poll_interval_ms", std::clamp<std::uint32_t>(config.poll_interval_ms, 1000u, 60000u)},
        {"admin.stale_ttl_ms", static_cast<std::int64_t>(std::clamp<std::uint64_t>(config.stale_ttl_ms, 3000ULL, 120000ULL))},
        {"SESSION_CONTINUITY_ENABLED", false},
        {"SESSION_CONTINUITY_LEASE_TTL_SEC", 900},
        {"SERVER_DRAIN_TIMEOUT_MS", 15000},
        {"RUNTIME_TRACING_ENABLED", false},
        {"RUNTIME_TRACING_SAMPLE_PERCENT", 100},
    }.dump();
}

std::string build_config_overview_json(const AdminConfig& config,
                                       std::int64_t current_revision,
                                       const std::string& current_values_json,
                                       const std::string& revisions_json) {
    json values = json::object();
    json revisions = json::array();
    try {
        values = json::parse(current_values_json);
    } catch (...) {
    }
    try {
        revisions = json::parse(revisions_json);
    } catch (...) {
    }

    json schema = json::array();
    for (const auto& f : kConfigSchema) {
        json entry{
            {"key", std::string(f.key)},
            {"label", std::string(f.label)},
            {"description", std::string(f.description)},
            {"type", std::string(f.value_type)},
            {"apply_mode", std::string(f.apply_mode)},
            {"scope", std::string(f.scope)},
        };
        if (f.value_type == "integer") {
            entry["min"] = f.min_val;
            entry["max"] = f.max_val;
        }
        schema.push_back(std::move(entry));
    }

    const char* raw_auth_secret = std::getenv("CHAT_AUTH_SIGNING_SECRET");
    const bool auth_secret_configured = (raw_auth_secret != nullptr && *raw_auth_secret != '\0');
    auto make_secret_ref = [](std::string_view key, std::string_view desc, bool configured) {
        return json{
            {"key", std::string(key)},
            {"description", std::string(desc)},
            {"secret_ref", true},
            {"editable", false},
            {"configured", configured},
            {"masked_value", configured ? "••••••••" : "—"},
        };
    };

    const json secret_refs = json::array({
        make_secret_ref("CHAT_AUTH_SIGNING_SECRET", "Signed chat login HMAC/Ed25519 key (masked).", auth_secret_configured),
        make_secret_ref("DB_URI", "PostgreSQL authority DSN (masked).", !config.db_uri.empty()),
        make_secret_ref("REDIS_URI", "Redis presence & stream DSN (masked).", !config.redis_uri.empty()),
        make_secret_ref("ADMIN_TOKEN", "Admin control-plane bearer token (masked).", !config.admin_token.empty()),
    });

    return json{
        {"current_revision", current_revision},
        {"current_values", std::move(values)},
        {"schema", std::move(schema)},
        {"secret_refs", secret_refs},
        {"revisions", std::move(revisions)},
    }.dump();
}

ConfigValidationResult evaluate_config_patch(const InventoryPoller& poller,
                                             const std::string& current_values_json,
                                             const std::string& patch_values_json,
                                             std::string_view confirmation_target) {
    ConfigValidationResult res;
    json current_vals;
    json patch_vals;
    try {
        current_vals = json::parse(current_values_json);
        patch_vals = json::parse(patch_values_json);
    } catch (...) {
        res.http_status = 400;
        res.error_code = "invalid_json";
        res.message = "Malformed configuration JSON object.";
        return res;
    }

    if (!current_vals.is_object() || !patch_vals.is_object()) {
        res.http_status = 400;
        res.error_code = "invalid_json";
        res.message = "Configuration payload must be a JSON object.";
        return res;
    }

    json next_vals = current_vals;
    for (auto it = patch_vals.begin(); it != patch_vals.end(); ++it) {
        const std::string& key = it.key();
        if (is_forbidden_secret_key(key)) {
            res.http_status = 400;
            res.error_code = "secret_or_unknown_key_forbidden";
            res.message = "Secret reference key cannot be modified through console schema API: " + key;
            return res;
        }
        const ConfigFieldMeta* meta = find_schema_field(key);
        if (!meta) {
            res.http_status = 400;
            res.error_code = "secret_or_unknown_key_forbidden";
            res.message = "Unknown or non-schema configuration key is forbidden: " + key;
            return res;
        }

        const json& val = it.value();
        if (meta->value_type == "boolean") {
            if (!val.is_boolean()) {
                res.http_status = 400;
                res.error_code = "schema_validation_failed";
                res.message = "Key '" + key + "' expects a boolean value.";
                return res;
            }
            next_vals[key] = val.get<bool>();
        } else {
            if (val.is_boolean() || !val.is_number_integer()) {
                res.http_status = 400;
                res.error_code = "schema_validation_failed";
                res.message = "Key '" + key + "' expects an integer value.";
                return res;
            }
            const std::int64_t num = val.get<std::int64_t>();
            if (num < meta->min_val || num > meta->max_val) {
                res.http_status = 400;
                res.error_code = "schema_validation_failed";
                res.message = "Key '" + key + "' value out of range [" + std::to_string(meta->min_val) +
                              ", " + std::to_string(meta->max_val) + "].";
                return res;
            }
            next_vals[key] = num;
        }
    }

    if (detail::trim_ascii(confirmation_target) != "stack-config") {
        res.http_status = 400;
        res.error_code = "confirmation_mismatch";
        res.message = "confirmation_target must equal 'stack-config' to apply or roll back configuration.";
        return res;
    }

    json diff = json::array();
    std::set<std::string> affected_set;
    for (const auto& meta : kConfigSchema) {
        const std::string key(meta.key);
        const json before_val = current_vals.contains(key) ? current_vals[key] : json();
        const json after_val = next_vals.contains(key) ? next_vals[key] : json();
        if (before_val == after_val) {
            continue;
        }

        diff.push_back(json{
            {"key", key},
            {"before", before_val},
            {"after", after_val},
            {"apply_mode", std::string(meta.apply_mode)},
        });
        if (meta.apply_mode == "runtime") {
            res.touches_runtime = true;
        } else {
            res.touches_restart_required = true;
            for (const auto& svc : collect_scope_services(poller, meta.scope)) {
                affected_set.insert(svc);
            }
        }
    }

    if (diff.empty()) {
        res.http_status = 400;
        res.error_code = "empty_diff";
        res.message = "Submitted configuration values match the current active revision.";
        return res;
    }

    if (res.touches_runtime && res.touches_restart_required) {
        res.apply_mode = "mixed";
    } else if (res.touches_restart_required) {
        res.apply_mode = "restart_required";
    } else {
        res.apply_mode = "runtime";
    }

    res.ok = true;
    res.http_status = 202;
    res.next_values_json = next_vals.dump();
    res.diff_summary_json = diff.dump();
    res.managed_env_json = build_managed_env_json(next_vals).dump();
    res.affected_services.assign(affected_set.begin(), affected_set.end());
    return res;
}

VerifyProbeState evaluate_verifying_services(const InventoryPoller& poller,
                                             const std::vector<std::string>& affected_services,
                                             std::uint64_t verifying_since_unix_ms,
                                             std::uint64_t verify_deadline_unix_ms,
                                             std::uint64_t now_unix_ms) {
    const std::uint64_t effective_now = (now_unix_ms > 0) ? now_unix_ms : detail::current_unix_ms();
    const auto snap = poller.snapshot();
    const std::uint32_t stale_ttl = poller.stale_ttl_ms();

    bool all_ready = !affected_services.empty() && static_cast<bool>(snap);
    if (all_ready) {
        for (const auto& svc_id : affected_services) {
            bool matched_ready = false;
            for (const auto& obs : snap->services) {
                if (obs.service_id == svc_id) {
                    const ServiceStatus eff = evaluate_effective_status(obs, effective_now, stale_ttl);
                    if (obs.observed_at_unix_ms >= verifying_since_unix_ms &&
                        obs.ready_ok &&
                        eff == ServiceStatus::READY) {
                        matched_ready = true;
                    }
                    break;
                }
            }
            if (!matched_ready) {
                all_ready = false;
                break;
            }
        }
    }

    if (all_ready) {
        return VerifyProbeState::SUCCEEDED;
    }
    if (verify_deadline_unix_ms > 0 && effective_now > verify_deadline_unix_ms) {
        return VerifyProbeState::TIMED_OUT;
    }
    return VerifyProbeState::PENDING;
}

PostgresOpsConfigBackend::PostgresOpsConfigBackend(AdminConfig config, InventoryPoller& poller)
    : config_(std::move(config)), poller_(poller) {}

ChatOpHttpResult PostgresOpsConfigBackend::list_operations(std::string_view status_filter, std::size_t limit) {
    if (config_.db_uri.empty()) {
        return detail::make_error_result(503, "db_unconfigured", "DB_URI is not configured.");
    }
    const std::string status_trim = detail::to_lower_ascii(detail::trim_ascii(status_filter));
    const std::size_t clamped_limit = std::clamp<std::size_t>(limit, 1, 100);

    try {
        pqxx::connection conn(config_.db_uri);
        pqxx::work tx(conn);
        tx.exec("SET LOCAL statement_timeout = 2500");
        lock_operations_ledger(tx);
        reconcile_operations_tx(tx, poller_);

        const auto active_rows = tx.exec(
            "select id::text from admin_operations where status in ('queued', 'running', 'verifying') "
            "order by created_at asc limit 1");
        const std::string active_op_id = active_rows.empty() ? std::string{} : std::string(active_rows[0][0].c_str());

        const std::string sql =
            std::string(kSelectOperationSql) +
            "where ($1 = '' or status = $1) "
            "order by created_at desc limit $2";
        const auto rows = tx.exec_params(sql, status_trim, static_cast<std::int64_t>(clamped_limit));
        json ops = json::array();
        for (const auto& r : rows) {
            ops.push_back(operation_row_to_json(r));
        }
        tx.commit();
        return detail::make_json_result(
            200,
            json{
                {"operations", std::move(ops)},
                {"active_operation_id", active_op_id},
                {"allowlisted_targets", json::parse(build_allowlisted_targets_json(poller_, !active_op_id.empty()))},
            });
    } catch (const std::exception& ex) {
        return detail::make_error_result(500, "db_query_failed", sanitize_connection_error(ex.what()));
    }
}

ChatOpHttpResult PostgresOpsConfigBackend::get_operation(std::string_view operation_id, bool refresh_probe) {
    if (config_.db_uri.empty()) {
        return detail::make_error_result(503, "db_unconfigured", "DB_URI is not configured.");
    }
    const std::string target_id = detail::trim_ascii(operation_id);
    if (!detail::is_valid_uuid(target_id)) {
        return detail::make_error_result(400, "invalid_operation_id", "Operation ID must be a valid UUID.");
    }

    try {
        bool should_poll = false;
        if (refresh_probe) {
            pqxx::connection conn(config_.db_uri);
            pqxx::read_transaction rtx(conn);
            const auto peek = rtx.exec_params("select status from admin_operations where id = $1::uuid", target_id);
            if (!peek.empty() && std::string_view(peek[0][0].c_str()) == "verifying") {
                should_poll = true;
            }
        }
        if (should_poll) {
            (void)poller_.poll_once();
        }

        pqxx::connection conn(config_.db_uri);
        pqxx::work tx(conn);
        tx.exec("SET LOCAL statement_timeout = 2500");
        lock_operations_ledger(tx);
        reconcile_operations_tx(tx, poller_);

        const auto rows = tx.exec_params(std::string(kSelectOperationSql) + "where id = $1::uuid", target_id);
        tx.commit();
        if (rows.empty()) {
            return detail::make_error_result(404, "operation_not_found", "Operation was not found.");
        }
        return detail::make_json_result(200, json{{"operation", operation_row_to_json(rows[0])}});
    } catch (const std::exception& ex) {
        return detail::make_error_result(500, "db_query_failed", sanitize_connection_error(ex.what()));
    }
}

ChatOpHttpResult PostgresOpsConfigBackend::create_restart_operation(std::string_view target_service,
                                                                    std::string_view confirmation_target,
                                                                    std::string_view reason,
                                                                    std::string_view idempotency_key,
                                                                    AdminRole actor_role) {
    if (config_.db_uri.empty()) {
        return detail::make_error_result(503, "db_unconfigured", "DB_URI is not configured.");
    }
    const std::string target = detail::trim_ascii(target_service);
    const std::string trimmed_reason = detail::trim_ascii(reason);
    const std::string trimmed_idem = detail::trim_ascii(idempotency_key);

    if (trimmed_reason.empty() || trimmed_reason.size() > 500) {
        return detail::make_error_result(400, "reason_required", "A non-empty audit reason (<= 500 chars) is required.");
    }
    if (!detail::is_valid_idempotency_key(trimmed_idem)) {
        return detail::make_error_result(400, "idempotency_key_required", "A valid ASCII idempotency_key is required.");
    }

    const RestartValidationResult check = evaluate_restart_request(poller_, target, confirmation_target);
    if (!check.ok) {
        return detail::make_error_result(check.http_status, check.error_code, check.message);
    }

    const std::string endpoint = "POST /api/v1/operations";
    const std::string req_hash = detail::sha256_hex(
        endpoint + "\nservice_restart\n" + target + "\n" + trimmed_reason);

    try {
        pqxx::connection conn(config_.db_uri);
        pqxx::work tx(conn);
        tx.exec("SET LOCAL statement_timeout = 2500");

        detail::lock_idempotency(tx, trimmed_idem);
        if (auto replay = detail::check_idempotency(
                tx, trimmed_idem, endpoint, req_hash, actor_role, "ops.lifecycle", "service_restart", target, trimmed_reason)) {
            return *replay;
        }

        lock_operations_ledger(tx);
        reconcile_operations_tx(tx, poller_);
        if (has_active_operation_tx(tx)) {
            detail::insert_audit_log(
                tx, actor_role, "ops.lifecycle", "service_restart", target, trimmed_reason, trimmed_idem, "conflict",
                json{{"error", "operation_in_progress"}});
            tx.commit();
            return detail::make_error_result(
                409,
                "operation_in_progress",
                "Another lifecycle or config operation is currently queued, running, or verifying.");
        }

        const std::uint64_t now_ms = detail::current_unix_ms();
        const json params{{"affected_services", check.affected_services}};
        const json step_log = json::array({step_log_entry("queued", "Operation accepted and queued for host adapter.", now_ms)});

        const auto inserted = tx.exec_params(
            "insert into admin_operations(operation_type, target_service, status, actor_role, reason, "
            "idempotency_key, params, blast_radius, step_log) "
            "values ('service_restart', $1, 'queued', $2, $3, $4, $5::jsonb, $6::jsonb, $7::jsonb) "
            "returning id::text",
            target,
            std::string(admin_role_to_string(actor_role)),
            trimmed_reason,
            trimmed_idem,
            params.dump(),
            check.blast_radius_json,
            step_log.dump());
        const std::string op_id = inserted[0][0].c_str();
        const auto rows = tx.exec_params(std::string(kSelectOperationSql) + "where id = $1::uuid", op_id);
        json resp_json{
            {"operation", operation_row_to_json(rows[0])},
            {"idempotency_key", trimmed_idem},
            {"idempotent_replay", false},
        };

        detail::insert_audit_log(
            tx, actor_role, "ops.lifecycle", "service_restart", target, trimmed_reason, trimmed_idem, "accepted",
            json{{"operation_id", op_id}, {"affected_services", check.affected_services}});
        detail::save_idempotency(tx, trimmed_idem, endpoint, req_hash, 202, resp_json);
        tx.commit();
        return detail::make_json_result(202, resp_json);
    } catch (const std::exception& ex) {
        return detail::make_error_result(500, "operation_create_failed", sanitize_connection_error(ex.what()));
    }
}

ChatOpHttpResult PostgresOpsConfigBackend::claim_next_queued_operation(AdminRole actor_role) {
    if (config_.db_uri.empty()) {
        return detail::make_error_result(503, "db_unconfigured", "DB_URI is not configured.");
    }
    try {
        pqxx::connection conn(config_.db_uri);
        pqxx::work tx(conn);
        tx.exec("SET LOCAL statement_timeout = 2500");
        lock_operations_ledger(tx);
        reconcile_operations_tx(tx, poller_);

        const auto running = tx.exec("select count(*) from admin_operations where status in ('running', 'verifying')");
        if (!running.empty() && running[0][0].as<std::int64_t>() > 0) {
            tx.commit();
            return detail::make_json_result(200, json{{"claimed", false}});
        }

        const auto queued = tx.exec(
            "select id::text from admin_operations where status = 'queued' order by created_at asc limit 1");
        if (queued.empty()) {
            tx.commit();
            return detail::make_json_result(200, json{{"claimed", false}});
        }

        const std::string op_id = queued[0][0].c_str();
        const std::uint64_t now_ms = detail::current_unix_ms();
        const json step = json::array({step_log_entry("running", "Claimed by host execution adapter.", now_ms)});
        tx.exec_params(
            "update admin_operations set status = 'running', started_at = now(), updated_at = now(), "
            "step_log = step_log || $2::jsonb where id = $1::uuid",
            op_id,
            step.dump());

        const auto rows = tx.exec_params(std::string(kSelectOperationSql) + "where id = $1::uuid", op_id);
        json op_json = operation_row_to_json(rows[0]);
        detail::insert_audit_log(
            tx, actor_role, "ops.worker", "worker_claim", op_json.value("target_service", ""),
            "Host adapter claimed queued operation", "", "succeeded", json{{"operation_id", op_id}});
        tx.commit();
        return detail::make_json_result(200, json{{"claimed", true}, {"operation", std::move(op_json)}});
    } catch (const std::exception& ex) {
        return detail::make_error_result(500, "worker_claim_failed", sanitize_connection_error(ex.what()));
    }
}

ChatOpHttpResult PostgresOpsConfigBackend::report_worker_result(std::string_view operation_id,
                                                                std::string_view outcome,
                                                                std::string_view summary,
                                                                AdminRole actor_role) {
    if (config_.db_uri.empty()) {
        return detail::make_error_result(503, "db_unconfigured", "DB_URI is not configured.");
    }
    const std::string target_id = detail::trim_ascii(operation_id);
    const std::string trimmed_outcome = detail::to_lower_ascii(detail::trim_ascii(outcome));
    const std::string clean_summary = sanitize_connection_error(detail::trim_ascii(summary));
    if (!detail::is_valid_uuid(target_id)) {
        return detail::make_error_result(400, "invalid_operation_id", "Operation ID must be a valid UUID.");
    }
    if (trimmed_outcome != "command_succeeded" && trimmed_outcome != "command_failed") {
        return detail::make_error_result(400, "invalid_outcome", "outcome must be command_succeeded or command_failed.");
    }

    try {
        pqxx::connection conn(config_.db_uri);
        pqxx::work tx(conn);
        tx.exec("SET LOCAL statement_timeout = 2500");
        lock_operations_ledger(tx);

        const auto found = tx.exec_params(
            "select status, target_service, params::text, reason, idempotency_key "
            "from admin_operations where id = $1::uuid",
            target_id);
        if (found.empty()) {
            tx.commit();
            return detail::make_error_result(404, "operation_not_found", "Operation was not found.");
        }
        if (std::string_view(found[0][0].c_str()) != "running") {
            tx.commit();
            return detail::make_error_result(409, "invalid_operation_state", "Operation is not currently running.");
        }

        const std::string target_svc = found[0][1].c_str();
        const std::uint64_t now_ms = detail::current_unix_ms();
        const std::uint64_t verify_timeout_ms = std::max<std::uint32_t>(config_.operation_verify_timeout_ms, 100u);

        if (trimmed_outcome == "command_succeeded") {
            json params = json::object();
            try {
                params = json::parse(found[0][2].c_str());
            } catch (...) {
            }
            const std::uint64_t deadline_ms = now_ms + verify_timeout_ms;
            params["verifying_since_unix_ms"] = now_ms;
            params["verify_deadline_unix_ms"] = deadline_ms;

            const std::string detail_msg = clean_summary.empty()
                ? "Host adapter command exited 0; verifying /readyz recovery."
                : clean_summary;
            const json step = json::array({step_log_entry("verifying", detail_msg, now_ms)});
            tx.exec_params(
                "update admin_operations set status = 'verifying', verifying_at = now(), updated_at = now(), "
                "verify_deadline_at = now() + (($3)::bigint * interval '1 millisecond'), "
                "params = $2::jsonb, step_log = step_log || $4::jsonb where id = $1::uuid",
                target_id,
                params.dump(),
                static_cast<std::int64_t>(verify_timeout_ms),
                step.dump());
            detail::insert_audit_log(
                tx, actor_role, "ops.worker", "worker_verifying", target_svc, found[0][3].c_str(), found[0][4].c_str(),
                "verifying", json{{"operation_id", target_id}, {"summary", detail_msg}});
        } else {
            const std::string err_msg = clean_summary.empty() ? "Host adapter command failed." : clean_summary;
            const json step = json::array({step_log_entry("failed", err_msg, now_ms)});
            tx.exec_params(
                "update admin_operations set status = 'failed', error_message = $2, finished_at = now(), updated_at = now(), "
                "step_log = step_log || $3::jsonb where id = $1::uuid",
                target_id,
                err_msg,
                step.dump());
            detail::insert_audit_log(
                tx, actor_role, "ops.worker", "worker_failed", target_svc, found[0][3].c_str(), found[0][4].c_str(),
                "failed", json{{"operation_id", target_id}, {"error", err_msg}});
        }

        const auto rows = tx.exec_params(std::string(kSelectOperationSql) + "where id = $1::uuid", target_id);
        json op_json = operation_row_to_json(rows[0]);
        tx.commit();
        return detail::make_json_result(200, json{{"operation", std::move(op_json)}});
    } catch (const std::exception& ex) {
        return detail::make_error_result(500, "worker_result_failed", sanitize_connection_error(ex.what()));
    }
}

ChatOpHttpResult PostgresOpsConfigBackend::get_config() {
    if (config_.db_uri.empty()) {
        return detail::make_error_result(503, "db_unconfigured", "DB_URI is not configured.");
    }
    try {
        pqxx::connection conn(config_.db_uri);
        pqxx::work tx(conn);
        tx.exec("SET LOCAL statement_timeout = 2500");
        lock_operations_ledger(tx);

        std::int64_t current_rev = 0;
        const std::string current_values = ensure_bootstrap_revision(tx, config_, current_rev);
        sync_poller_runtime_from_values(poller_, current_values);

        const auto rev_rows = tx.exec(
            "select revision, apply_mode, actor_role, reason, coalesce(operation_id::text, ''), "
            "coalesce(rollback_of_revision, 0), coalesce((extract(epoch from created_at) * 1000)::bigint, 0), "
            "diff_summary::text "
            "from admin_config_revisions order by revision desc limit 20");
        json rev_arr = json::array();
        for (const auto& r : rev_rows) {
            json diff = json::array();
            try {
                diff = json::parse(r[7].c_str());
            } catch (...) {
            }
            rev_arr.push_back(json{
                {"revision", r[0].as<std::int64_t>()},
                {"apply_mode", r[1].c_str()},
                {"actor_role", r[2].c_str()},
                {"reason", r[3].c_str()},
                {"operation_id", r[4].c_str()},
                {"rollback_of_revision", r[5].as<std::int64_t>()},
                {"created_at_unix_ms", r[6].as<std::int64_t>()},
                {"diff_summary", std::move(diff)},
            });
        }
        tx.commit();
        return ChatOpHttpResult{200, build_config_overview_json(config_, current_rev, current_values, rev_arr.dump())};
    } catch (const std::exception& ex) {
        return detail::make_error_result(500, "db_query_failed", sanitize_connection_error(ex.what()));
    }
}

ChatOpHttpResult PostgresOpsConfigBackend::apply_config(const std::string& raw_body,
                                                        std::string_view reason,
                                                        std::string_view idempotency_key,
                                                        AdminRole actor_role) {
    if (config_.db_uri.empty()) {
        return detail::make_error_result(503, "db_unconfigured", "DB_URI is not configured.");
    }
    const std::string trimmed_reason = detail::trim_ascii(reason);
    const std::string trimmed_idem = detail::trim_ascii(idempotency_key);
    if (trimmed_reason.empty() || trimmed_reason.size() > 500) {
        return detail::make_error_result(400, "reason_required", "A non-empty audit reason (<= 500 chars) is required.");
    }
    if (!detail::is_valid_idempotency_key(trimmed_idem)) {
        return detail::make_error_result(400, "idempotency_key_required", "A valid ASCII idempotency_key is required.");
    }

    json body;
    try {
        body = json::parse(raw_body);
    } catch (...) {
        return detail::make_error_result(400, "invalid_json", "Malformed JSON request body.");
    }
    if (!body.is_object() || !body.contains("values") || !body["values"].is_object()) {
        return detail::make_error_result(400, "invalid_json", "Body must contain a 'values' JSON object.");
    }
    if (!body.contains("expected_revision") || !body["expected_revision"].is_number_integer()) {
        return detail::make_error_result(400, "expected_revision_required", "Integer 'expected_revision' is required.");
    }
    const std::int64_t expected_revision = body["expected_revision"].get<std::int64_t>();
    const std::string confirmation_target = body.value("confirmation_target", "");
    const std::string patch_str = body["values"].dump();

    const std::string endpoint = "POST /api/v1/config/apply";
    const std::string req_hash = detail::sha256_hex(
        endpoint + "\n" + std::to_string(expected_revision) + "\n" + patch_str + "\n" + trimmed_reason);

    try {
        pqxx::connection conn(config_.db_uri);
        pqxx::work tx(conn);
        tx.exec("SET LOCAL statement_timeout = 2500");

        detail::lock_idempotency(tx, trimmed_idem);
        if (auto replay = detail::check_idempotency(
                tx, trimmed_idem, endpoint, req_hash, actor_role, "ops.config", "config_apply", "stack-config", trimmed_reason)) {
            return *replay;
        }

        lock_operations_ledger(tx);
        reconcile_operations_tx(tx, poller_);

        std::int64_t current_rev = 0;
        const std::string current_values = ensure_bootstrap_revision(tx, config_, current_rev);
        if (expected_revision != current_rev) {
            detail::insert_audit_log(
                tx, actor_role, "ops.config", "config_apply", "stack-config", trimmed_reason, trimmed_idem, "conflict",
                json{{"expected_revision", expected_revision}, {"current_revision", current_rev}});
            tx.commit();
            return detail::make_error_result(
                409,
                "revision_conflict",
                "expected_revision does not match current active revision " + std::to_string(current_rev) + ".");
        }

        const ConfigValidationResult check =
            evaluate_config_patch(poller_, current_values, patch_str, confirmation_target);
        if (!check.ok) {
            tx.commit();
            return detail::make_error_result(check.http_status, check.error_code, check.message);
        }

        if (has_active_operation_tx(tx)) {
            tx.commit();
            return detail::make_error_result(
                409,
                "operation_in_progress",
                "Another lifecycle or config operation is currently queued, running, or verifying.");
        }

        return commit_config_mutation_tx(
            tx, poller_, check, "config_apply", std::nullopt, actor_role, trimmed_reason, trimmed_idem, endpoint, req_hash);
    } catch (const std::exception& ex) {
        return detail::make_error_result(500, "config_apply_failed", sanitize_connection_error(ex.what()));
    }
}

ChatOpHttpResult PostgresOpsConfigBackend::rollback_config(std::int64_t target_revision,
                                                           std::string_view confirmation_target,
                                                           std::string_view reason,
                                                           std::string_view idempotency_key,
                                                           AdminRole actor_role) {
    if (config_.db_uri.empty()) {
        return detail::make_error_result(503, "db_unconfigured", "DB_URI is not configured.");
    }
    const std::string trimmed_reason = detail::trim_ascii(reason);
    const std::string trimmed_idem = detail::trim_ascii(idempotency_key);
    if (target_revision <= 0) {
        return detail::make_error_result(400, "invalid_revision", "target_revision must be a positive integer.");
    }
    if (trimmed_reason.empty() || trimmed_reason.size() > 500) {
        return detail::make_error_result(400, "reason_required", "A non-empty audit reason (<= 500 chars) is required.");
    }
    if (!detail::is_valid_idempotency_key(trimmed_idem)) {
        return detail::make_error_result(400, "idempotency_key_required", "A valid ASCII idempotency_key is required.");
    }

    const std::string endpoint = "POST /api/v1/config/rollback";
    const std::string req_hash = detail::sha256_hex(
        endpoint + "\n" + std::to_string(target_revision) + "\n" + trimmed_reason);

    try {
        pqxx::connection conn(config_.db_uri);
        pqxx::work tx(conn);
        tx.exec("SET LOCAL statement_timeout = 2500");

        detail::lock_idempotency(tx, trimmed_idem);
        if (auto replay = detail::check_idempotency(
                tx, trimmed_idem, endpoint, req_hash, actor_role, "ops.config", "config_rollback", "stack-config", trimmed_reason)) {
            return *replay;
        }

        lock_operations_ledger(tx);
        reconcile_operations_tx(tx, poller_);

        std::int64_t current_rev = 0;
        const std::string current_values = ensure_bootstrap_revision(tx, config_, current_rev);
        const auto target_rows = tx.exec_params(
            "select values::text from admin_config_revisions where revision = $1",
            target_revision);
        if (target_rows.empty()) {
            tx.commit();
            return detail::make_error_result(404, "revision_not_found", "Target configuration revision was not found.");
        }

        const ConfigValidationResult check =
            evaluate_config_patch(poller_, current_values, target_rows[0][0].c_str(), confirmation_target);
        if (!check.ok) {
            tx.commit();
            return detail::make_error_result(check.http_status, check.error_code, check.message);
        }

        if (has_active_operation_tx(tx)) {
            tx.commit();
            return detail::make_error_result(
                409,
                "operation_in_progress",
                "Another lifecycle or config operation is currently queued, running, or verifying.");
        }

        return commit_config_mutation_tx(
            tx, poller_, check, "config_rollback", target_revision, actor_role, trimmed_reason, trimmed_idem, endpoint, req_hash);
    } catch (const std::exception& ex) {
        return detail::make_error_result(500, "config_rollback_failed", sanitize_connection_error(ex.what()));
    }
}

void PostgresOpsConfigBackend::record_denied_audit(AdminRole actor_role,
                                                   std::string_view capability,
                                                   std::string_view action,
                                                   std::string_view target,
                                                   std::string_view reason,
                                                   std::string_view idempotency_key,
                                                   std::string_view deny_code) noexcept {
    if (config_.db_uri.empty()) {
        return;
    }
    try {
        pqxx::connection conn(config_.db_uri);
        pqxx::work tx(conn);
        detail::insert_audit_log(
            tx, actor_role, capability, action, target, reason, idempotency_key, "denied",
            json{{"error", std::string(deny_code)}});
        tx.commit();
    } catch (...) {
    }
}

} // namespace dynaxis::admin
