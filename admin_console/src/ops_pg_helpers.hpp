#pragma once

#include "admin/admin_types.hpp"
#include "admin/chat_ops_backend.hpp"

#include <nlohmann/json.hpp>
#include <openssl/sha.h>
#include <pqxx/pqxx>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dynaxis::admin::detail {

using json = nlohmann::json;

inline std::uint64_t current_unix_ms() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return ms < 0 ? 0ULL : static_cast<std::uint64_t>(ms);
}

inline std::string trim_ascii(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

inline std::string to_lower_ascii(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

inline bool is_valid_idempotency_key(std::string_view key) {
    if (key.empty() || key.size() > 128) {
        return false;
    }
    for (unsigned char ch : key) {
        if (ch < 0x21 || ch > 0x7E) {
            return false;
        }
    }
    return true;
}

inline bool is_valid_token_identifier(std::string_view val, std::size_t max_len = 64) {
    if (val.empty() || val.size() > max_len) {
        return false;
    }
    for (unsigned char ch : val) {
        if (ch < 0x21 || ch > 0x7E || ch == '/' || ch == '\\' || ch == '?' || ch == '#') {
            return false;
        }
    }
    return true;
}

inline bool is_valid_uuid(std::string_view s) {
    if (s.size() != 36) {
        return false;
    }
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') {
                return false;
            }
        } else if (std::isxdigit(static_cast<unsigned char>(c)) == 0) {
            return false;
        }
    }
    return true;
}

inline std::string sha256_hex(std::string_view input) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    if (SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest.data()) == nullptr) {
        return {};
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (unsigned char b : digest) {
        out.push_back(kHex[(b >> 4) & 0x0F]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

inline ChatOpHttpResult make_json_result(int status, const json& body) {
    return {status, body.dump()};
}

inline ChatOpHttpResult make_error_result(int status, std::string_view code, std::string_view message) {
    return {status, json{{"error", std::string(code)}, {"message", std::string(message)}}.dump()};
}

inline std::int64_t db_now_ms(pqxx::work& tx) {
    return tx.exec("select (extract(epoch from clock_timestamp()) * 1000)::bigint")[0][0].as<std::int64_t>();
}

inline void insert_audit_log(pqxx::work& tx,
                             AdminRole actor_role,
                             std::string_view capability,
                             std::string_view action,
                             std::string_view target,
                             std::string_view reason,
                             std::string_view idempotency_key,
                             std::string_view status,
                             const json& summary) {
    tx.exec_params(
        "insert into admin_audit_logs(actor_role, capability, action, target, reason, idempotency_key, status, summary) "
        "values ($1, $2, $3, $4, $5, nullif($6, ''), $7, $8::jsonb)",
        std::string(admin_role_to_string(actor_role)),
        std::string(capability),
        std::string(action),
        std::string(target),
        std::string(reason),
        std::string(idempotency_key),
        std::string(status),
        summary.dump());
}

inline void lock_idempotency(pqxx::work& tx, std::string_view idempotency_key) {
    tx.exec_params(
        "select pg_advisory_xact_lock(hashtextextended($1, 0))",
        "admin:idem:" + std::string(idempotency_key));
}

inline std::optional<ChatOpHttpResult> check_idempotency(pqxx::work& tx,
                                                         std::string_view idempotency_key,
                                                         std::string_view endpoint,
                                                         const std::string& request_hash,
                                                         AdminRole actor_role,
                                                         std::string_view capability,
                                                         std::string_view action,
                                                         std::string_view target,
                                                         std::string_view reason) {
    const auto rows = tx.exec_params(
        "select endpoint, request_hash, http_status, response_json::text "
        "from admin_idempotency_keys where idempotency_key = $1",
        std::string(idempotency_key));
    if (rows.empty()) {
        return std::nullopt;
    }

    const std::string existing_ep = rows[0][0].c_str();
    const std::string existing_hash = rows[0][1].c_str();
    const int stored_status = rows[0][2].as<int>();
    const std::string stored_body = rows[0][3].c_str();

    if (existing_ep == endpoint && existing_hash == request_hash) {
        try {
            json parsed = json::parse(stored_body);
            if (parsed.is_object()) {
                parsed["idempotent_replay"] = true;
            }
            return ChatOpHttpResult{stored_status, parsed.dump()};
        } catch (...) {
            return ChatOpHttpResult{stored_status, stored_body};
        }
    }

    insert_audit_log(
        tx,
        actor_role,
        capability,
        action,
        target,
        reason,
        idempotency_key,
        "conflict",
        json{{"expected_endpoint", existing_ep}, {"requested_endpoint", std::string(endpoint)}});
    tx.commit();
    return make_error_result(
        409,
        "idempotency_conflict",
        "Idempotency key already used with different request endpoint or parameters.");
}

inline void save_idempotency(pqxx::work& tx,
                             std::string_view idempotency_key,
                             std::string_view endpoint,
                             const std::string& request_hash,
                             int http_status,
                             const json& response_json) {
    tx.exec_params(
        "insert into admin_idempotency_keys(idempotency_key, endpoint, request_hash, http_status, response_json) "
        "values ($1, $2, $3, $4, $5::jsonb) "
        "on conflict (idempotency_key) do nothing",
        std::string(idempotency_key),
        std::string(endpoint),
        request_hash,
        http_status,
        response_json.dump());
}

} // namespace dynaxis::admin::detail
