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

/**
 * @brief Sticky Session 매핑(client_id -> backend_id)을 관리합니다.
 *
 * Redis(L2)와 로컬 캐시(L1)를 함께 사용해,
 * 재접속 시 라우팅 연속성은 유지하면서 조회 지연을 낮추는 것이 목적입니다.
 */
class SessionDirectory {
public:
    /**
     * @brief 세션 디렉터리를 생성합니다.
     * @param redis_client Redis 클라이언트
     * @param key_prefix Redis 키 접두사
     * @param ttl 매핑 TTL
     */
    SessionDirectory(std::shared_ptr<server::storage::redis::IRedisClient> redis_client,
                     std::string key_prefix,
                     std::chrono::seconds ttl);

    /**
     * @brief client_id에 대응하는 backend를 조회합니다.
     * @param client_id 클라이언트 식별자
     * @return 매핑이 있으면 backend_id
     */
    std::optional<std::string> find_backend(const std::string& client_id);

    /**
     * @brief 기존 매핑을 유지하거나 없으면 desired_backend를 할당합니다.
     * @param client_id 클라이언트 식별자
     * @param desired_backend 신규 할당 후보 backend_id
     * @return 최종 backend_id
     */
    std::optional<std::string> ensure_backend(const std::string& client_id, const std::string& desired_backend);

    /**
     * @brief backend 매핑의 TTL을 갱신합니다.
     * @param client_id 클라이언트 식별자
     * @param backend_id 현재 연결 backend_id
     */
    void refresh_backend(const std::string& client_id, const std::string& backend_id);

    /**
     * @brief 지정된 매핑이 현재 값과 일치할 때만 해제합니다.
     * @param client_id 클라이언트 식별자
     * @param backend_id 해제할 backend_id
     */
    void release_backend(const std::string& client_id, const std::string& backend_id);

private:
    std::string make_key(const std::string& client_id) const;

    /** @brief 로컬 캐시에서 client_id -> backend 매핑과 만료 시각을 보관합니다. */
    struct CacheEntry {
        std::string backend;                         ///< 캐시된 backend ID
        std::chrono::steady_clock::time_point expires; ///< 로컬 캐시 만료 시각
    };

    std::shared_ptr<server::storage::redis::IRedisClient> redis_;
    std::string key_prefix_;
    std::chrono::seconds ttl_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, CacheEntry> cache_;
};

} // namespace gateway
