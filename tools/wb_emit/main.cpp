// UTF-8, 한국어 주석
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <optional>

#include "../wb_common/redis_client_factory.hpp"
#include "server/core/storage/redis/client.hpp"

// -----------------------------------------------------------------------------
// wb_emit
// -----------------------------------------------------------------------------
// Redis Write-Behind 스트림에 수동으로 이벤트를 발행하여 파이프라인을 테스트하는 도구.
// 사용법: wb_emit [event_type]
/**
 * @brief write-behind Redis Stream에 테스트 이벤트를 발행하는 CLI 진입점입니다.
 * @param argc 커맨드라인 인자 개수
 * @param argv 커맨드라인 인자 배열
 * @return 종료 코드(0 정상)
 */
int main(int argc, char** argv) {
    try {

        const char* ruri = std::getenv("REDIS_URI");
        if (!ruri || !*ruri) {
            std::cerr << "wb_emit: REDIS_URI not set" << std::endl;
            return 2;
        }

        std::string stream = std::getenv("REDIS_STREAM_KEY") ? std::getenv("REDIS_STREAM_KEY") : std::string("session_events");

        server::core::storage::redis::Options ropts{};
        auto redis = wb_tools::make_redis_client(ruri, ropts);
        if (!redis || !redis->health_check()) {
            std::cerr << "wb_emit: redis health failed" << std::endl;
            return 3;
        }

        std::vector<std::pair<std::string, std::string>> fields;
        
        // 기본 필드는 session_login과 동일한 스키마를 따르므로 실서비스 파이프라인을 그대로 타게 된다.
        std::string type = (argc > 1) ? std::string(argv[1]) : std::string("session_login");
        fields.emplace_back("type", type);

        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        fields.emplace_back("ts_ms", std::to_string(static_cast<long long>(now_ms)));
        
        // 테스트용 더미 UUID
        fields.emplace_back("session_id", "00000000-0000-0000-0000-000000000001");
        fields.emplace_back("user_id", "00000000-0000-0000-0000-000000000002");
        fields.emplace_back("room_id", "00000000-0000-0000-0000-000000000003");
        fields.emplace_back("payload", R"({"note":"smoke test"})");

        std::string id;
        if (!redis->xadd(stream, fields, &id, std::nullopt, true)) {
            std::cerr << "wb_emit: XADD failed" << std::endl;
            return 4;
        }

        std::cout << "Event emitted. ID: " << id << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "wb_emit error: " << e.what() << std::endl;
        return 1;
    }
}
