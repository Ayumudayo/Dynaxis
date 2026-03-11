#include "gateway/session_directory.hpp"

#include <utility>

#include "server/core/storage/redis/client.hpp"

/**
 * @brief SessionDirectory의 L1 캐시 + Redis(L2) 매핑 구현입니다.
 *
 * 재접속 라우팅 연속성을 유지하면서도 조회 지연을 줄이기 위해,
 * 로컬 캐시를 우선 조회하고 Redis 원본을 뒤에서 동기화합니다.
 */
namespace gateway {

SessionDirectory::SessionDirectory(std::shared_ptr<server::core::storage::redis::IRedisClient> redis_client,
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
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(client_id);
        if (it != cache_.end()) {
            if (it->second.expires > now) {
                return it->second.backend;
            }
            cache_.erase(it);
        }
    }
    if (!redis_) {
        return std::nullopt;
    }
    auto value = redis_->get(make_key(client_id));
    if (value && !value->empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_[client_id] = CacheEntry{*value, std::chrono::steady_clock::now() + ttl_};
        return value;
    }
    return std::nullopt;
}

// 특정 클라이언트에게 할당된 백엔드가 있는지 확인하고, 없으면 desired_backend를 할당합니다.
// Redis의 SETNX(Set if Not Exists)와 유사한 원자적(Atomic) 연산을 수행하여
// 동시성 환경에서도 중복 할당을 방지합니다.
std::optional<std::string> SessionDirectory::ensure_backend(const std::string& client_id, const std::string& desired_backend) {
    if (client_id.empty() || desired_backend.empty()) {
        return std::nullopt;
    }

    if (!redis_) {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_[client_id] = CacheEntry{desired_backend, std::chrono::steady_clock::now() + ttl_};
        return desired_backend;
    }

    if (auto existing = find_backend(client_id)) {
        if (*existing == desired_backend) {
            redis_->set_if_equals(make_key(client_id), desired_backend, desired_backend, static_cast<unsigned int>(ttl_.count()));
        }
        return existing;
    }

    const auto key = make_key(client_id);
    if (redis_->set_if_not_exists(key, desired_backend, static_cast<unsigned int>(ttl_.count()))) {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_[client_id] = CacheEntry{desired_backend, std::chrono::steady_clock::now() + ttl_};
        return desired_backend;
    }

    auto current = redis_->get(key);
    if (current && !current->empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_[client_id] = CacheEntry{*current, std::chrono::steady_clock::now() + ttl_};
        return current;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    cache_[client_id] = CacheEntry{desired_backend, std::chrono::steady_clock::now() + ttl_};
    return desired_backend;
}

void SessionDirectory::refresh_backend(const std::string& client_id, const std::string& backend_id) {
    if (client_id.empty() || backend_id.empty()) {
        return;
    }
    if (redis_) {
        redis_->set_if_equals(make_key(client_id), backend_id, backend_id, static_cast<unsigned int>(ttl_.count()));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    cache_[client_id] = CacheEntry{backend_id, std::chrono::steady_clock::now() + ttl_};
}

void SessionDirectory::release_backend(const std::string& client_id, const std::string& backend_id) {
    if (client_id.empty() || backend_id.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(client_id);
        if (it != cache_.end() && it->second.backend == backend_id) {
            cache_.erase(it);
        }
    }
    if (redis_) {
        redis_->del_if_equals(make_key(client_id), backend_id);
    }
}

std::string SessionDirectory::make_key(const std::string& client_id) const {
    return key_prefix_ + client_id;
}

} // namespace gateway
