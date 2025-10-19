#include "load_balancer/session_directory.hpp"

#include <utility>

#include "server/storage/redis/client.hpp"

namespace load_balancer {

SessionDirectory::SessionDirectory(std::shared_ptr<server::storage::redis::IRedisClient> redis_client,
                                   std::string key_prefix,
                                   std::chrono::seconds ttl)
    : redis_(std::move(redis_client))
    , key_prefix_(std::move(key_prefix))
    , ttl_(ttl) {
    if (key_prefix_.empty()) {
        key_prefix_ = "gateway/session/";
    }
    if (key_prefix_.back() != '/') {
        key_prefix_.push_back('/');
    }
    if (ttl_ <= std::chrono::seconds::zero()) {
        ttl_ = std::chrono::seconds{30};
    }
}

std::optional<std::string> SessionDirectory::find_backend(const std::string& client_id) {
    if (client_id.empty()) {
        return std::nullopt;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(client_id);
        if (it != cache_.end()) {
            return it->second;
        }
    }
    if (!redis_) {
        return std::nullopt;
    }
    auto value = redis_->get(make_key(client_id));
    if (value && !value->empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_[client_id] = *value;
        return value;
    }
    return std::nullopt;
}

bool SessionDirectory::bind_backend(const std::string& client_id, const std::string& backend_id) {
    if (client_id.empty() || backend_id.empty()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_[client_id] = backend_id;
    }
    bool ok = true;
    if (redis_) {
        ok = redis_->setex(make_key(client_id), backend_id, static_cast<unsigned int>(ttl_.count()));
    }
    return ok;
}

void SessionDirectory::release_backend(const std::string& client_id) {
    if (client_id.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.erase(client_id);
    }
    if (!redis_) {
        return;
    }
    redis_->del(make_key(client_id));
}

std::string SessionDirectory::make_key(const std::string& client_id) const {
    return key_prefix_ + client_id;
}

} // namespace load_balancer
