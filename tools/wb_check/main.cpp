// UTF-8, 한국어 주석
#include <iostream>
#include <string>
#include <cstdlib>
#include <pqxx/pqxx>

// -----------------------------------------------------------------------------
// wb_check
// -----------------------------------------------------------------------------
// 특정 event_id가 PostgreSQL에 저장되었는지 확인하는 유틸리티.
// 사용법: wb_check <event_id>
/**
 * @brief `session_events`에 event_id가 존재하는지 확인하는 CLI 진입점입니다.
 * @param argc 커맨드라인 인자 개수
 * @param argv 커맨드라인 인자 배열
 * @return 종료 코드(0: found, 5: not found)
 */
int main(int argc, char** argv) {
    try {
        // .env 로드

        if (argc < 2) {
            std::cerr << "usage: wb_check <event_id>" << std::endl;
            return 2;
        }

        std::string event_id = argv[1];
        const char* db_uri = std::getenv("DB_URI");
        if (!db_uri || !*db_uri) {
            std::cerr << "wb_check: DB_URI not set" << std::endl;
            return 3;
        }

        pqxx::connection c(db_uri);
        if (!c.is_open()) {
            std::cerr << "wb_check: DB open failed" << std::endl;
            return 4;
        }

        pqxx::work w(c);
        // session_events 테이블에서 event_id 존재 여부만 빠르게 확인
        auto r = w.exec_params("select 1 from session_events where event_id=$1 limit 1", event_id);

        if (r.empty()) {
            std::cerr << "not found" << std::endl;
            return 5;
        }

        std::cout << "found" << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "wb_check error: " << e.what() << std::endl;
        return 1;
    }
}
