#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace server::storage::redis {
class IRedisClient;
} // namespace server::storage::redis

namespace gateway {

// Sticky Session을 관리하는 클래스입니다.
// Redis를 사용하여 클라이언트 ID와 백엔드 서버 ID 간의 매핑 정보를 저장하고 조회합니다.
// 로컬 인메모리 캐시(L1)와 Redis(L2)를 함께 사용하여 성능을 최적화합니다.
class SessionDirectory {
public:
    SessionDirectory(std::shared_ptr<server::storage::redis::IRedisClient> redis_client,
                     std::string key_prefix,
                     std::chrono::seconds ttl);

    std::optional<std::string> find_backend(const std::string& client_id);
    std::optional<std::string> ensure_backend(const std::string& client_id, const std::string& desired_backend);
    void refresh_backend(const std::string& client_id, const std::string& backend_id);
    void release_backend(const std::string& client_id, const std::string& backend_id);

private:
    std::string make_key(const std::string& client_id) const;

    struct CacheEntry {
        std::string backend;
        std::chrono::steady_clock::time_point expires;
    };

    std::shared_ptr<server::storage::redis::IRedisClient> redis_;
    std::string key_prefix_;
    std::chrono::seconds ttl_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, CacheEntry> cache_;
};

} // namespace gateway
