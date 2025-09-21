// UTF-8, 한국어 주석
#include <iostream>
#include <string>
#include <cstdlib>
#include <pqxx/pqxx>
#include "server/core/config/dotenv.hpp"

int main(int argc, char** argv) {
    try {
        // .env가 있으면 이를 우선(override=true), 없으면 OS 환경변수 사용
        (void)server::core::config::load_dotenv(".env", true);
        if (argc < 2) { std::cerr << "usage: wb_check <event_id>" << std::endl; return 2; }
        std::string event_id = argv[1];
        const char* db = std::getenv("DB_URI");
        if (!db || !*db) { std::cerr << "wb_check: DB_URI not set" << std::endl; return 3; }
        pqxx::connection c(db);
        if (!c.is_open()) { std::cerr << "wb_check: DB open failed" << std::endl; return 4; }
        pqxx::work w(c);
        auto r = w.exec_params("select 1 from session_events where event_id=$1 limit 1", event_id);
        if (r.empty()) { std::cerr << "not found" << std::endl; return 5; }
        std::cout << "found" << std::endl; return 0;
    } catch (const std::exception& e) {
        std::cerr << "wb_check error: " << e.what() << std::endl; return 1;
    }
}
