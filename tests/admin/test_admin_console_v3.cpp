#include "admin/admin_router.hpp"
#include "admin/admin_types.hpp"
#include "admin/inventory_poller.hpp"
#include "admin/ops_config_backend.hpp"
#include "admin/postgres_probe.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using json = nlohmann::json;
using namespace dynaxis::admin;

std::uint64_t now_ms() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

class ScopedHttpMockServer {
public:
    struct HttpReply {
        std::string status{"200 OK"};
        std::string body;
    };
    using Handler = std::function<HttpReply(std::string_view target)>;

    explicit ScopedHttpMockServer(Handler handler)
        : handler_(std::move(handler)),
          acceptor_(ioc_, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)),
          port_(acceptor_.local_endpoint().port()) {
        worker_ = std::thread([this]() { run(); });
    }

    ~ScopedHttpMockServer() {
        stop();
    }

    unsigned short port() const noexcept {
        return port_;
    }

    void stop() {
        if (stopped_.exchange(true)) {
            return;
        }
        boost::system::error_code ec;
        acceptor_.close(ec);
        ioc_.stop();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    void run() {
        while (!stopped_.load()) {
            tcp::socket socket(ioc_);
            boost::system::error_code ec;
            acceptor_.accept(socket, ec);
            if (ec) {
                break;
            }
            try {
                asio::streambuf buf;
                asio::read_until(socket, buf, "\r\n\r\n");
                std::istream req_stream(&buf);
                std::string method;
                std::string target;
                req_stream >> method >> target;

                const HttpReply reply = handler_(target);
                std::ostringstream oss;
                oss << "HTTP/1.1 " << reply.status << "\r\n"
                    << "Content-Type: text/plain; charset=utf-8\r\n"
                    << "Content-Length: " << reply.body.size() << "\r\n"
                    << "Connection: close\r\n\r\n"
                    << reply.body;
                const std::string raw = oss.str();
                asio::write(socket, asio::buffer(raw));
                socket.shutdown(tcp::socket::shutdown_both, ec);
            } catch (...) {
            }
        }
    }

    Handler handler_;
    asio::io_context ioc_;
    tcp::acceptor acceptor_;
    unsigned short port_{0};
    std::atomic<bool> stopped_{false};
    std::thread worker_;
};

class InMemoryOpsConfigBackend final : public IOpsConfigBackend {
public:
    InMemoryOpsConfigBackend(AdminConfig cfg, InventoryPoller& poller)
        : config_(std::move(cfg)), poller_(poller) {
        revisions_.push_back(json{
            {"revision", 1},
            {"values", json::parse(build_default_config_values_json(config_))},
            {"diff_summary", json::array()},
            {"apply_mode", "bootstrap"},
            {"actor_role", "system"},
            {"reason", "Initial stack schema defaults"},
            {"operation_id", ""},
            {"rollback_of_revision", 0},
            {"created_at_unix_ms", now_ms()},
        });
    }

    ChatOpHttpResult list_operations(std::string_view status_filter, std::size_t limit) override {
        std::lock_guard<std::mutex> lock(mu_);
        reconcile_locked();
        json ops = json::array();
        std::string active_id;
        for (auto it = operations_.rbegin(); it != operations_.rend(); ++it) {
            const std::string st = (*it).value("status", "");
            if (active_id.empty() && (st == "queued" || st == "running" || st == "verifying")) {
                active_id = (*it).value("operation_id", "");
            }
            if (!status_filter.empty() && st != status_filter) {
                continue;
            }
            ops.push_back(*it);
            if (ops.size() >= limit) {
                break;
            }
        }
        return {200, json{
                         {"operations", std::move(ops)},
                         {"active_operation_id", active_id},
                         {"allowlisted_targets", json::parse(build_allowlisted_targets_json(poller_, !active_id.empty()))},
                     }.dump()};
    }

    ChatOpHttpResult get_operation(std::string_view operation_id, bool refresh_probe) override {
        bool should_poll = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& op : operations_) {
                if (op.value("operation_id", "") == operation_id && op.value("status", "") == "verifying") {
                    should_poll = true;
                    break;
                }
            }
        }
        if (refresh_probe && should_poll) {
            (void)poller_.poll_once();
        }
        std::lock_guard<std::mutex> lock(mu_);
        reconcile_locked();
        for (const auto& op : operations_) {
            if (op.value("operation_id", "") == operation_id) {
                return {200, json{{"operation", op}}.dump()};
            }
        }
        return err(404, "operation_not_found", "Operation was not found.");
    }

    ChatOpHttpResult create_restart_operation(std::string_view target_service,
                                              std::string_view confirmation_target,
                                              std::string_view reason,
                                              std::string_view idempotency_key,
                                              AdminRole actor_role) override {
        if (reason.empty() || reason.size() > 500) {
            return err(400, "reason_required", "Audit reason is required.");
        }
        if (idempotency_key.empty()) {
            return err(400, "idempotency_key_required", "idempotency_key is required.");
        }

        const auto check = evaluate_restart_request(poller_, target_service, confirmation_target);
        if (!check.ok) {
            return err(check.http_status, check.error_code, check.message);
        }

        std::lock_guard<std::mutex> lock(mu_);
        reconcile_locked();
        const std::string ep = "POST /api/v1/operations";
        const std::string hash = ep + "|" + std::string(target_service) + "|" + std::string(reason);
        if (auto rep = check_idem_locked(idempotency_key, ep, hash)) {
            return *rep;
        }
        if (has_active_locked()) {
            return err(409, "operation_in_progress", "Another operation is already in flight.");
        }

        const std::string op_id = next_uuid();
        const std::uint64_t ts = now_ms();
        json op{
            {"operation_id", op_id},
            {"operation_type", "service_restart"},
            {"target_service", std::string(target_service)},
            {"status", "queued"},
            {"actor_role", std::string(admin_role_to_string(actor_role))},
            {"reason", std::string(reason)},
            {"idempotency_key", std::string(idempotency_key)},
            {"params", {{"affected_services", check.affected_services}}},
            {"blast_radius", json::parse(check.blast_radius_json)},
            {"step_log", json::array({{{"stage", "queued"}, {"detail", "Queued"}, {"at_unix_ms", ts}}})},
            {"error_message", ""},
            {"created_at_unix_ms", ts},
            {"started_at_unix_ms", 0},
            {"verifying_at_unix_ms", 0},
            {"finished_at_unix_ms", 0},
        };
        operations_.push_back(op);
        json resp{
            {"operation", std::move(op)},
            {"idempotency_key", std::string(idempotency_key)},
            {"idempotent_replay", false},
        };
        idem_[std::string(idempotency_key)] = {ep, hash, 202, resp};
        return {202, resp.dump()};
    }

    ChatOpHttpResult claim_next_queued_operation(AdminRole /*actor_role*/) override {
        std::lock_guard<std::mutex> lock(mu_);
        reconcile_locked();
        if (has_running_or_verifying_locked()) {
            return {200, json{{"claimed", false}}.dump()};
        }
        for (auto& op : operations_) {
            if (op.value("status", "") == "queued") {
                op["status"] = "running";
                op["started_at_unix_ms"] = now_ms();
                return {200, json{{"claimed", true}, {"operation", op}}.dump()};
            }
        }
        return {200, json{{"claimed", false}}.dump()};
    }

    ChatOpHttpResult report_worker_result(std::string_view operation_id,
                                          std::string_view outcome,
                                          std::string_view summary,
                                          AdminRole /*actor_role*/) override {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& op : operations_) {
            if (op.value("operation_id", "") == operation_id) {
                if (op.value("status", "") != "running") {
                    return err(409, "invalid_operation_state", "Not running.");
                }
                const std::uint64_t ts = now_ms();
                if (outcome == "command_succeeded") {
                    op["status"] = "verifying";
                    op["verifying_at_unix_ms"] = ts;
                    op["params"]["verifying_since_unix_ms"] = ts;
                    op["params"]["verify_deadline_unix_ms"] =
                        ts + std::max<std::uint32_t>(config_.operation_verify_timeout_ms, 10u);
                } else {
                    op["status"] = "failed";
                    op["error_message"] = std::string(summary);
                    op["finished_at_unix_ms"] = ts;
                }
                return {200, json{{"operation", op}}.dump()};
            }
        }
        return err(404, "operation_not_found", "Operation was not found.");
    }

    ChatOpHttpResult get_config() override {
        std::lock_guard<std::mutex> lock(mu_);
        const auto& latest = revisions_.back();
        json rev_meta = json::array();
        for (auto it = revisions_.rbegin(); it != revisions_.rend(); ++it) {
            rev_meta.push_back(*it);
        }
        return {200, build_config_overview_json(
                         config_,
                         latest.value("revision", 1LL),
                         latest.at("values").dump(),
                         rev_meta.dump())};
    }

    ChatOpHttpResult apply_config(const std::string& raw_body,
                                  std::string_view reason,
                                  std::string_view idempotency_key,
                                  AdminRole actor_role) override {
        if (reason.empty() || idempotency_key.empty()) {
            return err(400, "invalid_request", "Reason and idempotency_key required.");
        }
        json body = json::parse(raw_body);
        if (!body.contains("expected_revision") || !body["expected_revision"].is_number_integer()) {
            return err(400, "expected_revision_required", "expected_revision required.");
        }
        const std::int64_t expected_rev = body["expected_revision"].get<std::int64_t>();
        const std::string patch_str = body["values"].dump();

        std::lock_guard<std::mutex> lock(mu_);
        reconcile_locked();
        const std::string ep = "POST /api/v1/config/apply";
        const std::string hash = ep + "|" + std::to_string(expected_rev) + "|" + patch_str + "|" + std::string(reason);
        if (auto rep = check_idem_locked(idempotency_key, ep, hash)) {
            return *rep;
        }

        const auto& latest = revisions_.back();
        const std::int64_t current_rev = latest.value("revision", 1LL);
        if (expected_rev != current_rev) {
            return err(409, "revision_conflict", "Revision mismatch.");
        }

        const auto check = evaluate_config_patch(
            poller_, latest.at("values").dump(), patch_str, body.value("confirmation_target", ""));
        if (!check.ok) {
            return err(check.http_status, check.error_code, check.message);
        }
        if (has_active_locked()) {
            return err(409, "operation_in_progress", "Operation already active.");
        }

        return commit_config_locked("config_apply", std::nullopt, check, reason, idempotency_key, actor_role, ep, hash);
    }

    ChatOpHttpResult rollback_config(std::int64_t target_revision,
                                     std::string_view confirmation_target,
                                     std::string_view reason,
                                     std::string_view idempotency_key,
                                     AdminRole actor_role) override {
        if (reason.empty() || idempotency_key.empty()) {
            return err(400, "invalid_request", "Reason and idempotency_key required.");
        }
        std::lock_guard<std::mutex> lock(mu_);
        reconcile_locked();
        const std::string ep = "POST /api/v1/config/rollback";
        const std::string hash = ep + "|" + std::to_string(target_revision) + "|" + std::string(reason);
        if (auto rep = check_idem_locked(idempotency_key, ep, hash)) {
            return *rep;
        }

        std::string target_vals;
        for (const auto& rev : revisions_) {
            if (rev.value("revision", 0LL) == target_revision) {
                target_vals = rev.at("values").dump();
                break;
            }
        }
        if (target_vals.empty()) {
            return err(404, "revision_not_found", "Revision not found.");
        }

        const auto check = evaluate_config_patch(
            poller_, revisions_.back().at("values").dump(), target_vals, confirmation_target);
        if (!check.ok) {
            return err(check.http_status, check.error_code, check.message);
        }
        if (has_active_locked()) {
            return err(409, "operation_in_progress", "Operation already active.");
        }
        return commit_config_locked("config_rollback", target_revision, check, reason, idempotency_key, actor_role, ep, hash);
    }

    void record_denied_audit(AdminRole actor_role,
                             std::string_view capability,
                             std::string_view action,
                             std::string_view target,
                             std::string_view /*reason*/,
                             std::string_view /*idempotency_key*/,
                             std::string_view deny_code) noexcept override {
        std::lock_guard<std::mutex> lock(mu_);
        denied_audits_.push_back(json{
            {"actor_role", std::string(admin_role_to_string(actor_role))},
            {"capability", std::string(capability)},
            {"action", std::string(action)},
            {"target", std::string(target)},
            {"deny_code", std::string(deny_code)},
        });
    }

    std::vector<json> denied_audits() const {
        std::lock_guard<std::mutex> lock(mu_);
        return denied_audits_;
    }

private:
    struct IdemEntry {
        std::string ep;
        std::string hash;
        int status{202};
        json resp;
    };

    static ChatOpHttpResult err(int st, std::string_view code, std::string_view msg) {
        return {st, json{{"error", std::string(code)}, {"message", std::string(msg)}}.dump()};
    }

    std::string next_uuid() {
        ++seq_;
        std::ostringstream oss;
        oss << "00000000-0000-4000-8000-" << std::setw(12) << std::setfill('0') << seq_;
        return oss.str();
    }

    std::optional<ChatOpHttpResult> check_idem_locked(std::string_view key, const std::string& ep, const std::string& hash) {
        auto it = idem_.find(std::string(key));
        if (it == idem_.end()) {
            return std::nullopt;
        }
        if (it->second.ep == ep && it->second.hash == hash) {
            json rep = it->second.resp;
            rep["idempotent_replay"] = true;
            return ChatOpHttpResult{it->second.status, rep.dump()};
        }
        return err(409, "idempotency_conflict", "Idempotency key conflict.");
    }

    bool has_active_locked() const {
        return std::any_of(operations_.begin(), operations_.end(), [](const json& op) {
            const std::string st = op.value("status", "");
            return st == "queued" || st == "running" || st == "verifying";
        });
    }

    bool has_running_or_verifying_locked() const {
        return std::any_of(operations_.begin(), operations_.end(), [](const json& op) {
            const std::string st = op.value("status", "");
            return st == "running" || st == "verifying";
        });
    }

    void reconcile_locked() {
        const std::uint64_t ts = now_ms();
        for (auto& op : operations_) {
            if (op.value("status", "") != "verifying") {
                continue;
            }
            const auto affected = op.at("params").value("affected_services", std::vector<std::string>{});
            const auto since_ms = op.at("params").value("verifying_since_unix_ms", 0ULL);
            const auto deadline_ms = op.at("params").value("verify_deadline_unix_ms", 0ULL);
            const auto state = evaluate_verifying_services(poller_, affected, since_ms, deadline_ms, ts);
            if (state == VerifyProbeState::SUCCEEDED) {
                op["status"] = "succeeded";
                op["finished_at_unix_ms"] = ts;
            } else if (state == VerifyProbeState::TIMED_OUT) {
                op["status"] = "timed_out";
                op["error_message"] = "Verify deadline elapsed without /readyz recovery.";
                op["finished_at_unix_ms"] = ts;
            }
        }
    }

    ChatOpHttpResult commit_config_locked(std::string_view op_type,
                                          std::optional<std::int64_t> rollback_of,
                                          const ConfigValidationResult& check,
                                          std::string_view reason,
                                          std::string_view idem_key,
                                          AdminRole actor_role,
                                          const std::string& ep,
                                          const std::string& hash) {
        const std::string op_id = next_uuid();
        const std::uint64_t ts = now_ms();
        const json next_vals = json::parse(check.next_values_json);
        const std::string status = check.touches_restart_required ? "queued" : "succeeded";

        json op{
            {"operation_id", op_id},
            {"operation_type", std::string(op_type)},
            {"target_service", "stack-config"},
            {"status", status},
            {"actor_role", std::string(admin_role_to_string(actor_role))},
            {"reason", std::string(reason)},
            {"idempotency_key", std::string(idem_key)},
            {"params",
             {
                 {"managed_env", json::parse(check.managed_env_json)},
                 {"affected_services", check.affected_services},
             }},
            {"created_at_unix_ms", ts},
            {"finished_at_unix_ms", check.touches_restart_required ? 0ULL : ts},
        };
        operations_.push_back(op);

        const std::int64_t next_rev = static_cast<std::int64_t>(revisions_.size() + 1);
        revisions_.push_back(json{
            {"revision", next_rev},
            {"values", next_vals},
            {"diff_summary", json::parse(check.diff_summary_json)},
            {"apply_mode", check.apply_mode},
            {"actor_role", std::string(admin_role_to_string(actor_role))},
            {"reason", std::string(reason)},
            {"operation_id", op_id},
            {"rollback_of_revision", rollback_of.value_or(0)},
            {"created_at_unix_ms", ts},
        });

        poller_.update_runtime_tuning(
            static_cast<std::uint32_t>(next_vals.value("admin.poll_interval_ms", 5000LL)),
            static_cast<std::uint32_t>(next_vals.value("admin.stale_ttl_ms", 15000LL)));

        json resp{
            {"revision", next_rev},
            {"apply_mode", check.apply_mode},
            {"diff_summary", json::parse(check.diff_summary_json)},
            {"operation", std::move(op)},
            {"idempotency_key", std::string(idem_key)},
            {"idempotent_replay", false},
        };
        if (rollback_of.has_value()) {
            resp["rollback_of_revision"] = *rollback_of;
        }
        idem_[std::string(idem_key)] = {ep, hash, 202, resp};
        return {202, resp.dump()};
    }

    AdminConfig config_;
    InventoryPoller& poller_;
    mutable std::mutex mu_;
    std::vector<json> operations_;
    std::vector<json> revisions_;
    std::unordered_map<std::string, IdemEntry> idem_;
    std::vector<json> denied_audits_;
    std::uint64_t seq_{0};
};

AdminHttpRequest make_req(std::string method, std::string target, std::string token, std::string body = {}) {
    return AdminHttpRequest{
        std::move(method),
        std::move(target),
        token.empty()
            ? std::unordered_map<std::string, std::string>{}
            : std::unordered_map<std::string, std::string>{{"authorization", "Bearer " + token}},
        std::move(body),
        "127.0.0.1",
    };
}

} // namespace

TEST(AdminConsoleV3Test, RbacBlocksViewerAndOperatorAndReadOnlyRecordsDeniedAudit) {
    AdminConfig cfg;
    cfg.auth_mode = AdminAuthMode::TOKEN;
    cfg.read_only = false;
    cfg.viewer_token = "tok_v";
    cfg.operator_token = "tok_o";
    cfg.admin_token = "tok_a";
    cfg.web_root = ADMIN_WEB_ASSET_DIR;

    ServiceTargetConfig srv1{"server-1", "Server 1", ServiceKind::LONG_RUNNING, TopologyLayer::SERVER, ProbeType::HTTP_OK, "127.0.0.1", 0};
    InventoryPoller poller(cfg, {srv1});
    auto ops = std::make_shared<InMemoryOpsConfigBackend>(cfg, poller);
    AdminRouter router(cfg, poller, nullptr, ops);

    const std::string restart_body =
        R"({"operation_type":"service_restart","target_service":"server-1","confirmation_target":"server-1","reason":"rbac check","idempotency_key":"k1"})";

    for (const auto& tok : {std::string("tok_v"), std::string("tok_o")}) {
        const auto r1 = router.handle(make_req("POST", "/api/v1/operations", tok, restart_body));
        EXPECT_EQ(r1.status, "403 Forbidden");
        EXPECT_EQ(json::parse(r1.body).value("error", ""), "insufficient_role");

        const auto r2 = router.handle(make_req("POST", "/api/v1/operations/worker/claim", tok, "{}"));
        EXPECT_EQ(r2.status, "403 Forbidden");

        const auto r3 = router.handle(make_req(
            "POST",
            "/api/v1/config/apply",
            tok,
            R"({"expected_revision":1,"values":{"admin.poll_interval_ms":4000},"confirmation_target":"stack-config","reason":"rbac","idempotency_key":"k2"})"));
        EXPECT_EQ(r3.status, "403 Forbidden");

        const auto r4 = router.handle(make_req(
            "POST",
            "/api/v1/config/rollback",
            tok,
            R"({"target_revision":1,"confirmation_target":"stack-config","reason":"rbac","idempotency_key":"k3"})"));
        EXPECT_EQ(r4.status, "403 Forbidden");
    }

    EXPECT_EQ(ops->denied_audits().size(), 8U);

    // ADMIN_READ_ONLY=1 blocks even AdminRole::ADMIN
    cfg.read_only = true;
    AdminRouter ro_router(cfg, poller, nullptr, ops);
    const auto ro_res = ro_router.handle(make_req("POST", "/api/v1/operations", "tok_a", restart_body));
    EXPECT_EQ(ro_res.status, "403 Forbidden");
    EXPECT_EQ(json::parse(ro_res.body).value("error", ""), "read_only_mode");
}

TEST(AdminConsoleV3Test, AllowlistConfirmationSiblingReadinessConcurrencyAndIdempotency) {
    std::atomic<bool> s1_ready{true};
    std::atomic<bool> s2_ready{false};

    ScopedHttpMockServer s1_srv([&](std::string_view target) -> ScopedHttpMockServer::HttpReply {
        if (target == "/healthz") return {"200 OK", "ok"};
        if (target == "/readyz") return {s1_ready.load() ? "200 OK" : "503 Service Unavailable", "s1"};
        return {"404 Not Found", ""};
    });
    ScopedHttpMockServer s2_srv([&](std::string_view target) -> ScopedHttpMockServer::HttpReply {
        if (target == "/healthz") return {"200 OK", "ok"};
        if (target == "/readyz") return {s2_ready.load() ? "200 OK" : "503 Service Unavailable", "s2"};
        return {"404 Not Found", ""};
    });

    AdminConfig cfg;
    cfg.auth_mode = AdminAuthMode::TOKEN;
    cfg.read_only = false;
    cfg.admin_token = "tok_admin";
    cfg.web_root = ADMIN_WEB_ASSET_DIR;

    ServiceTargetConfig t_s1{"server-1", "Server 1", ServiceKind::LONG_RUNNING, TopologyLayer::SERVER, ProbeType::HTTP_HEALTH_READY, "127.0.0.1", s1_srv.port()};
    ServiceTargetConfig t_s2{"server-2", "Server 2", ServiceKind::LONG_RUNNING, TopologyLayer::SERVER, ProbeType::HTTP_HEALTH_READY, "127.0.0.1", s2_srv.port()};
    ServiceTargetConfig t_worker{"wb_worker", "Write-Behind Worker", ServiceKind::LONG_RUNNING, TopologyLayer::WORKER, ProbeType::HTTP_OK, "127.0.0.1", s1_srv.port(), "/healthz", "", ""};
    ServiceTargetConfig t_pg{"postgres", "PostgreSQL", ServiceKind::INFRASTRUCTURE, TopologyLayer::DATA, ProbeType::POSTGRES_SQL, "127.0.0.1", 35432};
    ServiceTargetConfig t_edge{"haproxy", "HAProxy", ServiceKind::LONG_RUNNING, TopologyLayer::EDGE, ProbeType::HTTP_OK, "127.0.0.1", 8404};

    InventoryPoller poller(cfg, {t_s1, t_s2, t_worker, t_pg, t_edge});
    poller.poll_once();
    auto ops = std::make_shared<InMemoryOpsConfigBackend>(cfg, poller);
    AdminRouter router(cfg, poller, nullptr, ops);

    // Infrastructure / edge / unknown rejected 400 target_not_allowlisted
    for (const auto& forbidden : {std::string("postgres"), std::string("haproxy"), std::string("admin")}) {
        const auto res = router.handle(make_req(
            "POST",
            "/api/v1/operations",
            "tok_admin",
            json{
                {"operation_type", "service_restart"},
                {"target_service", forbidden},
                {"confirmation_target", forbidden},
                {"reason", "bad target"},
                {"idempotency_key", "k-bad-" + forbidden},
            }.dump()));
        EXPECT_EQ(res.status, "400 Bad Request");
        EXPECT_EQ(json::parse(res.body).value("error", ""), "target_not_allowlisted");
    }

    // Wrong confirmation_target rejected 400
    const auto bad_conf = router.handle(make_req(
        "POST",
        "/api/v1/operations",
        "tok_admin",
        R"({"operation_type":"service_restart","target_service":"server-2","confirmation_target":"server-1","reason":"typo","idempotency_key":"k-typo"})"));
    EXPECT_EQ(bad_conf.status, "400 Bad Request");
    EXPECT_EQ(json::parse(bad_conf.body).value("error", ""), "confirmation_mismatch");

    // Sibling guard: s2 is NOT_READY, so restarting s1 (only ready sibling) is blocked 409 peer_not_ready
    const auto peer_blk = router.handle(make_req(
        "POST",
        "/api/v1/operations",
        "tok_admin",
        R"({"operation_type":"service_restart","target_service":"server-1","confirmation_target":"server-1","reason":"unsafe","idempotency_key":"k-peer"})"));
    EXPECT_EQ(peer_blk.status, "409 Conflict");
    EXPECT_EQ(json::parse(peer_blk.body).value("error", ""), "peer_not_ready");

    // Restarting crashed server-2 while server-1 IS ready succeeds (202 Accepted)
    const std::string valid_body =
        R"({"operation_type":"service_restart","target_service":"server-2","confirmation_target":"server-2","reason":"recover s2","idempotency_key":"k-s2"})";
    const auto ok1 = router.handle(make_req("POST", "/api/v1/operations", "tok_admin", valid_body));
    EXPECT_EQ(ok1.status, "202 Accepted");
    const json j1 = json::parse(ok1.body);
    EXPECT_FALSE(j1.value("idempotent_replay", true));
    const std::string op_id = j1.at("operation").value("operation_id", "");

    // Idempotent replay
    const auto rep = router.handle(make_req("POST", "/api/v1/operations", "tok_admin", valid_body));
    EXPECT_EQ(rep.status, "202 Accepted");
    EXPECT_TRUE(json::parse(rep.body).value("idempotent_replay", false));
    EXPECT_EQ(json::parse(rep.body).at("operation").value("operation_id", ""), op_id);

    // Conflict on reused key with different target
    const auto conf = router.handle(make_req(
        "POST",
        "/api/v1/operations",
        "tok_admin",
        R"({"operation_type":"service_restart","target_service":"wb_worker","confirmation_target":"wb_worker","reason":"diff","idempotency_key":"k-s2"})"));
    EXPECT_EQ(conf.status, "409 Conflict");
    EXPECT_EQ(json::parse(conf.body).value("error", ""), "idempotency_conflict");

    // Concurrency guard blocks another service operation while k-s2 is queued
    const auto busy = router.handle(make_req(
        "POST",
        "/api/v1/operations",
        "tok_admin",
        R"({"operation_type":"service_restart","target_service":"wb_worker","confirmation_target":"wb_worker","reason":"concurrent","idempotency_key":"k-worker-2"})"));
    EXPECT_EQ(busy.status, "409 Conflict");
    EXPECT_EQ(json::parse(busy.body).value("error", ""), "operation_in_progress");
}

TEST(AdminConsoleV3Test, AsyncLifecycleStateMachineVerifyingSucceededAndTimedOut) {
    std::atomic<bool> s1_ready{true};
    std::atomic<bool> s2_ready{true};

    ScopedHttpMockServer s1_srv([&](std::string_view target) -> ScopedHttpMockServer::HttpReply {
        if (target == "/healthz") return {"200 OK", "ok"};
        if (target == "/readyz") return {s1_ready.load() ? "200 OK" : "503 Service Unavailable", "s1"};
        return {"404 Not Found", ""};
    });
    ScopedHttpMockServer s2_srv([&](std::string_view target) -> ScopedHttpMockServer::HttpReply {
        if (target == "/healthz") return {"200 OK", "ok"};
        if (target == "/readyz") return {s2_ready.load() ? "200 OK" : "503 Service Unavailable", "s2"};
        return {"404 Not Found", ""};
    });

    AdminConfig cfg;
    cfg.auth_mode = AdminAuthMode::TOKEN;
    cfg.read_only = false;
    cfg.admin_token = "tok_admin";
    cfg.operation_verify_timeout_ms = 50;
    cfg.web_root = ADMIN_WEB_ASSET_DIR;

    ServiceTargetConfig t_s1{"server-1", "Server 1", ServiceKind::LONG_RUNNING, TopologyLayer::SERVER, ProbeType::HTTP_HEALTH_READY, "127.0.0.1", s1_srv.port()};
    ServiceTargetConfig t_s2{"server-2", "Server 2", ServiceKind::LONG_RUNNING, TopologyLayer::SERVER, ProbeType::HTTP_HEALTH_READY, "127.0.0.1", s2_srv.port()};

    InventoryPoller poller(cfg, {t_s1, t_s2});
    poller.poll_once();
    auto ops = std::make_shared<InMemoryOpsConfigBackend>(cfg, poller);
    AdminRouter router(cfg, poller, nullptr, ops);

    // 1. Happy path: queued -> running -> verifying -> succeeded
    const auto q1 = router.handle(make_req(
        "POST",
        "/api/v1/operations",
        "tok_admin",
        R"({"operation_type":"service_restart","target_service":"server-1","confirmation_target":"server-1","reason":"rolling","idempotency_key":"k-life-1"})"));
    ASSERT_EQ(q1.status, "202 Accepted");
    const std::string op1_id = json::parse(q1.body).at("operation").value("operation_id", "");

    const auto c1 = router.handle(make_req("POST", "/api/v1/operations/worker/claim", "tok_admin", "{}"));
    ASSERT_EQ(c1.status, "200 OK");
    EXPECT_TRUE(json::parse(c1.body).value("claimed", false));
    EXPECT_EQ(json::parse(c1.body).at("operation").value("status", ""), "running");

    const auto w1 = router.handle(make_req(
        "POST",
        "/api/v1/operations/" + op1_id + "/worker-result",
        "tok_admin",
        R"({"outcome":"command_succeeded","summary":"restarted server-1"})"));
    ASSERT_EQ(w1.status, "200 OK");
    EXPECT_EQ(json::parse(w1.body).at("operation").value("status", ""), "verifying");

    const auto g1 = router.handle(make_req("GET", "/api/v1/operations/" + op1_id, "tok_admin"));
    ASSERT_EQ(g1.status, "200 OK");
    EXPECT_EQ(json::parse(g1.body).at("operation").value("status", ""), "succeeded");

    // 2. Post-restart verify failure: command exits 0, but s2 /readyz stays 503 past verify_deadline -> timed_out
    s2_ready.store(false);
    poller.poll_once();
    const auto q2 = router.handle(make_req(
        "POST",
        "/api/v1/operations",
        "tok_admin",
        R"({"operation_type":"service_restart","target_service":"server-2","confirmation_target":"server-2","reason":"failing probe","idempotency_key":"k-life-2"})"));
    ASSERT_EQ(q2.status, "202 Accepted");
    const std::string op2_id = json::parse(q2.body).at("operation").value("operation_id", "");

    ASSERT_EQ(router.handle(make_req("POST", "/api/v1/operations/worker/claim", "tok_admin", "{}")).status, "200 OK");
    ASSERT_EQ(
        router.handle(make_req(
                          "POST",
                          "/api/v1/operations/" + op2_id + "/worker-result",
                          "tok_admin",
                          R"({"outcome":"command_succeeded","summary":"docker exited 0"})"))
            .status,
        "200 OK");

    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const auto g2 = router.handle(make_req("GET", "/api/v1/operations/" + op2_id, "tok_admin"));
    ASSERT_EQ(g2.status, "200 OK");
    EXPECT_EQ(json::parse(g2.body).at("operation").value("status", ""), "timed_out");
}

TEST(AdminConsoleV3Test, SchemaValidationRuntimeTuningConflictRollbackAndSecretMasking) {
    AdminConfig cfg;
    cfg.auth_mode = AdminAuthMode::TOKEN;
    cfg.read_only = false;
    cfg.admin_token = "tok_admin_v3_secret";
    cfg.db_uri = "postgresql://dynaxis:super_secret_pw@127.0.0.1:5432/dynaxis_db";
    cfg.redis_uri = "tcp://:redis_pw@127.0.0.1:6379";
    cfg.web_root = ADMIN_WEB_ASSET_DIR;

    ServiceTargetConfig t_s1{"server-1", "Server 1", ServiceKind::LONG_RUNNING, TopologyLayer::SERVER, ProbeType::HTTP_OK, "127.0.0.1", 39091};
    InventoryPoller poller(cfg, {t_s1});
    auto ops = std::make_shared<InMemoryOpsConfigBackend>(cfg, poller);
    AdminRouter router(cfg, poller, nullptr, ops);

    // GET /api/v1/config masks all secret_refs and leaks zero secret values
    const auto cfg_res = router.handle(make_req("GET", "/api/v1/config", "tok_admin_v3_secret"));
    ASSERT_EQ(cfg_res.status, "200 OK");
    EXPECT_EQ(cfg_res.body.find("super_secret_pw"), std::string::npos);
    EXPECT_EQ(cfg_res.body.find("redis_pw"), std::string::npos);
    EXPECT_EQ(cfg_res.body.find("tok_admin_v3_secret"), std::string::npos);
    const json cfg_json = json::parse(cfg_res.body);
    EXPECT_EQ(cfg_json.value("current_revision", 0LL), 1);

    // Reject secret_ref and unknown keys
    const auto sec_rej = router.handle(make_req(
        "POST",
        "/api/v1/config/apply",
        "tok_admin_v3_secret",
        R"({"expected_revision":1,"values":{"CHAT_AUTH_SIGNING_SECRET":"hijack"},"confirmation_target":"stack-config","reason":"bad","idempotency_key":"c-sec"})"));
    EXPECT_EQ(sec_rej.status, "400 Bad Request");
    EXPECT_EQ(json::parse(sec_rej.body).value("error", ""), "secret_or_unknown_key_forbidden");

    // Reject out-of-range integer & invalid boolean type
    const auto range_rej = router.handle(make_req(
        "POST",
        "/api/v1/config/apply",
        "tok_admin_v3_secret",
        R"({"expected_revision":1,"values":{"admin.poll_interval_ms":999999},"confirmation_target":"stack-config","reason":"bad","idempotency_key":"c-rng"})"));
    EXPECT_EQ(range_rej.status, "400 Bad Request");
    EXPECT_EQ(json::parse(range_rej.body).value("error", ""), "schema_validation_failed");

    const auto bool_rej = router.handle(make_req(
        "POST",
        "/api/v1/config/apply",
        "tok_admin_v3_secret",
        R"({"expected_revision":1,"values":{"SESSION_CONTINUITY_ENABLED":"yes"},"confirmation_target":"stack-config","reason":"bad","idempotency_key":"c-bool"})"));
    EXPECT_EQ(bool_rej.status, "400 Bad Request");
    EXPECT_EQ(json::parse(bool_rej.body).value("error", ""), "schema_validation_failed");

    // Apply runtime-only tuning -> immediate poller reflection & /api/v1/meta update
    const auto rt_ok = router.handle(make_req(
        "POST",
        "/api/v1/config/apply",
        "tok_admin_v3_secret",
        R"({"expected_revision":1,"values":{"admin.poll_interval_ms":3500,"admin.stale_ttl_ms":12000},"confirmation_target":"stack-config","reason":"tune","idempotency_key":"c-rt-1"})"));
    ASSERT_EQ(rt_ok.status, "202 Accepted");
    const json rt_json = json::parse(rt_ok.body);
    EXPECT_EQ(rt_json.value("revision", 0LL), 2);
    EXPECT_EQ(rt_json.value("apply_mode", ""), "runtime");
    EXPECT_EQ(rt_json.at("operation").value("status", ""), "succeeded");
    EXPECT_EQ(poller.poll_interval_ms(), 3500U);
    EXPECT_EQ(poller.stale_ttl_ms(), 12000U);

    const auto meta_res = router.handle(make_req("GET", "/api/v1/meta", "tok_admin_v3_secret"));
    ASSERT_EQ(meta_res.status, "200 OK");
    EXPECT_EQ(json::parse(meta_res.body).value("poll_interval_ms", 0U), 3500U);
    EXPECT_EQ(json::parse(meta_res.body).value("stale_ttl_ms", 0U), 12000U);

    // Stale revision conflict
    const auto stale_rev = router.handle(make_req(
        "POST",
        "/api/v1/config/apply",
        "tok_admin_v3_secret",
        R"({"expected_revision":1,"values":{"admin.poll_interval_ms":4200},"confirmation_target":"stack-config","reason":"stale","idempotency_key":"c-stale"})"));
    EXPECT_EQ(stale_rev.status, "409 Conflict");
    EXPECT_EQ(json::parse(stale_rev.body).value("error", ""), "revision_conflict");

    // Rollback to revision 1 restores default 5000 / 15000 immediately
    const auto rb_ok = router.handle(make_req(
        "POST",
        "/api/v1/config/rollback",
        "tok_admin_v3_secret",
        R"({"target_revision":1,"confirmation_target":"stack-config","reason":"restore defaults","idempotency_key":"c-rb-1"})"));
    ASSERT_EQ(rb_ok.status, "202 Accepted");
    EXPECT_EQ(json::parse(rb_ok.body).value("revision", 0LL), 3);
    EXPECT_EQ(poller.poll_interval_ms(), 5000U);
    EXPECT_EQ(poller.stale_ttl_ms(), 15000U);
}
