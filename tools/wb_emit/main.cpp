// UTF-8, 한국어 주석
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <cstdlib>

#include "server/core/config/dotenv.hpp"
#include "server/storage/redis/client.hpp"

int main(int argc, char** argv) {
    try {
        (void)server::core::config::load_dotenv(".env", true);
        const char* ruri = std::getenv("REDIS_URI");
        if (!ruri || !*ruri) { std::cerr << "wb_emit: REDIS_URI not set" << std::endl; return 2; }
        std::string stream = std::getenv("REDIS_STREAM_KEY") ? std::getenv("REDIS_STREAM_KEY") : std::string("session_events");
        server::storage::redis::Options ropts{};
        auto redis = server::storage::redis::make_redis_client(ruri, ropts);
        if (!redis || !redis->health_check()) { std::cerr << "wb_emit: redis health failed" << std::endl; return 3; }
        std::vector<std::pair<std::string,std::string>> fields;
        fields.emplace_back("type", (argc>1? std::string(argv[1]) : std::string("session_login")));
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        fields.emplace_back("ts_ms", std::to_string(static_cast<long long>(now_ms)));
        fields.emplace_back("session_id", "00000000-0000-0000-0000-000000000001");
        fields.emplace_back("user_id", "00000000-0000-0000-0000-000000000002");
        fields.emplace_back("room_id", "00000000-0000-0000-0000-000000000003");
        fields.emplace_back("payload", R"({"note":"smoke test"})");
        std::string id;
        if (!redis->xadd(stream, fields, &id, std::nullopt, true)) {
            std::cerr << "wb_emit: XADD failed" << std::endl; return 4;
        }
        std::cout << id << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "wb_emit error: " << e.what() << std::endl; return 1;
    }
}
