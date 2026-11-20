#include "admin/admin_router.hpp"
#include "admin/admin_types.hpp"
#include "admin/chat_ops_backend.hpp"
#include "admin/inventory_poller.hpp"
#include "admin/resp_probe.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <memory>
#include <mutex>
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

class FakeChatOpsBackend final : public IChatOpsBackend {
public:
    struct SanctionRecord {
        std::string id;
        std::string subject;
        std::string kind;
        std::string reason;
        std::string actor_role;
        std::int64_t created_ms{1000};
        std::int64_t expires_ms{0};
        std::int64_t revoked_ms{0};
        bool active{true};
    };

    struct RoomRecord {
        std::string room_id;
        std::string name;
        bool is_active{true};
        bool is_locked{false};
        std::string owner;
        std::int64_t watermark{42};
        std::vector<std::string> members;
    };

    struct IdemEntry {
        std::string endpoint;
        std::string request_hash;
        int http_status{200};
        json response_json;
    };

    FakeChatOpsBackend() {
        rooms_.push_back({"00000000-0000-0000-0000-000000000001", "lobby", true, false, "", 10, {"alice"}});
        rooms_.push_back({"00000000-0000-0000-0000-000000000002", "general", true, false, "alice", 42, {"alice", "bob"}});
    }

    ChatOpHttpResult list_rooms(std::string_view query, bool include_closed, std::size_t limit) override {
        std::lock_guard<std::mutex> lock(mu_);
        json arr = json::array();
        for (const auto& rm : rooms_) {
            if (!include_closed && !rm.is_active) continue;
            if (!query.empty() && rm.name.find(query) == std::string::npos) continue;
            arr.push_back(json{
                {"room_id", rm.room_id},
                {"name", rm.name},
                {"is_public", !rm.is_locked},
                {"is_active", rm.is_active},
                {"is_locked", rm.is_locked},
                {"owner", rm.owner},
                {"watermark", rm.watermark},
                {"db_member_count", rm.members.size()},
                {"shared_member_count", rm.members.size()},
                {"online_count", rm.members.size()},
                {"created_at_unix_ms", 1700000000000LL},
                {"closed_at_unix_ms", rm.is_active ? 0LL : 1700000010000LL},
            });
            if (arr.size() >= limit) break;
        }
        return {200, json{{"rooms", std::move(arr)}, {"redis_connected", true}}.dump()};
    }

    ChatOpHttpResult get_room_detail(std::string_view room_name) override {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& rm : rooms_) {
            if (rm.name == room_name) {
                return {200, json{
                    {"room",
                     {
                         {"room_id", rm.room_id},
                         {"name", rm.name},
                         {"is_public", !rm.is_locked},
                         {"is_active", rm.is_active},
                         {"is_locked", rm.is_locked},
                         {"owner", rm.owner},
                         {"watermark", rm.watermark},
                         {"online_members", rm.members},
                         {"shared_members", rm.members},
                         {"db_members", json::array()},
                         {"created_at_unix_ms", 1700000000000LL},
                     }},
                    {"privacy_notice", "Chat message plaintext content is intentionally excluded from admin console views."},
                }.dump()};
            }
        }
        return {404, json{{"error", "room_not_found"}, {"message", "Room not found"}}.dump()};
    }

    ChatOpHttpResult close_room(std::string_view room_name,
                                std::string_view reason,
                                std::string_view idempotency_key,
                                AdminRole actor_role) override {
        if (room_name == "lobby") {
            return {400, json{{"error", "protected_room"}, {"message", "lobby cannot be closed"}}.dump()};
        }
        if (reason.empty() || idempotency_key.empty()) {
            return {400, json{{"error", "validation_failed"}, {"message", "reason and idempotency_key required"}}.dump()};
        }

        std::lock_guard<std::mutex> lock(mu_);
        const std::string ep = "POST /api/v1/chat/rooms/" + std::string(room_name) + "/close";
        const std::string hash = std::string(reason);
        if (auto dup = check_idem_locked(idempotency_key, ep, hash, actor_role, "close_room", room_name, reason)) {
            return *dup;
        }

        for (auto& rm : rooms_) {
            if (rm.name == room_name) {
                rm.is_active = false;
                const json resp{
                    {"status", "closed"},
                    {"room", rm.name},
                    {"idempotency_key", std::string(idempotency_key)},
                    {"idempotent_replay", false},
                };
                save_idem_locked(idempotency_key, ep, hash, 200, resp);
                append_audit_locked(actor_role, "chat.moderation", "close_room", rm.name, reason, idempotency_key, "succeeded");
                return {200, resp.dump()};
            }
        }
        return {404, json{{"error", "room_not_found"}, {"message", "Room not found"}}.dump()};
    }

    ChatOpHttpResult list_users(std::string_view query, std::size_t /*limit*/) override {
        std::lock_guard<std::mutex> lock(mu_);
        json arr = json::array();
        if (query.empty() || std::string("alice").find(query) != std::string::npos) {
            arr.push_back(json{
                {"user_id", "11111111-1111-1111-1111-111111111111"},
                {"subject", "alice"},
                {"lease_active", alice_lease_active_},
                {"lease_ttl_ms", alice_lease_active_ ? 115000 : 0},
                {"logical_session_id", "22222222-2222-2222-2222-222222222222"},
                {"presence_exists", alice_lease_active_},
                {"presence_ttl_ms", alice_lease_active_ ? 58000 : -1},
                {"muted", false},
                {"banned", false},
            });
        }
        return {200, json{{"users", std::move(arr)}, {"redis_connected", true}}.dump()};
    }

    ChatOpHttpResult get_user_detail(std::string_view subject) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (subject != "alice") {
            return {404, json{{"error", "user_not_found"}, {"message", "User not found"}}.dump()};
        }
        return {200, json{
            {"subject", "alice"},
            {"user_id", "11111111-1111-1111-1111-111111111111"},
            {"state_alignment", alice_lease_active_ ? "aligned" : "offline_aligned"},
            {"postgres_truth",
             {
                 {"lease_present", alice_lease_active_},
                 {"lease_active", alice_lease_active_},
                 {"lease_ttl_ms", alice_lease_active_ ? 115000 : 0},
                 {"logical_session_id", "22222222-2222-2222-2222-222222222222"},
                 {"muted", false},
                 {"banned", false},
                 {"sessions", json::array()},
             }},
            {"redis_state",
             {
                 {"connected", true},
                 {"presence_exists", alice_lease_active_},
                 {"presence_ttl_ms", alice_lease_active_ ? 58000 : -1},
                 {"continuity_room", "general"},
                 {"gateway_backend", "server-1"},
                 {"rooms", alice_lease_active_ ? json::array({"general"}) : json::array()},
             }},
            {"sanctions", json::array()},
            {"secret_policy", "access_token, resume_token, and lease token fields are strictly excluded."},
        }.dump()};
    }

    ChatOpHttpResult kick_user(std::string_view subject,
                               std::string_view reason,
                               std::string_view idempotency_key,
                               AdminRole actor_role) override {
        if (reason.empty() || idempotency_key.empty()) {
            return {400, json{{"error", "validation_failed"}, {"message", "reason and idempotency_key required"}}.dump()};
        }
        std::lock_guard<std::mutex> lock(mu_);
        const std::string ep = "POST /api/v1/chat/users/" + std::string(subject) + "/kick";
        const std::string hash = std::string(reason);
        if (auto dup = check_idem_locked(idempotency_key, ep, hash, actor_role, "kick_user", subject, reason)) {
            return *dup;
        }
        if (subject == "alice") {
            alice_lease_active_ = false;
        }
        const json resp{
            {"status", "kicked"},
            {"subject", std::string(subject)},
            {"lease_cleared", true},
            {"idempotency_key", std::string(idempotency_key)},
            {"idempotent_replay", false},
        };
        save_idem_locked(idempotency_key, ep, hash, 200, resp);
        append_audit_locked(actor_role, "chat.moderation", "kick_user", subject, reason, idempotency_key, "succeeded");
        return {200, resp.dump()};
    }

    ChatOpHttpResult list_sanctions(bool active_only, std::string_view subject, std::size_t /*limit*/) override {
        std::lock_guard<std::mutex> lock(mu_);
        json arr = json::array();
        for (const auto& s : sanctions_) {
            if (active_only && !s.active) continue;
            if (!subject.empty() && s.subject.find(subject) == std::string::npos) continue;
            arr.push_back(json{
                {"sanction_id", s.id},
                {"subject", s.subject},
                {"kind", s.kind},
                {"reason", s.reason},
                {"actor_role", s.actor_role},
                {"created_at_unix_ms", s.created_ms},
                {"expires_at_unix_ms", s.expires_ms},
                {"revoked_at_unix_ms", s.revoked_ms},
                {"active", s.active},
            });
        }
        return {200, json{{"sanctions", std::move(arr)}}.dump()};
    }

    ChatOpHttpResult create_sanction(std::string_view subject,
                                     std::string_view kind,
                                     std::uint64_t duration_sec,
                                     bool confirm_permanent,
                                     std::string_view reason,
                                     std::string_view idempotency_key,
                                     AdminRole actor_role) override {
        if (subject.empty() || reason.empty() || idempotency_key.empty()) {
            return {400, json{{"error", "validation_failed"}, {"message", "missing required fields"}}.dump()};
        }
        if (duration_sec > 86400ULL && !confirm_permanent) {
            return {400, json{{"error", "confirmation_required"}, {"message", "confirm_permanent required"}}.dump()};
        }

        std::lock_guard<std::mutex> lock(mu_);
        const std::string ep = "POST /api/v1/chat/sanctions";
        const std::string hash =
            std::string(subject) + "|" + std::string(kind) + "|" + std::to_string(duration_sec) + "|" + std::string(reason);
        if (auto dup = check_idem_locked(idempotency_key, ep, hash, actor_role, "create_sanction", subject, reason)) {
            return *dup;
        }

        const std::string id = "aaaaaaaa-aaaa-4aaa-8aaa-00000000000" + std::to_string(sanctions_.size() + 1);
        SanctionRecord rec;
        rec.id = id;
        rec.subject = std::string(subject);
        rec.kind = std::string(kind);
        rec.reason = std::string(reason);
        rec.actor_role = std::string(admin_role_to_string(actor_role));
        rec.expires_ms = 1700000000000LL + static_cast<std::int64_t>(duration_sec * 1000ULL);
        rec.active = true;
        sanctions_.push_back(rec);

        const json resp{
            {"sanction_id", id},
            {"subject", rec.subject},
            {"kind", rec.kind},
            {"duration_sec", duration_sec},
            {"expires_at_unix_ms", rec.expires_ms},
            {"idempotency_key", std::string(idempotency_key)},
            {"idempotent_replay", false},
        };
        save_idem_locked(idempotency_key, ep, hash, 201, resp);
        append_audit_locked(actor_role, "chat.moderation", "create_sanction_" + rec.kind, rec.subject, reason, idempotency_key, "succeeded");
        return {201, resp.dump()};
    }

    ChatOpHttpResult revoke_sanction(std::string_view sanction_id,
                                     std::string_view reason,
                                     std::string_view idempotency_key,
                                     AdminRole actor_role) override {
        if (reason.empty() || idempotency_key.empty()) {
            return {400, json{{"error", "validation_failed"}, {"message", "reason and idempotency_key required"}}.dump()};
        }
        std::lock_guard<std::mutex> lock(mu_);
        const std::string ep = "DELETE /api/v1/chat/sanctions/" + std::string(sanction_id);
        const std::string hash = std::string(reason);
        if (auto dup = check_idem_locked(idempotency_key, ep, hash, actor_role, "revoke_sanction", sanction_id, reason)) {
            return *dup;
        }

        for (auto& s : sanctions_) {
            if (s.id == sanction_id) {
                s.active = false;
                s.revoked_ms = 1700000005000LL;
                const json resp{
                    {"status", "revoked"},
                    {"sanction_id", s.id},
                    {"subject", s.subject},
                    {"kind", s.kind},
                    {"idempotency_key", std::string(idempotency_key)},
                    {"idempotent_replay", false},
                };
                save_idem_locked(idempotency_key, ep, hash, 200, resp);
                append_audit_locked(actor_role, "chat.moderation", "revoke_sanction", s.subject, reason, idempotency_key, "succeeded");
                return {200, resp.dump()};
            }
        }
        return {404, json{{"error", "sanction_not_found"}, {"message", "Sanction not found"}}.dump()};
    }

    ChatOpHttpResult inspect_dlq(std::size_t /*limit*/) override {
        return {200, json{
            {"wb_stream", "session_events"},
            {"dlq_stream", "session_events_dlq"},
            {"dead_stream", "session_events_dead"},
            {"pending_stream_length", 0},
            {"dlq_length", 1},
            {"dead_length", 0},
            {"entries", json::array()},
        }.dump()};
    }

    ChatOpHttpResult replay_dlq(std::size_t max_items,
                                std::string_view reason,
                                std::string_view idempotency_key,
                                AdminRole actor_role) override {
        std::lock_guard<std::mutex> lock(mu_);
        const json resp{
            {"status", "completed"},
            {"attempted", max_items},
            {"replayed_ok", 1},
            {"retried_dlq", 0},
            {"moved_dead", 0},
            {"idempotency_key", std::string(idempotency_key)},
            {"idempotent_replay", false},
        };
        append_audit_locked(actor_role, "wb.dlq", "replay_dlq", "session_events_dlq", reason, idempotency_key, "succeeded");
        return {200, resp.dump()};
    }

    ChatOpHttpResult list_audit_logs(std::string_view /*actor_filter*/,
                                     std::string_view /*action_filter*/,
                                     std::string_view /*target_filter*/,
                                     std::string_view status_filter,
                                     std::size_t limit) override {
        std::lock_guard<std::mutex> lock(mu_);
        json arr = json::array();
        for (auto it = audit_logs_.rbegin(); it != audit_logs_.rend(); ++it) {
            if (!status_filter.empty() && (*it).value("status", "") != status_filter) continue;
            arr.push_back(*it);
            if (arr.size() >= limit) break;
        }
        return {200, json{{"audit_logs", std::move(arr)}}.dump()};
    }

    void record_denied_audit(AdminRole actor_role,
                             std::string_view capability,
                             std::string_view action,
                             std::string_view target,
                             std::string_view reason,
                             std::string_view idempotency_key,
                             std::string_view /*deny_code*/) noexcept override {
        std::lock_guard<std::mutex> lock(mu_);
        append_audit_locked(actor_role, capability, action, target, reason, idempotency_key, "denied");
    }

private:
    std::optional<ChatOpHttpResult> check_idem_locked(std::string_view key,
                                                      const std::string& ep,
                                                      const std::string& hash,
                                                      AdminRole actor_role,
                                                      std::string_view action,
                                                      std::string_view target,
                                                      std::string_view reason) {
        const auto it = idem_.find(std::string(key));
        if (it == idem_.end()) {
            return std::nullopt;
        }
        if (it->second.endpoint == ep && it->second.request_hash == hash) {
            json replay = it->second.response_json;
            replay["idempotent_replay"] = true;
            return ChatOpHttpResult{it->second.http_status, replay.dump()};
        }
        append_audit_locked(actor_role, "chat.moderation", action, target, reason, key, "conflict");
        return ChatOpHttpResult{
            409,
            json{{"error", "idempotency_conflict"}, {"message", "Idempotency conflict"}}.dump(),
        };
    }

    void save_idem_locked(std::string_view key,
                          std::string ep,
                          std::string hash,
                          int status,
                          json resp) {
        idem_[std::string(key)] = IdemEntry{std::move(ep), std::move(hash), status, std::move(resp)};
    }

    void append_audit_locked(AdminRole role,
                              std::string_view capability,
                              std::string_view action,
                              std::string_view target,
                              std::string_view reason,
                              std::string_view idem_key,
                              std::string_view status) {
        audit_logs_.push_back(json{
            {"id", static_cast<std::int64_t>(audit_logs_.size() + 1)},
            {"created_at_unix_ms", 1700000000000LL},
            {"actor_role", std::string(admin_role_to_string(role))},
            {"capability", std::string(capability)},
            {"action", std::string(action)},
            {"target", std::string(target)},
            {"reason", std::string(reason)},
            {"idempotency_key", std::string(idem_key)},
            {"status", std::string(status)},
            {"summary", json::object()},
        });
    }

    std::mutex mu_;
    std::vector<RoomRecord> rooms_;
    std::vector<SanctionRecord> sanctions_;
    std::unordered_map<std::string, IdemEntry> idem_;
    std::vector<json> audit_logs_;
    bool alice_lease_active_{true};
};

class ScopedRespPipelineMock {
public:
    explicit ScopedRespPipelineMock(std::string scripted_reply)
        : reply_(std::move(scripted_reply)),
          acceptor_(ioc_, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)),
          port_(acceptor_.local_endpoint().port()) {
        do_accept();
        worker_ = std::thread([this]() { ioc_.run(); });
    }

    ~ScopedRespPipelineMock() {
        if (!stopped_.exchange(true)) {
            asio::post(ioc_, [this]() {
                boost::system::error_code ec;
                acceptor_.cancel(ec);
                acceptor_.close(ec);
            });
            if (worker_.joinable()) {
                worker_.join();
            }
            boost::system::error_code ec;
            acceptor_.close(ec);
        }
    }

    unsigned short port() const noexcept {
        return port_;
    }

private:
    void do_accept() {
        if (stopped_.load()) {
            return;
        }
        auto socket = std::make_shared<tcp::socket>(ioc_);
        acceptor_.async_accept(*socket, [this, socket](const boost::system::error_code& ec) {
            if (!ec && !stopped_.load()) {
                handle_client(*socket);
            }
            if (!stopped_.load()) {
                do_accept();
            }
        });
    }

    void handle_client(tcp::socket& socket) {
        boost::system::error_code ec;
        std::array<char, 1024> buf{};
        (void)socket.read_some(asio::buffer(buf), ec);
        asio::write(socket, asio::buffer(reply_), ec);
        socket.shutdown(tcp::socket::shutdown_both, ec);
        socket.close(ec);
    }

    std::string reply_;
    asio::io_context ioc_;
    tcp::acceptor acceptor_;
    unsigned short port_{0};
    std::atomic<bool> stopped_{false};
    std::thread worker_;
};

std::string resp_bulk(std::string_view s) {
    return "$" + std::to_string(s.size()) + "\r\n" + std::string(s) + "\r\n";
}

} // namespace

TEST(AdminConsoleV2Test, RbacAndReadOnlyGuardsRecordDeniedAudit) {
    AdminConfig cfg;
    cfg.auth_mode = AdminAuthMode::TOKEN;
    cfg.read_only = true;
    cfg.viewer_token = "tok_viewer";
    cfg.operator_token = "tok_operator";
    cfg.admin_token = "tok_admin";
    cfg.web_root = ADMIN_WEB_ASSET_DIR;

    auto backend = std::make_shared<FakeChatOpsBackend>();
    InventoryPoller poller(cfg, {});
    AdminRouter ro_router(cfg, poller, backend);

    const std::string sanction_body =
        R"({"subject":"alice","kind":"mute","duration_sec":300,"reason":"spam","idempotency_key":"k1"})";

    // ADMIN_READ_ONLY=true blocks even operator/admin on mutating routes
    const auto ro_sanction = ro_router.handle(AdminHttpRequest{
        "POST", "/api/v1/chat/sanctions", {{"authorization", "Bearer tok_operator"}}, sanction_body, "127.0.0.1"});
    EXPECT_EQ(ro_sanction.status, "403 Forbidden");
    EXPECT_EQ(json::parse(ro_sanction.body).at("error"), "read_only_mode");

    const auto ro_close = ro_router.handle(AdminHttpRequest{
        "POST", "/api/v1/chat/rooms/general/close", {{"authorization", "Bearer tok_admin"}},
        R"({"reason":"cleanup","idempotency_key":"k2"})", "127.0.0.1"});
    EXPECT_EQ(ro_close.status, "403 Forbidden");
    EXPECT_EQ(json::parse(ro_close.body).at("error"), "read_only_mode");

    // ADMIN_READ_ONLY=false allows viewer GETs, blocks viewer POST/DELETE with insufficient_role
    cfg.read_only = false;
    AdminRouter rw_router(cfg, poller, backend);

    EXPECT_EQ(rw_router.handle(AdminHttpRequest{
        "GET", "/api/v1/chat/rooms", {{"authorization", "Bearer tok_viewer"}}, "", "127.0.0.1"}).status, "200 OK");
    EXPECT_EQ(rw_router.handle(AdminHttpRequest{
        "GET", "/api/v1/chat/users", {{"authorization", "Bearer tok_viewer"}}, "", "127.0.0.1"}).status, "200 OK");
    EXPECT_EQ(rw_router.handle(AdminHttpRequest{
        "GET", "/api/v1/chat/sanctions", {{"authorization", "Bearer tok_viewer"}}, "", "127.0.0.1"}).status, "200 OK");
    EXPECT_EQ(rw_router.handle(AdminHttpRequest{
        "GET", "/api/v1/wb/dlq", {{"authorization", "Bearer tok_viewer"}}, "", "127.0.0.1"}).status, "200 OK");
    EXPECT_EQ(rw_router.handle(AdminHttpRequest{
        "GET", "/api/v1/audit-logs", {{"authorization", "Bearer tok_viewer"}}, "", "127.0.0.1"}).status, "200 OK");

    const auto viewer_post = rw_router.handle(AdminHttpRequest{
        "POST", "/api/v1/wb/dlq/replay", {{"authorization", "Bearer tok_viewer"}},
        R"({"max_items":10,"reason":"try","idempotency_key":"k3"})", "127.0.0.1"});
    EXPECT_EQ(viewer_post.status, "403 Forbidden");
    EXPECT_EQ(json::parse(viewer_post.body).at("error"), "insufficient_role");

    const auto viewer_del = rw_router.handle(AdminHttpRequest{
        "DELETE", "/api/v1/chat/sanctions/00000000-0000-4000-8000-000000000001?reason=try",
        {{"authorization", "Bearer tok_viewer"}, {"idempotency-key", "k4"}}, "", "127.0.0.1"});
    EXPECT_EQ(viewer_del.status, "403 Forbidden");
    EXPECT_EQ(json::parse(viewer_del.body).at("error"), "insufficient_role");

    const auto denied_logs = rw_router.handle(AdminHttpRequest{
        "GET", "/api/v1/audit-logs?status=denied", {{"authorization", "Bearer tok_viewer"}}, "", "127.0.0.1"});
    ASSERT_EQ(denied_logs.status, "200 OK");
    const auto logs_json = json::parse(denied_logs.body);
    EXPECT_EQ(logs_json.at("audit_logs").size(), 4u);
}

TEST(AdminConsoleV2Test, SanctionsRoomsKickIdempotencyReplayAndConflict) {
    AdminConfig cfg;
    cfg.auth_mode = AdminAuthMode::TOKEN;
    cfg.read_only = false;
    cfg.operator_token = "tok_operator";
    cfg.web_root = ADMIN_WEB_ASSET_DIR;

    auto backend = std::make_shared<FakeChatOpsBackend>();
    InventoryPoller poller(cfg, {});
    AdminRouter router(cfg, poller, backend);

    const std::string mute_payload =
        R"({"subject":"alice","kind":"mute","duration_sec":300,"reason":"flooding","idempotency_key":"idem-mute-1"})";

    const auto created = router.handle(AdminHttpRequest{
        "POST", "/api/v1/chat/sanctions", {{"authorization", "Bearer tok_operator"}}, mute_payload, "127.0.0.1"});
    ASSERT_EQ(created.status, "201 Created");
    const auto created_json = json::parse(created.body);
    const std::string sanction_id = created_json.at("sanction_id").get<std::string>();
    EXPECT_FALSE(created_json.at("idempotent_replay").get<bool>());

    // Identical key + identical payload => idempotent_replay: true
    const auto replayed = router.handle(AdminHttpRequest{
        "POST", "/api/v1/chat/sanctions", {{"authorization", "Bearer tok_operator"}}, mute_payload, "127.0.0.1"});
    ASSERT_EQ(replayed.status, "201 Created");
    const auto replayed_json = json::parse(replayed.body);
    EXPECT_EQ(replayed_json.at("sanction_id"), sanction_id);
    EXPECT_TRUE(replayed_json.at("idempotent_replay").get<bool>());

    // Same key + mismatched payload => 409 idempotency_conflict
    const std::string conflict_payload =
        R"({"subject":"bob","kind":"ban","duration_sec":600,"reason":"flooding","idempotency_key":"idem-mute-1"})";
    const auto conflicted = router.handle(AdminHttpRequest{
        "POST", "/api/v1/chat/sanctions", {{"authorization", "Bearer tok_operator"}}, conflict_payload, "127.0.0.1"});
    EXPECT_EQ(conflicted.status, "409 Conflict");
    EXPECT_EQ(json::parse(conflicted.body).at("error"), "idempotency_conflict");

    // Revoke via DELETE
    const auto revoked = router.handle(AdminHttpRequest{
        "DELETE", "/api/v1/chat/sanctions/" + sanction_id + "?reason=false+positive",
        {{"authorization", "Bearer tok_operator"}, {"idempotency-key", "idem-revoke-1"}}, "", "127.0.0.1"});
    EXPECT_EQ(revoked.status, "200 OK");
    EXPECT_EQ(json::parse(revoked.body).at("status"), "revoked");

    // Protect lobby, close general, kick alice
    const auto close_lobby = router.handle(AdminHttpRequest{
        "POST", "/api/v1/chat/rooms/lobby/close", {{"authorization", "Bearer tok_operator"}},
        R"({"reason":"test","idempotency_key":"idem-lobby"})", "127.0.0.1"});
    EXPECT_EQ(close_lobby.status, "400 Bad Request");
    EXPECT_EQ(json::parse(close_lobby.body).at("error"), "protected_room");

    const auto close_general = router.handle(AdminHttpRequest{
        "POST", "/api/v1/chat/rooms/general/close", {{"authorization", "Bearer tok_operator"}},
        R"({"reason":"cleanup room","idempotency_key":"idem-close-gen"})", "127.0.0.1"});
    EXPECT_EQ(close_general.status, "200 OK");
    EXPECT_EQ(json::parse(close_general.body).at("status"), "closed");

    const auto kick_alice = router.handle(AdminHttpRequest{
        "POST", "/api/v1/chat/users/alice/kick", {{"authorization", "Bearer tok_operator"}},
        R"({"reason":"rebalance","idempotency_key":"idem-kick-alice"})", "127.0.0.1"});
    EXPECT_EQ(kick_alice.status, "200 OK");
    EXPECT_EQ(json::parse(kick_alice.body).at("status"), "kicked");
}

TEST(AdminConsoleV2Test, TruthSourceComparisonPrivacyAndSecretRedaction) {
    AdminConfig cfg;
    cfg.auth_mode = AdminAuthMode::DEVELOPMENT;
    cfg.dev_default_role = AdminRole::VIEWER;
    cfg.web_root = ADMIN_WEB_ASSET_DIR;

    auto backend = std::make_shared<FakeChatOpsBackend>();
    InventoryPoller poller(cfg, {});
    AdminRouter router(cfg, poller, backend);

    const auto user_resp = router.handle(AdminHttpRequest{"GET", "/api/v1/chat/users/alice", {}, "", "127.0.0.1"});
    ASSERT_EQ(user_resp.status, "200 OK");
    const auto ujson = json::parse(user_resp.body);
    EXPECT_EQ(ujson.at("state_alignment"), "aligned");
    EXPECT_TRUE(ujson.at("postgres_truth").at("lease_active").get<bool>());
    EXPECT_TRUE(ujson.at("redis_state").at("presence_exists").get<bool>());
    EXPECT_FALSE(ujson.at("postgres_truth").contains("token"));
    EXPECT_FALSE(ujson.at("postgres_truth").contains("token_hash"));

    const auto room_resp = router.handle(AdminHttpRequest{"GET", "/api/v1/chat/rooms/general", {}, "", "127.0.0.1"});
    ASSERT_EQ(room_resp.status, "200 OK");
    const auto rjson = json::parse(room_resp.body);
    EXPECT_EQ(rjson.at("room").at("watermark"), 42);
    EXPECT_FALSE(rjson.at("room").contains("messages"));
    EXPECT_FALSE(rjson.at("room").contains("password"));

    const auto js_resp = router.handle(AdminHttpRequest{"GET", "/js/views_v2.js", {}, "", "127.0.0.1"});
    EXPECT_EQ(js_resp.status, "200 OK");
}

TEST(AdminConsoleV2Test, RespParserHandlesPttlXlenAndXrangePreview) {
    const std::string pttl_payload = "+OK\r\n:42100\r\n";
    ScopedRespPipelineMock pttl_srv(pttl_payload);

    const long long pttl = query_redis_pttl_ms(
        "tcp://:secret_pw_token@127.0.0.1:" + std::to_string(pttl_srv.port()),
        "127.0.0.1",
        pttl_srv.port(),
        "dynaxis:presence:user:123",
        1000);
    EXPECT_EQ(pttl, 42100);

    const std::string xrange_entry_fields =
        "*6\r\n" +
        resp_bulk("orig_event_id") + resp_bulk("evt-99") +
        resp_bulk("type") + resp_bulk("session_login") +
        resp_bulk("error") + resp_bulk("postgresql://user:secret_pw_token@db:5432/app");
    const std::string xrange_entry = "*2\r\n" + resp_bulk("1700000000000-0") + xrange_entry_fields;
    const std::string streams_payload =
        "+OK\r\n"
        ":12\r\n"
        ":1\r\n"
        ":0\r\n"
        "*1\r\n" +
        xrange_entry;

    ScopedRespPipelineMock streams_srv(streams_payload);
    const WbStreamsSnapshot snap = inspect_wb_streams_resp(
        "tcp://:secret_pw_token@127.0.0.1:" + std::to_string(streams_srv.port()),
        "127.0.0.1",
        streams_srv.port(),
        "session_events",
        "session_events_dlq",
        "session_events_dead",
        10,
        1000);

    ASSERT_TRUE(snap.ok);
    EXPECT_EQ(snap.pending_stream_length, 12);
    EXPECT_EQ(snap.dlq_length, 1);
    EXPECT_EQ(snap.dead_length, 0);
    ASSERT_EQ(snap.preview_entries.size(), 1u);
    EXPECT_EQ(snap.preview_entries[0].stream_id, "1700000000000-0");
    EXPECT_EQ(snap.preview_entries[0].orig_event_id, "evt-99");
    EXPECT_EQ(snap.preview_entries[0].type, "session_login");
    EXPECT_EQ(snap.preview_entries[0].error.find("secret_pw_token"), std::string::npos);

    // Closed port must not expose password in snap.error
    const WbStreamsSnapshot fail_snap = inspect_wb_streams_resp(
        "tcp://:secret_pw_token@127.0.0.1:1",
        "127.0.0.1",
        1,
        "session_events",
        "session_events_dlq",
        "session_events_dead",
        5,
        200);
    EXPECT_FALSE(fail_snap.ok);
    EXPECT_EQ(fail_snap.error.find("secret_pw_token"), std::string::npos);
}
