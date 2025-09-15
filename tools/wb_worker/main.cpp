// UTF-8, 한국어 주석
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <vector>
#include "server/storage/redis/client.hpp"
#include "server/core/util/log.hpp"

int main(int, char**) {
    using server::core::log::info;
    try {
        const char* ruri = std::getenv("REDIS_URI");
        if (!ruri || !*ruri) {
            std::cerr << "WB worker: REDIS_URI not set" << std::endl;
            return 2;
        }
        server::storage::redis::Options ropts{};
        auto redis = server::storage::redis::make_redis_client(ruri, ropts);
        if (!redis || !redis->health_check()) { std::cerr << "WB worker: Redis health check failed" << std::endl; return 3; }
        std::string stream = std::getenv("REDIS_STREAM_KEY") ? std::getenv("REDIS_STREAM_KEY") : std::string("session_events");
        std::string group  = std::getenv("WB_GROUP") ? std::getenv("WB_GROUP") : std::string("wb_group");
        std::string consumer = std::getenv("WB_CONSUMER") ? std::getenv("WB_CONSUMER") : std::string("wb_consumer");
        (void)redis->xgroup_create_mkstream(stream, group);
        info(std::string("WB worker consuming stream=") + stream + ", group=" + group + ", consumer=" + consumer);
        while (true) {
            std::vector<server::storage::redis::IRedisClient::StreamEntry> entries;
            if (!redis->xreadgroup(stream, group, consumer, 2000, 100, entries)) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }
            for (auto& e : entries) {
                std::string type;
                for (auto& f : e.fields) if (f.first == "type") { type = f.second; break; }
                info(std::string("WB handle id=") + e.id + ", type=" + type);
                (void)redis->xack(stream, group, e.id);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "WB worker error: " << e.what() << std::endl;
        return 1;
    }
}
