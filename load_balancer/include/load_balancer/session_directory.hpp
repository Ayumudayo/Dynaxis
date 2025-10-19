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

namespace load_balancer {

class SessionDirectory {
public:
    SessionDirectory(std::shared_ptr<server::storage::redis::IRedisClient> redis_client,
                     std::string key_prefix,
                     std::chrono::seconds ttl);

    std::optional<std::string> find_backend(const std::string& client_id);
    bool bind_backend(const std::string& client_id, const std::string& backend_id);
    void release_backend(const std::string& client_id);

private:
    std::string make_key(const std::string& client_id) const;

    std::shared_ptr<server::storage::redis::IRedisClient> redis_;
    std::string key_prefix_;
    std::chrono::seconds ttl_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> cache_;
};

} // namespace load_balancer
