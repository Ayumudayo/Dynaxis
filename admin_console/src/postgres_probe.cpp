#include "admin/postgres_probe.hpp"

#include <pqxx/pqxx>

#include <algorithm>
#include <chrono>
#include <regex>
#include <set>

namespace dynaxis::admin {

std::string sanitize_connection_error(std::string raw) {
    // URI 형태의 user:password@ 자격증명과 keyword=value 형태의 password를 제거한다.
    static const std::regex uri_cred_re(R"(([A-Za-z][A-Za-z0-9+.-]*://)([^/@\s]+@))");
    raw = std::regex_replace(raw, uri_cred_re, "$1[redacted]@");

    static const std::regex kv_pass_re(R"((password\s*=\s*)('[^']*'|\S+))", std::regex::icase);
    raw = std::regex_replace(raw, kv_pass_re, "$1[redacted]");
    return raw;
}

std::int64_t count_expected_migrations(const std::filesystem::path& migrations_dir) {
    std::error_code ec;
    if (migrations_dir.empty() || !std::filesystem::exists(migrations_dir, ec) ||
        !std::filesystem::is_directory(migrations_dir, ec)) {
        return 0;
    }

    static const std::regex re_num(R"((\d+)_.*\.sql$)");
    std::set<long long> versions;
    for (const auto& entry : std::filesystem::directory_iterator(migrations_dir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        std::smatch m;
        if (std::regex_search(name, m, re_num)) {
            try {
                versions.insert(std::stoll(m[1]));
            } catch (...) {
            }
        }
    }
    return static_cast<std::int64_t>(versions.size());
}

LibpqxxPostgresProbe::LibpqxxPostgresProbe(std::string db_uri, std::filesystem::path migrations_dir)
    : db_uri_(std::move(db_uri)), migrations_dir_(std::move(migrations_dir)) {}

PostgresProbeResult LibpqxxPostgresProbe::probe(std::uint32_t deadline_ms) {
    PostgresProbeResult result;
    result.expected_migrations = count_expected_migrations(migrations_dir_);
    if (db_uri_.empty()) {
        result.error = "DB_URI is not configured";
        return result;
    }

    const std::uint32_t clamped_ms = std::clamp<std::uint32_t>(deadline_ms, 200, 10'000);
    const std::uint32_t connect_sec = std::max<std::uint32_t>(1, (clamped_ms + 999u) / 1000u);
    std::string conn_str = db_uri_;
    if (conn_str.find("connect_timeout") == std::string::npos) {
        if (conn_str.find("://") != std::string::npos) {
            conn_str += (conn_str.find('?') == std::string::npos) ? "?" : "&";
            conn_str += "connect_timeout=" + std::to_string(connect_sec);
        } else {
            conn_str += " connect_timeout=" + std::to_string(connect_sec);
        }
    }

    const auto started_at = std::chrono::steady_clock::now();
    try {
        pqxx::connection conn(conn_str);
        if (!conn.is_open()) {
            result.error = "PostgreSQL connection not open";
            return result;
        }

        pqxx::nontransaction tx(conn);
        tx.exec("SET statement_timeout = " + std::to_string(clamped_ms));

        try {
            const auto rooms_res = tx.exec("SELECT COUNT(*) FROM rooms WHERE is_active = true");
            if (!rooms_res.empty()) {
                result.active_rooms = rooms_res[0][0].as<std::int64_t>(0);
            }
        } catch (const std::exception&) {
            result.active_rooms = -1;
        }

        try {
            const auto mig_res = tx.exec("SELECT COUNT(*) FROM schema_migrations");
            if (!mig_res.empty()) {
                result.applied_migrations = mig_res[0][0].as<std::int64_t>(0);
            }
            if (result.expected_migrations > 0 && result.applied_migrations >= 0) {
                result.all_migrations_applied = (result.applied_migrations >= result.expected_migrations);
            } else {
                result.all_migrations_applied = (result.applied_migrations > 0);
            }
        } catch (const std::exception&) {
            result.applied_migrations = -1;
            result.all_migrations_applied = false;
        }

        result.healthy = true;
    } catch (const std::exception& ex) {
        result.error = sanitize_connection_error(ex.what());
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at);
    result.latency_ms = static_cast<std::uint32_t>(std::max<long long>(0, elapsed.count()));
    return result;
}

} // namespace dynaxis::admin
