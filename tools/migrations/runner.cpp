#include <pqxx/pqxx>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct Migration {
    long long version{};
    fs::path path;
};

/**
 * @brief SQL 마이그레이션 러너 구현입니다.
 *
 * `schema_migrations` 버전 테이블로 재실행 안전성을 보장하고,
 * `CONCURRENTLY` 포함 스크립트는 non-transaction 경로로 분리해 PostgreSQL 제약을 준수합니다.
 */
static std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string s; in.seekg(0, std::ios::end); s.resize(static_cast<size_t>(in.tellg())); in.seekg(0);
    in.read(s.data(), static_cast<std::streamsize>(s.size()));
    return s;
}

/**
 * @brief 마이그레이션 적용 진입점입니다.
 * @param argc 커맨드라인 인자 개수
 * @param argv 커맨드라인 인자 배열
 * @return 종료 코드(0 정상)
 */
int main(int argc, char** argv) {
    try {
        bool dry_run = false;
        std::string db_uri;
        for (int i = 1; i < argc; ++i) {
            std::string a(argv[i]);
            if (a == "--dry-run") dry_run = true;
            else if ((a == "--db-uri" || a == "-u") && i + 1 < argc) { db_uri = argv[++i]; }
        }
        if (db_uri.empty()) {
            const char* e = std::getenv("DB_URI");
            if (!e || !*e) {
                std::cerr << "Usage: runner [--db-uri <uri>] [--dry-run]\n";
                return 2;
            }
            db_uri = e;
        }

        // 마이그레이션 파일 수집:
        // 파일명 숫자 prefix(예: 0001_...)를 버전으로 해석해 적용 순서를 결정한다.
        std::vector<Migration> migs;
        std::regex re_num(R"((\d+)_.*\.sql$)");
        fs::path dir;
        if (const char* env_dir = std::getenv("MIGRATIONS_DIR"); env_dir && *env_dir) {
            dir = env_dir;
        } else {
            dir = fs::path("tools") / "migrations";
        }
        std::cout << "Using migrations directory: " << dir << std::endl;
        for (auto& ent : fs::directory_iterator(dir)) {
            if (!ent.is_regular_file()) continue;
            auto name = ent.path().filename().string();
            std::smatch m; if (std::regex_search(name, m, re_num)) {
                long long v = std::stoll(m[1]);
                migs.push_back({v, ent.path()});
            }
        }
        std::sort(migs.begin(), migs.end(), [](const Migration& a, const Migration& b){ return a.version < b.version; });

        // DB 연결
        pqxx::connection c(db_uri);
        if (!c.is_open()) { std::cerr << "Failed to open connection" << std::endl; return 3; }

        // schema_migrations 테이블 보장:
        // 이미 적용한 버전을 기록해 idempotent 재실행을 가능하게 한다.
        {
            pqxx::work w(c);
            w.exec("create table if not exists schema_migrations (version bigint primary key, applied_at timestamptz not null default now())");
            w.commit();
        }

        // 적용 완료 버전 로드
        std::set<long long> applied;
        {
            pqxx::work w(c);
            auto r = w.exec("select version from schema_migrations");
            for (auto const& row : r) { applied.insert(row[0].as<long long>()); }
        }

        // 실행 계획: 아직 적용되지 않은 버전만 추린다.
        std::vector<Migration> plan;
        for (auto& m : migs) if (!applied.count(m.version)) plan.push_back(m);

        std::cout << "Pending migrations: " << plan.size() << std::endl;
        for (auto& m : plan) std::cout << "  - " << m.path.filename().string() << " (" << m.version << ")\n";
        if (dry_run) return 0;

        // 마이그레이션 적용
        for (auto& m : plan) {
            auto sql = slurp(m.path);
            bool non_tx = sql.find("concurrently") != std::string::npos || sql.find("CONCURRENTLY") != std::string::npos;
            std::cout << "Applying " << m.path.filename().string() << (non_tx ? " (non-tx)" : "") << std::endl;
            if (non_tx) {
                pqxx::nontransaction n(c);
                // CONCURRENTLY가 포함된 스크립트는 트랜잭션 블록 안에서 실행할 수 없다.
                // 세미콜론 단위로 쪼개 nontransaction에서 순차 실행한다.
                std::regex re_split(R"(;)");
                std::sregex_token_iterator it(sql.begin(), sql.end(), re_split, -1);
                std::sregex_token_iterator end;
                for (; it != end; ++it) {
                    std::string stmt = *it;
                    // 공백-only statement는 제거
                    stmt.erase(0, stmt.find_first_not_of(" \t\n\r"));
                    stmt.erase(stmt.find_last_not_of(" \t\n\r") + 1);
                    if (!stmt.empty()) {
                        try {
                            n.exec(stmt);
                        } catch (const std::exception& e) {
                            std::cerr << "Failed statement: " << stmt << "\nError: " << e.what() << std::endl;
                            throw;
                        }
                    }
                }
            } else {
                pqxx::work w(c);
                w.exec(sql);
                w.commit();
            }
            pqxx::work w2(c);
            w2.exec_params("insert into schema_migrations(version) values($1)", m.version);
            w2.commit();
        }

        std::cout << "Done." << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "runner error: " << e.what() << std::endl;
        return 1;
    }
}
