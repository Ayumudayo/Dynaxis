#include "admin/chat_ops_backend.hpp"

#include "admin/postgres_probe.hpp"
#include "admin/resp_probe.hpp"
#include "ops_pg_helpers.hpp"
#include "server/storage/redis/factory.hpp"

#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <set>
#include <sstream>
#include <unordered_map>

namespace dynaxis::admin {
namespace {

using json = nlohmann::json;
using detail::check_idempotency;
using detail::db_now_ms;
using detail::insert_audit_log;
using detail::is_valid_idempotency_key;
using detail::is_valid_token_identifier;
using detail::is_valid_uuid;
using detail::lock_idempotency;
using detail::make_error_result;
using detail::make_json_result;
using detail::save_idempotency;
using detail::sha256_hex;
using detail::to_lower_ascii;
using detail::trim_ascii;

std::string effective_redis_uri(const AdminConfig& cfg) {
    if (!cfg.redis_uri.empty()) {
        return cfg.redis_uri;
    }
    const unsigned short port = (cfg.redis_host_port > 0) ? cfg.redis_host_port : 6379;
    return "tcp://127.0.0.1:" + std::to_string(port);
}

std::shared_ptr<server::core::storage::redis::IRedisClient> connect_redis(const AdminConfig& cfg) {
    try {
        server::storage::redis::Options opts{};
        opts.pool_max = 4;
        opts.use_streams = true;
        auto client = server::storage::redis::make_redis_client(effective_redis_uri(cfg), opts);
        if (client && client->health_check()) {
            return client;
        }
    } catch (...) {
    }
    return nullptr;
}

void lock_chat_state(pqxx::work& tx, const std::string& key) {
    tx.exec_params(
        "select pg_advisory_xact_lock(hashtextextended(case when $1 like 'chat:room:%' then lower($1) else $1 end, 0))",
        "chat:" + key);
}

std::optional<json> read_chat_state(pqxx::work& tx, const std::string& key) {
    const auto rows = tx.exec_params(
        "select value::text from chat_shared_state "
        "where key = case when $1 like 'room:%' then lower($1) else $1 end",
        key);
    if (rows.empty()) {
        return std::nullopt;
    }
    return json::parse(rows[0][0].as<std::string>());
}

void write_chat_state(pqxx::work& tx, const std::string& key, const json& value) {
    tx.exec_params(
        "insert into chat_shared_state(key, value) "
        "values(case when $1 like 'room:%' then lower($1) else $1 end, $2::jsonb) "
        "on conflict (key) do update set value = excluded.value",
        key,
        value.dump());
}

void erase_chat_state(pqxx::work& tx, const std::string& key) {
    tx.exec_params(
        "delete from chat_shared_state "
        "where key = case when $1 like 'room:%' then lower($1) else $1 end",
        key);
}

void publish_control_change(const AdminConfig& cfg) {
    if (auto redis = connect_redis(cfg)) {
        (void)redis->publish(cfg.redis_channel_prefix + "fanout:control", "gw=admin\nchanged");
    }
}

std::unordered_map<std::string, std::vector<std::string>> collect_redis_room_members(
    const std::shared_ptr<server::core::storage::redis::IRedisClient>& redis) {
    std::unordered_map<std::string, std::vector<std::string>> out;
    if (!redis) {
        return out;
    }
    std::set<std::string> room_names{"lobby"};
    std::vector<std::string> active_rooms;
    if (redis->smembers("rooms:active", active_rooms)) {
        for (const auto& name : active_rooms) {
            if (!name.empty()) {
                room_names.insert(name);
            }
        }
    }
    for (const auto& room_name : room_names) {
        std::vector<std::string> members;
        if (redis->smembers("room:users:" + room_name, members) && !members.empty()) {
            std::sort(members.begin(), members.end());
            members.erase(std::unique(members.begin(), members.end()), members.end());
            out[to_lower_ascii(room_name)] = std::move(members);
        }
    }
    return out;
}

} // namespace

PostgresRedisChatOpsBackend::PostgresRedisChatOpsBackend(AdminConfig config)
    : config_(std::move(config)) {}

ChatOpHttpResult PostgresRedisChatOpsBackend::list_rooms(std::string_view query,
                                                         bool include_closed,
                                                         std::size_t limit) {
    if (config_.db_uri.empty()) {
        return make_error_result(503, "db_unconfigured", "DB_URI is not configured.");
    }

    try {
        const std::string needle = trim_ascii(query);
        const std::size_t clamped_limit = std::clamp<std::size_t>(limit, 1, 200);

        pqxx::connection conn(config_.db_uri);
        pqxx::work tx(conn);
        tx.exec("SET LOCAL statement_timeout = 2500");

        const auto rows = tx.exec_params(
            "select r.id::text, r.name, r.is_public, r.is_active, "
            "coalesce((extract(epoch from r.created_at) * 1000)::bigint, 0), "
            "coalesce((extract(epoch from r.closed_at) * 1000)::bigint, 0), "
            "coalesce((select max(m.id) from messages m where m.room_id = r.id), 0) as watermark, "
            "(select count(*) from memberships mb where mb.room_id = r.id and mb.is_member = true) as db_member_count, "
            "coalesce(css.value::text, '') as shared_state "
            "from rooms r "
            "left join chat_shared_state css on css.key = ('room:' || lower(r.name)) "
            "where ($1 = '' or lower(r.name) like ('%' || lower($1) || '%')) "
            "and ($2 or r.is_active = true) "
            "order by r.is_active desc, r.created_at desc "
            "limit $3",
            needle,
            include_closed,
            static_cast<int>(clamped_limit));

        auto redis = connect_redis(config_);
        const auto redis_members_by_room = collect_redis_room_members(redis);

        json items = json::array();
        for (const auto& row : rows) {
            const std::string name = row[1].c_str();
            const std::string lower_name = to_lower_ascii(name);
            const bool is_public = row[2].as<bool>();
            const bool is_active = row[3].as<bool>();
            const std::string shared_raw = row[8].c_str();

            std::string owner;
            bool is_locked = !is_public;
            std::size_t shared_member_count = 0;
            bool shared_active = is_active;
            if (!shared_raw.empty()) {
                try {
                    const auto authority = json::parse(shared_raw);
                    owner = authority.value("owner", "");
                    is_locked = !authority.value("password", "").empty();
                    shared_active = authority.value("active", is_active);
                    if (authority.contains("members") && authority["members"].is_object()) {
                        for (const auto& [member_name, flag] : authority["members"].items()) {
                            (void)member_name;
                            if (flag.is_boolean() && flag.get<bool>()) {
                                ++shared_member_count;
                            }
                        }
                    }
                } catch (...) {
                }
            }

            std::size_t online_count = 0;
            if (const auto it = redis_members_by_room.find(lower_name); it != redis_members_by_room.end()) {
                online_count = it->second.size();
            }

            items.push_back(json{
                {"room_id", row[0].c_str()},
                {"name", name},
                {"is_public", is_public},
                {"is_active", is_active && shared_active},
                {"is_locked", is_locked},
                {"owner", owner},
                {"watermark", row[6].as<std::int64_t>()},
                {"db_member_count", row[7].as<std::int64_t>()},
                {"shared_member_count", shared_member_count},
                {"online_count", online_count},
                {"created_at_unix_ms", row[4].as<std::int64_t>()},
                {"closed_at_unix_ms", row[5].as<std::int64_t>()},
            });
        }
        tx.commit();

        return make_json_result(200, json{
            {"rooms", std::move(items)},
            {"redis_connected", static_cast<bool>(redis)},
            {"privacy_notice", "Chat message plaintext content is intentionally omitted."},
        });
    } catch (const std::exception& ex) {
        return make_error_result(500, "db_query_failed", sanitize_connection_error(ex.what()));
    }
}

ChatOpHttpResult PostgresRedisChatOpsBackend::get_room_detail(std::string_view room_name) {
    if (config_.db_uri.empty()) {
        return make_error_result(503, "db_unconfigured", "DB_URI is not configured.");
    }
    const std::string target = trim_ascii(room_name);
    if (!is_valid_token_identifier(target, 128)) {
        return make_error_result(400, "invalid_room", "Invalid room identifier.");
    }

    try {
        pqxx::connection conn(config_.db_uri);
        pqxx::work tx(conn);
        tx.exec("SET LOCAL statement_timeout = 2500");

        const auto rows = tx.exec_params(
            "select r.id::text, r.name, r.is_public, r.is_active, "
            "coalesce((extract(epoch from r.created_at) * 1000)::bigint, 0), "
            "coalesce((extract(epoch from r.closed_at) * 1000)::bigint, 0), "
            "coalesce((select max(m.id) from messages m where m.room_id = r.id), 0) as watermark, "
            "coalesce(css.value::text, '') as shared_state "
            "from rooms r "
            "left join chat_shared_state css on css.key = ('room:' || lower(r.name)) "
            "where lower(r.name) = lower($1) "
            "order by r.is_active desc, r.created_at desc "
            "limit 1",
            target);

        if (rows.empty()) {
            return make_error_result(404, "room_not_found", "Room was not found.");
        }

        const std::string room_id = rows[0][0].c_str();
        const std::string canonical_name = rows[0][1].c_str();
        const bool is_public = rows[0][2].as<bool>();
        const bool is_active = rows[0][3].as<bool>();
        const std::string shared_raw = rows[0][7].c_str();

        std::string owner;
        bool is_locked = !is_public;
        bool shared_active = is_active;
        json invites = json::array();
        json shared_members = json::array();
        if (!shared_raw.empty()) {
            try {
                const auto authority = json::parse(shared_raw);
                owner = authority.value("owner", "");
                is_locked = !authority.value("password", "").empty();
                shared_active = authority.value("active", is_active);
                if (authority.contains("invites") && authority["invites"].is_array()) {
                    invites = authority["invites"];
                }
                if (authority.contains("members") && authority["members"].is_object()) {
                    for (const auto& [member_name, flag] : authority["members"].items()) {
                        if (flag.is_boolean() && flag.get<bool>()) {
                            shared_members.push_back(member_name);
                        }
                    }
                }
            } catch (...) {
            }
        }

        const auto member_rows = tx.exec_params(
            "select u.name, mb.role, coalesce(mb.last_seen_msg_id, 0), "
            "coalesce((extract(epoch from mb.joined_at) * 1000)::bigint, 0) "
            "from memberships mb "
            "join users u on u.id = mb.user_id "
            "where mb.room_id = $1::uuid and mb.is_member = true "
            "order by mb.joined_at asc limit 100",
            room_id);

        json db_members = json::array();
        for (const auto& mrow : member_rows) {
            db_members.push_back(json{
                {"subject", mrow[0].c_str()},
                {"role", mrow[1].c_str()},
                {"last_seen_msg_id", mrow[2].as<std::int64_t>()},
                {"joined_at_unix_ms", mrow[3].as<std::int64_t>()},
            });
        }
        tx.commit();

        json online_members = json::array();
        if (auto redis = connect_redis(config_)) {
            std::vector<std::string> members;
            if (redis->smembers("room:users:" + canonical_name, members)) {
                std::sort(members.begin(), members.end());
                for (const auto& m : members) {
                    online_members.push_back(m);
                }
            }
        }

        return make_json_result(200, json{
            {"room",
             {
                 {"room_id", room_id},
                 {"name", canonical_name},
                 {"is_public", is_public},
                 {"is_active", is_active && shared_active},
                 {"is_locked", is_locked},
                 {"owner", owner},
                 {"invites", invites},
                 {"shared_members", shared_members},
                 {"db_members", db_members},
                 {"online_members", online_members},
                 {"watermark", rows[0][6].as<std::int64_t>()},
                 {"created_at_unix_ms", rows[0][4].as<std::int64_t>()},
                 {"closed_at_unix_ms", rows[0][5].as<std::int64_t>()},
             }},
            {"privacy_notice", "Chat message plaintext content is intentionally excluded from admin console views."},
        });
    } catch (const std::exception& ex) {
        return make_error_result(500, "db_query_failed", sanitize_connection_error(ex.what()));
    }
}

ChatOpHttpResult PostgresRedisChatOpsBackend::close_room(std::string_view room_name,
                                                         std::string_view reason,
                                                         std::string_view idempotency_key,
                                                         AdminRole actor_role) {
    if (config_.db_uri.empty()) {
        return make_error_result(503, "db_unconfigured", "DB_URI is not configured.");
    }
    const std::string target = trim_ascii(room_name);
    const std::string trimmed_reason = trim_ascii(reason);
    const std::string trimmed_idem = trim_ascii(idempotency_key);
    if (!is_valid_token_identifier(target, 128)) {
        return make_error_result(400, "invalid_room", "Invalid room identifier.");
    }
    if (to_lower_ascii(target) == "lobby") {
        return make_error_result(400, "protected_room", "The default lobby room cannot be forcibly closed.");
    }
    if (trimmed_reason.empty() || trimmed_reason.size() > 500) {
        return make_error_result(400, "reason_required", "A non-empty audit reason (<= 500 chars) is required.");
    }
    if (!is_valid_idempotency_key(trimmed_idem)) {
        return make_error_result(400, "idempotency_key_required", "A valid ASCII idempotency_key is required.");
    }

    const std::string endpoint = "POST /api/v1/chat/rooms/" + to_lower_ascii(target) + "/close";
    const std::string req_hash = sha256_hex(endpoint + "\n" + trimmed_reason);

    try {
        pqxx::connection conn(config_.db_uri);
        pqxx::work tx(conn);
        tx.exec("SET LOCAL statement_timeout = 2500");

        lock_idempotency(tx, trimmed_idem);
        if (auto replay = check_idempotency(
                tx, trimmed_idem, endpoint, req_hash, actor_role, "chat.moderation", "close_room", target, trimmed_reason)) {
            return *replay;
        }

        const std::string state_key = "room:" + target;
        lock_chat_state(tx, state_key);
        auto authority_opt = read_chat_state(tx, state_key);
        const auto room_rows = tx.exec_params(
            "select id::text, name, is_active from rooms where lower(name) = lower($1) "
            "order by is_active desc, created_at desc limit 1",
            target);

        if (room_rows.empty() && !authority_opt.has_value()) {
            insert_audit_log(
                tx, actor_role, "chat.moderation", "close_room", target, trimmed_reason, trimmed_idem, "failed",
                json{{"error", "room_not_found"}});
            tx.commit();
            return make_error_result(404, "room_not_found", "Room was not found.");
        }

        std::string canonical_name = room_rows.empty() ? target : std::string(room_rows[0][1].c_str());
        json authority = authority_opt.value_or(json::object());
        if (!authority.contains("owner")) {
            authority["owner"] = "";
        }
        if (!authority.contains("password")) {
            authority["password"] = "";
        }
        if (!authority.contains("invites")) {
            authority["invites"] = json::array();
        }
        authority["active"] = false;
        write_chat_state(tx, state_key, authority);

        tx.exec_params(
            "update rooms set is_active = false, closed_at = coalesce(closed_at, now()) where lower(name) = lower($1)",
            canonical_name);

        const json resp_json{
            {"status", "closed"},
            {"room", canonical_name},
            {"idempotency_key", trimmed_idem},
            {"idempotent_replay", false},
        };

        insert_audit_log(
            tx, actor_role, "chat.moderation", "close_room", canonical_name, trimmed_reason, trimmed_idem, "succeeded",
            json{{"room", canonical_name}});
        save_idempotency(tx, trimmed_idem, endpoint, req_hash, 200, resp_json);
        tx.commit();

        if (auto redis = connect_redis(config_)) {
            (void)redis->srem("rooms:active", canonical_name);
            (void)redis->srem("rooms:active", to_lower_ascii(canonical_name));
            (void)redis->publish(config_.redis_channel_prefix + "fanout:control", "gw=admin\nchanged");
        }
        return make_json_result(200, resp_json);
    } catch (const std::exception& ex) {
        return make_error_result(500, "close_room_failed", sanitize_connection_error(ex.what()));
    }
}

ChatOpHttpResult PostgresRedisChatOpsBackend::list_users(std::string_view query, std::size_t limit) {
    if (config_.db_uri.empty()) {
        return make_error_result(503, "db_unconfigured", "DB_URI is not configured.");
    }

    try {
        const std::string needle = trim_ascii(query);
        const std::size_t clamped_limit = std::clamp<std::size_t>(limit, 1, 200);

        pqxx::connection conn(config_.db_uri);
        pqxx::work tx(conn);
        tx.exec("SET LOCAL statement_timeout = 2500");

        const std::int64_t now_ms = db_now_ms(tx);
        const auto rows = tx.exec_params(
            "select u.id::text, u.name, "
            "coalesce((extract(epoch from u.last_login_at) * 1000)::bigint, 0), "
            "coalesce(css_login.value::text, ''), "
            "coalesce(css_pen.value::text, '') "
            "from users u "
            "left join chat_shared_state css_login on css_login.key = ('login:' || u.name) "
            "left join chat_shared_state css_pen on css_pen.key = ('penalty:' || u.name) "
            "where ($1 = '' or lower(u.name) like ('%' || lower($1) || '%')) "
            "order by (css_login.value is not null) desc, u.last_login_at desc nulls last, u.created_at desc "
            "limit $2",
            needle,
            static_cast<int>(clamped_limit));

        auto redis = connect_redis(config_);
        const std::string ruri = effective_redis_uri(config_);

        json items = json::array();
        for (const auto& row : rows) {
            const std::string user_id = row[0].c_str();
            const std::string subject = row[1].c_str();
            const std::string login_raw = row[3].c_str();
            const std::string pen_raw = row[4].c_str();

            bool lease_active = false;
            std::int64_t lease_ttl_ms = 0;
            std::string logical_session_id;
            if (!login_raw.empty()) {
                try {
                    const auto lj = json::parse(login_raw);
                    const std::int64_t exp = lj.value("expires", 0LL);
                    logical_session_id = lj.value("logical", "");
                    if (exp > now_ms) {
                        lease_active = true;
                        lease_ttl_ms = exp - now_ms;
                    }
                } catch (...) {
                }
            }

            bool muted = false;
            bool banned = false;
            if (!pen_raw.empty()) {
                try {
                    const auto pj = json::parse(pen_raw);
                    muted = pj.value("mute", 0LL) > now_ms;
                    banned = pj.value("ban", 0LL) > now_ms;
                } catch (...) {
                }
            }

            long long presence_ttl_ms = -2;
            bool presence_exists = false;
            if (redis) {
                presence_ttl_ms = query_redis_pttl_ms(
                    ruri,
                    "127.0.0.1",
                    config_.redis_host_port,
                    config_.redis_channel_prefix + "presence:user:" + user_id,
                    300);
                presence_exists = (presence_ttl_ms >= 0 || presence_ttl_ms == -1);
            }

            items.push_back(json{
                {"user_id", user_id},
                {"subject", subject},
                {"last_login_at_unix_ms", row[2].as<std::int64_t>()},
                {"lease_active", lease_active},
                {"lease_ttl_ms", lease_ttl_ms},
                {"logical_session_id", logical_session_id},
                {"presence_exists", presence_exists},
                {"presence_ttl_ms", presence_ttl_ms >= 0 ? presence_ttl_ms : -1},
                {"muted", muted},
                {"banned", banned},
            });
        }
        tx.commit();

        return make_json_result(200, json{
            {"users", std::move(items)},
            {"redis_connected", static_cast<bool>(redis)},
            {"lookup_hint", "Search targets canonical lowercase ASCII subject (principal_id), not display_name."},
        });
    } catch (const std::exception& ex) {
        return make_error_result(500, "db_query_failed", sanitize_connection_error(ex.what()));
    }
}

ChatOpHttpResult PostgresRedisChatOpsBackend::get_user_detail(std::string_view subject) {
    if (config_.db_uri.empty()) {
        return make_error_result(503, "db_unconfigured", "DB_URI is not configured.");
    }
    const std::string target = trim_ascii(subject);
    if (!is_valid_token_identifier(target, 64)) {
        return make_error_result(400, "invalid_subject", "Invalid subject identifier.");
    }

    try {
        pqxx::connection conn(config_.db_uri);
        pqxx::work tx(conn);
        tx.exec("SET LOCAL statement_timeout = 2500");

        const std::int64_t now_ms = db_now_ms(tx);
        const auto user_rows = tx.exec_params(
            "select id::text, name, "
            "coalesce((extract(epoch from created_at) * 1000)::bigint, 0), "
            "coalesce((extract(epoch from last_login_at) * 1000)::bigint, 0) "
            "from users "
            "where lower(name) = lower($1) "
            "order by (name = $1) desc, created_at desc "
            "limit 1",
            target);

        std::string user_id;
        std::string canonical_subject = target;
        std::int64_t created_at_ms = 0;
        std::int64_t last_login_ms = 0;
        if (!user_rows.empty()) {
            user_id = user_rows[0][0].c_str();
            canonical_subject = user_rows[0][1].c_str();
            created_at_ms = user_rows[0][2].as<std::int64_t>();
            last_login_ms = user_rows[0][3].as<std::int64_t>();
        }

        const auto login_state = read_chat_state(tx, "login:" + canonical_subject);
        const auto penalty_state = read_chat_state(tx, "penalty:" + canonical_subject);

        bool lease_active = false;
        std::int64_t lease_expires_ms = 0;
        std::int64_t lease_ttl_ms = 0;
        std::string logical_session_id;
        if (login_state.has_value()) {
            lease_expires_ms = login_state->value("expires", 0LL);
            logical_session_id = login_state->value("logical", "");
            if (lease_expires_ms > now_ms) {
                lease_active = true;
                lease_ttl_ms = lease_expires_ms - now_ms;
            }
        }

        std::int64_t mute_expires_ms = 0;
        std::int64_t ban_expires_ms = 0;
        if (penalty_state.has_value()) {
            mute_expires_ms = penalty_state->value("mute", 0LL);
            ban_expires_ms = penalty_state->value("ban", 0LL);
        }

        json sessions_json = json::array();
        if (!user_id.empty()) {
            const auto sess_rows = tx.exec_params(
                "select id::text, "
                "coalesce((extract(epoch from created_at) * 1000)::bigint, 0), "
                "coalesce((extract(epoch from expires_at) * 1000)::bigint, 0), "
                "coalesce((extract(epoch from revoked_at) * 1000)::bigint, 0), "
                "(revoked_at is null and expires_at > now()) as active "
                "from sessions where user_id = $1::uuid "
                "order by created_at desc limit 5",
                user_id);
            for (const auto& sr : sess_rows) {
                const std::int64_t exp_ms = sr[2].as<std::int64_t>();
                const bool active = sr[4].as<bool>();
                sessions_json.push_back(json{
                    {"logical_session_id", sr[0].c_str()},
                    {"created_at_unix_ms", sr[1].as<std::int64_t>()},
                    {"expires_at_unix_ms", exp_ms},
                    {"revoked_at_unix_ms", sr[3].as<std::int64_t>()},
                    {"ttl_ms", active && exp_ms > now_ms ? (exp_ms - now_ms) : 0},
                    {"active", active},
                });
            }
        }

        const auto sanc_rows = tx.exec_params(
            "select id::text, kind, reason, actor_role, "
            "(extract(epoch from created_at) * 1000)::bigint, "
            "(extract(epoch from expires_at) * 1000)::bigint, "
            "coalesce((extract(epoch from revoked_at) * 1000)::bigint, 0), "
            "(revoked_at is null and expires_at > now()) as active "
            "from admin_sanctions where lower(subject) = lower($1) "
            "order by created_at desc limit 10",
            canonical_subject);
        json sanctions_json = json::array();
        for (const auto& sr : sanc_rows) {
            sanctions_json.push_back(json{
                {"sanction_id", sr[0].c_str()},
                {"kind", sr[1].c_str()},
                {"reason", sr[2].c_str()},
                {"actor_role", sr[3].c_str()},
                {"created_at_unix_ms", sr[4].as<std::int64_t>()},
                {"expires_at_unix_ms", sr[5].as<std::int64_t>()},
                {"revoked_at_unix_ms", sr[6].as<std::int64_t>()},
                {"active", sr[7].as<bool>()},
            });
        }
        tx.commit();

        auto redis = connect_redis(config_);
        const std::string ruri = effective_redis_uri(config_);
        bool presence_exists = false;
        long long presence_pttl_ms = -2;
        std::string continuity_room;
        std::string gateway_backend;
        json redis_rooms = json::array();

        if (redis) {
            if (!user_id.empty()) {
                presence_pttl_ms = query_redis_pttl_ms(
                    ruri,
                    "127.0.0.1",
                    config_.redis_host_port,
                    config_.redis_channel_prefix + "presence:user:" + user_id,
                    400);
                presence_exists = (presence_pttl_ms >= 0 || presence_pttl_ms == -1);
            }
            if (!logical_session_id.empty()) {
                if (auto r = redis->get(config_.redis_channel_prefix + "continuity:room:" + logical_session_id)) {
                    continuity_room = *r;
                }
                if (auto gw = redis->get("gateway/session/" + logical_session_id)) {
                    gateway_backend = *gw;
                }
            }
            if (gateway_backend.empty()) {
                if (auto gw = redis->get("gateway/session/" + canonical_subject)) {
                    gateway_backend = *gw;
                }
            }

            const auto room_members = collect_redis_room_members(redis);
            for (const auto& [lower_room, members] : room_members) {
                for (const auto& member : members) {
                    if (to_lower_ascii(member) == to_lower_ascii(canonical_subject)) {
                        redis_rooms.push_back(lower_room);
                        break;
                    }
                }
            }
        }

        const bool found_anywhere = !user_rows.empty() || login_state.has_value() || penalty_state.has_value() ||
                                    presence_exists || !redis_rooms.empty();
        if (!found_anywhere) {
            return make_error_result(404, "user_not_found", "Subject was not found in Postgres or Redis.");
        }

        std::string alignment = "offline_aligned";
        if (!redis) {
            alignment = "redis_unavailable";
        } else if (lease_active && (presence_exists || !redis_rooms.empty())) {
            alignment = "aligned";
        } else if (!lease_active && (presence_exists || !redis_rooms.empty())) {
            alignment = "orphan_redis_presence";
        } else if (lease_active && !presence_exists && redis_rooms.empty()) {
            alignment = "missing_redis_presence";
        }

        return make_json_result(200, json{
            {"subject", canonical_subject},
            {"user_id", user_id},
            {"created_at_unix_ms", created_at_ms},
            {"last_login_at_unix_ms", last_login_ms},
            {"now_unix_ms", now_ms},
            {"state_alignment", alignment},
            {"postgres_truth",
             {
                 {"lease_present", login_state.has_value()},
                 {"lease_active", lease_active},
                 {"lease_expires_unix_ms", lease_expires_ms},
                 {"lease_ttl_ms", lease_ttl_ms},
                 {"logical_session_id", logical_session_id},
                 {"muted", mute_expires_ms > now_ms},
                 {"mute_remaining_ms", mute_expires_ms > now_ms ? (mute_expires_ms - now_ms) : 0},
                 {"banned", ban_expires_ms > now_ms},
                 {"ban_remaining_ms", ban_expires_ms > now_ms ? (ban_expires_ms - now_ms) : 0},
                 {"sessions", sessions_json},
             }},
            {"redis_state",
             {
                 {"connected", static_cast<bool>(redis)},
                 {"presence_exists", presence_exists},
                 {"presence_ttl_ms", presence_pttl_ms >= 0 ? presence_pttl_ms : -1},
                 {"continuity_room", continuity_room},
                 {"gateway_backend", gateway_backend},
                 {"rooms", redis_rooms},
             }},
            {"sanctions", sanctions_json},
            {"secret_policy", "access_token, resume_token, and lease token fields are strictly excluded."},
        });
    } catch (const std::exception& ex) {
        return make_error_result(500, "db_query_failed", sanitize_connection_error(ex.what()));
    }
}

ChatOpHttpResult PostgresRedisChatOpsBackend::kick_user(std::string_view subject,
                                                        std::string_view reason,
                                                        std::string_view idempotency_key,
                                                        AdminRole actor_role) {
    if (config_.db_uri.empty()) {
        return make_error_result(503, "db_unconfigured", "DB_URI is not configured.");
    }
    const std::string target = trim_ascii(subject);
    const std::string trimmed_reason = trim_ascii(reason);
    const std::string trimmed_idem = trim_ascii(idempotency_key);
    if (!is_valid_token_identifier(target, 64)) {
        return make_error_result(400, "invalid_subject", "Invalid subject identifier.");
    }
    if (trimmed_reason.empty() || trimmed_reason.size() > 500) {
        return make_error_result(400, "reason_required", "A non-empty audit reason (<= 500 chars) is required.");
    }
    if (!is_valid_idempotency_key(trimmed_idem)) {
        return make_error_result(400, "idempotency_key_required", "A valid ASCII idempotency_key is required.");
    }

    const std::string endpoint = "POST /api/v1/chat/users/" + to_lower_ascii(target) + "/kick";
    const std::string req_hash = sha256_hex(endpoint + "\n" + trimmed_reason);

    try {
        pqxx::connection conn(config_.db_uri);
        pqxx::work tx(conn);
        tx.exec("SET LOCAL statement_timeout = 2500");

        lock_idempotency(tx, trimmed_idem);
        if (auto replay = check_idempotency(
                tx, trimmed_idem, endpoint, req_hash, actor_role, "chat.moderation", "kick_user", target, trimmed_reason)) {
            return *replay;
        }

        const auto user_rows = tx.exec_params(
            "select id::text, name from users where lower(name) = lower($1) order by (name = $1) desc limit 1",
            target);
        std::string user_id = user_rows.empty() ? std::string() : std::string(user_rows[0][0].c_str());
        std::string canonical_subject = user_rows.empty() ? target : std::string(user_rows[0][1].c_str());

        lock_chat_state(tx, "login:" + canonical_subject);
        const bool had_lease = read_chat_state(tx, "login:" + canonical_subject).has_value();
        erase_chat_state(tx, "login:" + canonical_subject);
        if (canonical_subject != target) {
            lock_chat_state(tx, "login:" + target);
            erase_chat_state(tx, "login:" + target);
        }

        if (!user_id.empty()) {
            tx.exec_params(
                "update sessions set revoked_at = coalesce(revoked_at, now()) "
                "where user_id = $1::uuid and revoked_at is null",
                user_id);
        }

        const json resp_json{
            {"status", "kicked"},
            {"subject", canonical_subject},
            {"lease_cleared", had_lease},
            {"idempotency_key", trimmed_idem},
            {"idempotent_replay", false},
        };

        insert_audit_log(
            tx, actor_role, "chat.moderation", "kick_user", canonical_subject, trimmed_reason, trimmed_idem, "succeeded",
            json{{"lease_cleared", had_lease}});
        save_idempotency(tx, trimmed_idem, endpoint, req_hash, 200, resp_json);
        tx.commit();

        if (auto redis = connect_redis(config_)) {
            if (!user_id.empty()) {
                (void)redis->del(config_.redis_channel_prefix + "presence:user:" + user_id);
            }
            const auto room_members = collect_redis_room_members(redis);
            for (const auto& [room_key, members] : room_members) {
                (void)members;
                (void)redis->srem("room:users:" + room_key, canonical_subject);
                if (canonical_subject != target) {
                    (void)redis->srem("room:users:" + room_key, target);
                }
            }
            (void)redis->publish(config_.redis_channel_prefix + "fanout:control", "gw=admin\nchanged");
        }
        return make_json_result(200, resp_json);
    } catch (const std::exception& ex) {
        return make_error_result(500, "kick_failed", sanitize_connection_error(ex.what()));
    }
}

ChatOpHttpResult PostgresRedisChatOpsBackend::list_sanctions(bool active_only,
                                                             std::string_view subject,
                                                             std::size_t limit) {
    if (config_.db_uri.empty()) {
        return make_error_result(503, "db_unconfigured", "DB_URI is not configured.");
    }

    try {
        const std::string needle = trim_ascii(subject);
        const std::size_t clamped_limit = std::clamp<std::size_t>(limit, 1, 200);

        pqxx::connection conn(config_.db_uri);
        pqxx::work tx(conn);
        tx.exec("SET LOCAL statement_timeout = 2500");

        const auto rows = tx.exec_params(
            "select id::text, subject, kind, reason, actor_role, "
            "(extract(epoch from created_at) * 1000)::bigint, "
            "(extract(epoch from expires_at) * 1000)::bigint, "
            "coalesce((extract(epoch from revoked_at) * 1000)::bigint, 0), "
            "coalesce(revoked_role, ''), coalesce(revoked_reason, ''), "
            "(revoked_at is null and expires_at > now()) as active "
            "from admin_sanctions "
            "where ($1 = '' or lower(subject) like ('%' || lower($1) || '%')) "
            "and (not $2 or (revoked_at is null and expires_at > now())) "
            "order by created_at desc "
            "limit $3",
            needle,
            active_only,
            static_cast<int>(clamped_limit));

        json items = json::array();
        for (const auto& row : rows) {
            items.push_back(json{
                {"sanction_id", row[0].c_str()},
                {"subject", row[1].c_str()},
                {"kind", row[2].c_str()},
                {"reason", row[3].c_str()},
                {"actor_role", row[4].c_str()},
                {"created_at_unix_ms", row[5].as<std::int64_t>()},
                {"expires_at_unix_ms", row[6].as<std::int64_t>()},
                {"revoked_at_unix_ms", row[7].as<std::int64_t>()},
                {"revoked_role", row[8].c_str()},
                {"revoked_reason", row[9].c_str()},
                {"active", row[10].as<bool>()},
            });
        }
        tx.commit();

        return make_json_result(200, json{{"sanctions", std::move(items)}});
    } catch (const std::exception& ex) {
        return make_error_result(500, "db_query_failed", sanitize_connection_error(ex.what()));
    }
}

ChatOpHttpResult PostgresRedisChatOpsBackend::create_sanction(std::string_view subject,
                                                              std::string_view kind,
                                                              std::uint64_t duration_sec,
                                                              bool confirm_permanent,
                                                              std::string_view reason,
                                                              std::string_view idempotency_key,
                                                              AdminRole actor_role) {
    if (config_.db_uri.empty()) {
        return make_error_result(503, "db_unconfigured", "DB_URI is not configured.");
    }

    const std::string target = trim_ascii(subject);
    const std::string kind_lower = to_lower_ascii(trim_ascii(kind));
    const std::string trimmed_reason = trim_ascii(reason);
    const std::string trimmed_idem = trim_ascii(idempotency_key);

    if (!is_valid_token_identifier(target, 64)) {
        return make_error_result(400, "invalid_subject", "Invalid subject identifier.");
    }
    if (kind_lower != "mute" && kind_lower != "ban") {
        return make_error_result(400, "invalid_kind", "Sanction kind must be 'mute' or 'ban'.");
    }
    if (duration_sec < 5 || duration_sec > 31536000ULL) {
        return make_error_result(400, "invalid_duration", "Duration must be between 5 seconds and 365 days.");
    }
    if (duration_sec > 86400ULL && !confirm_permanent) {
        return make_error_result(
            400,
            "confirmation_required",
            "Long sanctions (>24h) require confirm_permanent=true.");
    }
    if (trimmed_reason.empty() || trimmed_reason.size() > 500) {
        return make_error_result(400, "reason_required", "A non-empty audit reason (<= 500 chars) is required.");
    }
    if (!is_valid_idempotency_key(trimmed_idem)) {
        return make_error_result(400, "idempotency_key_required", "A valid ASCII idempotency_key is required.");
    }

    const std::string endpoint = "POST /api/v1/chat/sanctions";
    const std::string req_hash = sha256_hex(
        to_lower_ascii(target) + "\n" + kind_lower + "\n" + std::to_string(duration_sec) + "\n" + trimmed_reason);

    try {
        pqxx::connection conn(config_.db_uri);
        pqxx::work tx(conn);
        tx.exec("SET LOCAL statement_timeout = 2500");

        lock_idempotency(tx, trimmed_idem);
        if (auto replay = check_idempotency(
                tx, trimmed_idem, endpoint, req_hash, actor_role, "chat.moderation", "create_sanction", target, trimmed_reason)) {
            return *replay;
        }

        const auto user_rows = tx.exec_params(
            "select id::text, name from users where lower(name) = lower($1) order by (name = $1) desc limit 1",
            target);
        const std::string user_id = user_rows.empty() ? std::string() : std::string(user_rows[0][0].c_str());
        const std::string canonical_subject = user_rows.empty() ? target : std::string(user_rows[0][1].c_str());

        const std::string pen_key = "penalty:" + canonical_subject;
        lock_chat_state(tx, pen_key);
        if (kind_lower == "ban") {
            lock_chat_state(tx, "login:" + canonical_subject);
        }

        const std::int64_t now_ms = db_now_ms(tx);
        const std::int64_t expires_ms = now_ms + static_cast<std::int64_t>(duration_sec) * 1000LL;

        const auto ins = tx.exec_params(
            "insert into admin_sanctions(subject, kind, reason, actor_role, expires_at) "
            "values ($1, $2, $3, $4, to_timestamp($5 / 1000.0)) "
            "returning id::text, (extract(epoch from created_at) * 1000)::bigint, (extract(epoch from expires_at) * 1000)::bigint",
            canonical_subject,
            kind_lower,
            trimmed_reason,
            std::string(admin_role_to_string(actor_role)),
            expires_ms);

        const std::string sanction_id = ins[0][0].c_str();
        const std::int64_t created_ms = ins[0][1].as<std::int64_t>();
        const std::int64_t persisted_expires_ms = ins[0][2].as<std::int64_t>();

        json penalty = read_chat_state(tx, pen_key).value_or(json::object());
        const std::int64_t prev_val = penalty.value(kind_lower, 0LL);
        penalty[kind_lower] = std::max(prev_val, persisted_expires_ms);
        write_chat_state(tx, pen_key, penalty);

        if (kind_lower == "ban") {
            erase_chat_state(tx, "login:" + canonical_subject);
            if (!user_id.empty()) {
                tx.exec_params(
                    "update sessions set revoked_at = coalesce(revoked_at, now()) "
                    "where user_id = $1::uuid and revoked_at is null",
                    user_id);
            }
        }

        const json resp_json{
            {"sanction_id", sanction_id},
            {"subject", canonical_subject},
            {"kind", kind_lower},
            {"duration_sec", duration_sec},
            {"created_at_unix_ms", created_ms},
            {"expires_at_unix_ms", persisted_expires_ms},
            {"idempotency_key", trimmed_idem},
            {"idempotent_replay", false},
        };

        insert_audit_log(
            tx, actor_role, "chat.moderation", "create_sanction_" + kind_lower, canonical_subject, trimmed_reason, trimmed_idem,
            "succeeded", json{{"sanction_id", sanction_id}, {"duration_sec", duration_sec}});
        save_idempotency(tx, trimmed_idem, endpoint, req_hash, 201, resp_json);
        tx.commit();

        publish_control_change(config_);
        return make_json_result(201, resp_json);
    } catch (const std::exception& ex) {
        return make_error_result(500, "create_sanction_failed", sanitize_connection_error(ex.what()));
    }
}

ChatOpHttpResult PostgresRedisChatOpsBackend::revoke_sanction(std::string_view sanction_id,
                                                              std::string_view reason,
                                                              std::string_view idempotency_key,
                                                              AdminRole actor_role) {
    if (config_.db_uri.empty()) {
        return make_error_result(503, "db_unconfigured", "DB_URI is not configured.");
    }

    const std::string target_id = trim_ascii(sanction_id);
    const std::string trimmed_reason = trim_ascii(reason);
    const std::string trimmed_idem = trim_ascii(idempotency_key);

    if (!is_valid_uuid(target_id)) {
        return make_error_result(400, "invalid_sanction_id", "Sanction ID must be a valid UUID.");
    }
    if (trimmed_reason.empty() || trimmed_reason.size() > 500) {
        return make_error_result(400, "reason_required", "A non-empty revoke reason (<= 500 chars) is required.");
    }
    if (!is_valid_idempotency_key(trimmed_idem)) {
        return make_error_result(400, "idempotency_key_required", "A valid ASCII idempotency_key is required.");
    }

    const std::string endpoint = "DELETE /api/v1/chat/sanctions/" + to_lower_ascii(target_id);
    const std::string req_hash = sha256_hex(endpoint + "\n" + trimmed_reason);

    try {
        pqxx::connection conn(config_.db_uri);
        pqxx::work tx(conn);
        tx.exec("SET LOCAL statement_timeout = 2500");

        lock_idempotency(tx, trimmed_idem);
        if (auto replay = check_idempotency(
                tx, trimmed_idem, endpoint, req_hash, actor_role, "chat.moderation", "revoke_sanction", target_id, trimmed_reason)) {
            return *replay;
        }

        const auto found = tx.exec_params(
            "select subject, kind, (revoked_at is not null) from admin_sanctions where id = $1::uuid",
            target_id);
        if (found.empty()) {
            insert_audit_log(
                tx, actor_role, "chat.moderation", "revoke_sanction", target_id, trimmed_reason, trimmed_idem, "failed",
                json{{"error", "sanction_not_found"}});
            tx.commit();
            return make_error_result(404, "sanction_not_found", "Sanction was not found.");
        }

        const std::string subject = found[0][0].c_str();
        const std::string kind = found[0][1].c_str();
        const std::string pen_key = "penalty:" + subject;
        lock_chat_state(tx, pen_key);

        tx.exec_params(
            "update admin_sanctions "
            "set revoked_at = coalesce(revoked_at, now()), "
            "revoked_role = coalesce(revoked_role, $2), "
            "revoked_reason = coalesce(revoked_reason, $3) "
            "where id = $1::uuid",
            target_id,
            std::string(admin_role_to_string(actor_role)),
            trimmed_reason);

        const auto max_rem = tx.exec_params(
            "select coalesce(max((extract(epoch from expires_at) * 1000)::bigint), 0) "
            "from admin_sanctions "
            "where subject = $1 and kind = $2 and revoked_at is null and expires_at > now()",
            subject,
            kind);
        const std::int64_t next_expires_ms = max_rem[0][0].as<std::int64_t>();

        json penalty = read_chat_state(tx, pen_key).value_or(json::object());
        penalty[kind] = next_expires_ms;
        write_chat_state(tx, pen_key, penalty);

        const json resp_json{
            {"status", "revoked"},
            {"sanction_id", target_id},
            {"subject", subject},
            {"kind", kind},
            {"next_expires_at_unix_ms", next_expires_ms},
            {"idempotency_key", trimmed_idem},
            {"idempotent_replay", false},
        };

        insert_audit_log(
            tx, actor_role, "chat.moderation", "revoke_sanction", subject, trimmed_reason, trimmed_idem, "succeeded",
            json{{"sanction_id", target_id}, {"kind", kind}, {"next_expires_at_unix_ms", next_expires_ms}});
        save_idempotency(tx, trimmed_idem, endpoint, req_hash, 200, resp_json);
        tx.commit();

        publish_control_change(config_);
        return make_json_result(200, resp_json);
    } catch (const std::exception& ex) {
        return make_error_result(500, "revoke_sanction_failed", sanitize_connection_error(ex.what()));
    }
}

ChatOpHttpResult PostgresRedisChatOpsBackend::inspect_dlq(std::size_t limit) {
    const std::size_t clamped_limit = std::clamp<std::size_t>(limit, 1, 100);
    const WbStreamsSnapshot snap = inspect_wb_streams_resp(
        effective_redis_uri(config_),
        "127.0.0.1",
        config_.redis_host_port,
        config_.wb_stream_key,
        config_.dlq_stream_key,
        config_.dead_stream_key,
        clamped_limit,
        config_.probe_timeout_ms);

    if (!snap.ok) {
        return make_error_result(503, "redis_unavailable", snap.error.empty() ? "Failed to inspect Redis DLQ." : snap.error);
    }

    json entries = json::array();
    for (const auto& e : snap.preview_entries) {
        entries.push_back(json{
            {"stream_id", e.stream_id},
            {"orig_event_id", e.orig_event_id},
            {"type", e.type},
            {"ts_ms", e.ts_ms},
            {"user_id", e.user_id},
            {"session_id", e.session_id},
            {"room_id", e.room_id},
            {"retry_count", e.retry_count},
            {"error", e.error},
        });
    }

    return make_json_result(200, json{
        {"wb_stream", config_.wb_stream_key},
        {"dlq_stream", config_.dlq_stream_key},
        {"dead_stream", config_.dead_stream_key},
        {"pending_stream_length", snap.pending_stream_length},
        {"dlq_length", snap.dlq_length},
        {"dead_length", snap.dead_length},
        {"entries", std::move(entries)},
    });
}

ChatOpHttpResult PostgresRedisChatOpsBackend::replay_dlq(std::size_t max_items,
                                                         std::string_view reason,
                                                         std::string_view idempotency_key,
                                                         AdminRole actor_role) {
    if (config_.db_uri.empty()) {
        return make_error_result(503, "db_unconfigured", "DB_URI is not configured.");
    }

    const std::size_t batch_limit = std::clamp<std::size_t>(max_items, 1, 100);
    const std::string trimmed_reason = trim_ascii(reason);
    const std::string trimmed_idem = trim_ascii(idempotency_key);

    if (trimmed_reason.empty() || trimmed_reason.size() > 500) {
        return make_error_result(400, "reason_required", "A non-empty audit reason (<= 500 chars) is required.");
    }
    if (!is_valid_idempotency_key(trimmed_idem)) {
        return make_error_result(400, "idempotency_key_required", "A valid ASCII idempotency_key is required.");
    }

    const std::string endpoint = "POST /api/v1/wb/dlq/replay";
    const std::string req_hash = sha256_hex(std::to_string(batch_limit) + "\n" + trimmed_reason);

    try {
        pqxx::connection conn(config_.db_uri);
        pqxx::work tx(conn);
        tx.exec("SET LOCAL statement_timeout = 5000");

        lock_idempotency(tx, trimmed_idem);
        if (auto replay = check_idempotency(
                tx, trimmed_idem, endpoint, req_hash, actor_role, "wb.dlq", "replay_dlq", config_.dlq_stream_key, trimmed_reason)) {
            return *replay;
        }

        auto redis = connect_redis(config_);
        if (!redis) {
            return make_error_result(503, "redis_unavailable", "Redis is unavailable for DLQ replay.");
        }

        const std::string r_uri = effective_redis_uri(config_);
        (void)ensure_stream_group_from_zero_resp(
            r_uri, "127.0.0.1", config_.redis_host_port, config_.dlq_stream_key, config_.dlq_group, config_.probe_timeout_ms);
        const std::string consumer_name = "admin_console_v2";

        std::vector<server::core::storage::redis::IRedisClient::StreamEntry> entries;
        server::core::storage::redis::IRedisClient::StreamAutoClaimResult claimed;
        if (redis->xautoclaim(config_.dlq_stream_key, config_.dlq_group, consumer_name, 5000, "0-0", batch_limit, claimed)) {
            for (auto& e : claimed.entries) {
                entries.push_back(std::move(e));
            }
        }
        if (entries.size() < batch_limit) {
            std::vector<server::core::storage::redis::IRedisClient::StreamEntry> fresh;
            if (redis->xreadgroup(
                    config_.dlq_stream_key,
                    config_.dlq_group,
                    consumer_name,
                    50,
                    batch_limit - entries.size(),
                    fresh)) {
                for (auto& e : fresh) {
                    entries.push_back(std::move(e));
                }
            }
        }

        std::size_t replayed_ok = 0;
        std::size_t retried_dlq = 0;
        std::size_t moved_dead = 0;

        for (const auto& entry : entries) {
            std::unordered_map<std::string, std::string> mp;
            mp.reserve(entry.fields.size());
            for (const auto& [k, v] : entry.fields) {
                mp.emplace(k, v);
            }

            const std::string orig_id = mp.count("orig_event_id") ? mp["orig_event_id"] : entry.id;
            int retry_count = 0;
            if (mp.count("retry_count")) {
                try {
                    retry_count = std::stoi(mp["retry_count"]);
                } catch (...) {
                    retry_count = 0;
                }
            }

            const std::string type = mp.count("type") ? mp["type"] : "unknown";
            long long ts_v = 0;
            if (mp.count("ts_ms")) {
                try {
                    ts_v = std::stoll(mp["ts_ms"]);
                } catch (...) {
                    ts_v = 0;
                }
            }
            const std::string uid = mp.count("user_id") ? mp["user_id"] : "";
            const std::string sid = mp.count("session_id") ? mp["session_id"] : "";
            const std::string rid = mp.count("room_id") ? mp["room_id"] : "";

            json payload_json = json::object();
            for (const auto& [k, v] : mp) {
                if (k == "orig_event_id" || k == "error" || k == "retry_count") {
                    continue;
                }
                payload_json[k] = v;
            }

            try {
                pqxx::subtransaction sub(tx);
                sub.exec_params(
                    "insert into session_events(event_id, type, ts, user_id, session_id, room_id, payload) "
                    "values ($1, $2, to_timestamp(($3)::bigint / 1000.0), "
                    "nullif($4, '')::uuid, nullif($5, '')::uuid, nullif($6, '')::uuid, $7::jsonb) "
                    "on conflict (event_id) do nothing",
                    orig_id,
                    type,
                    ts_v,
                    uid,
                    sid,
                    rid,
                    payload_json.dump());
                sub.commit();
                (void)ack_and_delete_stream_entry_resp(
                    r_uri, "127.0.0.1", config_.redis_host_port, config_.dlq_stream_key, config_.dlq_group, entry.id, config_.probe_timeout_ms);
                ++replayed_ok;
            } catch (const std::exception& sql_ex) {
                const std::string clean_err = sanitize_connection_error(sql_ex.what());
                std::vector<std::pair<std::string, std::string>> fields;
                fields.emplace_back("orig_event_id", orig_id);
                for (const auto& [k, v] : entry.fields) {
                    if (k != "retry_count" && k != "error" && k != "orig_event_id") {
                        fields.emplace_back(k, v);
                    }
                }
                fields.emplace_back("error", clean_err);

                if (retry_count + 1 >= static_cast<int>(config_.dlq_retry_max)) {
                    if (redis->xadd(config_.dead_stream_key, fields, nullptr, std::nullopt, true)) {
                        (void)ack_and_delete_stream_entry_resp(
                            r_uri, "127.0.0.1", config_.redis_host_port, config_.dlq_stream_key, config_.dlq_group, entry.id, config_.probe_timeout_ms);
                        ++moved_dead;
                    }
                } else {
                    fields.emplace_back("retry_count", std::to_string(retry_count + 1));
                    if (redis->xadd(config_.dlq_stream_key, fields, nullptr, std::nullopt, true)) {
                        (void)ack_and_delete_stream_entry_resp(
                            r_uri, "127.0.0.1", config_.redis_host_port, config_.dlq_stream_key, config_.dlq_group, entry.id, config_.probe_timeout_ms);
                        ++retried_dlq;
                    }
                }
            }
        }

        const WbStreamsSnapshot after_snap = inspect_wb_streams_resp(
            effective_redis_uri(config_),
            "127.0.0.1",
            config_.redis_host_port,
            config_.wb_stream_key,
            config_.dlq_stream_key,
            config_.dead_stream_key,
            10,
            config_.probe_timeout_ms);

        const json resp_json{
            {"status", "completed"},
            {"attempted", entries.size()},
            {"replayed_ok", replayed_ok},
            {"retried_dlq", retried_dlq},
            {"moved_dead", moved_dead},
            {"dlq_length", after_snap.ok ? after_snap.dlq_length : -1},
            {"dead_length", after_snap.ok ? after_snap.dead_length : -1},
            {"idempotency_key", trimmed_idem},
            {"idempotent_replay", false},
        };

        insert_audit_log(
            tx,
            actor_role,
            "wb.dlq",
            "replay_dlq",
            config_.dlq_stream_key,
            trimmed_reason,
            trimmed_idem,
            "succeeded",
            json{
                {"attempted", entries.size()},
                {"replayed_ok", replayed_ok},
                {"retried_dlq", retried_dlq},
                {"moved_dead", moved_dead},
            });
        save_idempotency(tx, trimmed_idem, endpoint, req_hash, 200, resp_json);
        tx.commit();
        return make_json_result(200, resp_json);
    } catch (const std::exception& ex) {
        return make_error_result(500, "dlq_replay_failed", sanitize_connection_error(ex.what()));
    }
}

ChatOpHttpResult PostgresRedisChatOpsBackend::list_audit_logs(std::string_view actor_filter,
                                                              std::string_view action_filter,
                                                              std::string_view target_filter,
                                                              std::string_view status_filter,
                                                              std::size_t limit) {
    if (config_.db_uri.empty()) {
        return make_error_result(503, "db_unconfigured", "DB_URI is not configured.");
    }

    try {
        const std::string actor_q = to_lower_ascii(trim_ascii(actor_filter));
        const std::string action_q = to_lower_ascii(trim_ascii(action_filter));
        const std::string target_q = trim_ascii(target_filter);
        const std::string status_q = to_lower_ascii(trim_ascii(status_filter));
        const std::size_t clamped_limit = std::clamp<std::size_t>(limit, 1, 200);

        pqxx::connection conn(config_.db_uri);
        pqxx::work tx(conn);
        tx.exec("SET LOCAL statement_timeout = 2500");

        const auto rows = tx.exec_params(
            "select id, (extract(epoch from created_at) * 1000)::bigint, "
            "actor_role, capability, action, target, reason, "
            "coalesce(idempotency_key, ''), status, summary::text "
            "from admin_audit_logs "
            "where ($1 = '' or lower(actor_role) = $1) "
            "and ($2 = '' or lower(action) like ('%' || $2 || '%')) "
            "and ($3 = '' or lower(target) like ('%' || lower($3) || '%')) "
            "and ($4 = '' or lower(status) = $4) "
            "order by created_at desc, id desc "
            "limit $5",
            actor_q,
            action_q,
            target_q,
            status_q,
            static_cast<int>(clamped_limit));

        json items = json::array();
        for (const auto& row : rows) {
            json sum = json::object();
            try {
                sum = json::parse(row[9].c_str());
            } catch (...) {
            }
            items.push_back(json{
                {"id", row[0].as<std::int64_t>()},
                {"created_at_unix_ms", row[1].as<std::int64_t>()},
                {"actor_role", row[2].c_str()},
                {"capability", row[3].c_str()},
                {"action", row[4].c_str()},
                {"target", row[5].c_str()},
                {"reason", row[6].c_str()},
                {"idempotency_key", row[7].c_str()},
                {"status", row[8].c_str()},
                {"summary", std::move(sum)},
            });
        }
        tx.commit();

        return make_json_result(200, json{{"audit_logs", std::move(items)}});
    } catch (const std::exception& ex) {
        return make_error_result(500, "db_query_failed", sanitize_connection_error(ex.what()));
    }
}

void PostgresRedisChatOpsBackend::record_denied_audit(AdminRole actor_role,
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
        tx.exec("SET LOCAL statement_timeout = 1000");
        insert_audit_log(
            tx,
            actor_role,
            capability,
            action,
            target.empty() ? std::string_view("-") : target,
            reason,
            idempotency_key,
            "denied",
            json{{"error", std::string(deny_code)}});
        tx.commit();
    } catch (...) {
    }
}

} // namespace dynaxis::admin
