#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace server {
namespace storage::redis {
class IRedisClient;
} // namespace storage::redis
} // namespace server

namespace server::state {

/**
 * @brief 서버 인스턴스 정보 구조체
 * 
 * 로드 밸런싱과 서비스 디스커버리를 위해 각 서버 인스턴스가 자신의 상태를 공유 저장소에 등록합니다.
 * 이 구조체는 그 등록 정보를 담고 있습니다.
 */
struct InstanceRecord {
    std::string instance_id;      // 고유 인스턴스 ID (UUID 등)
    std::string host;             // 접속 가능한 호스트 주소 (IP 또는 도메인)
    std::uint16_t port{0};        // 접속 포트
    std::string role;             // 역할 (예: "chat-server", "login-server")
    std::uint32_t capacity{0};    // 최대 수용 가능한 세션 수
    std::uint32_t active_sessions{0}; // 현재 활성 세션 수
    std::uint64_t last_heartbeat_ms{0}; // 마지막 생존 신호 시간 (Epoch ms)
};

/**
 * @brief 인스턴스 상태 관리 백엔드 인터페이스
 * 
 * 다양한 저장소(Redis, Consul, In-Memory 등)를 통해 인스턴스 정보를 저장하고 조회하는
 * 공통 인터페이스를 정의합니다. 이를 통해 구체적인 저장소 구현에 의존하지 않고
 * 서비스 디스커버리 로직을 작성할 수 있습니다.
 */
class IInstanceStateBackend {
public:
    virtual ~IInstanceStateBackend() = default;
    
    // 인스턴스 정보를 등록하거나 갱신합니다.
    virtual bool upsert(const InstanceRecord& record) = 0;
    
    // 인스턴스 정보를 삭제합니다. (종료 시 호출)
    virtual bool remove(const std::string& instance_id) = 0;
    
    // 생존 신호(Heartbeat)를 갱신합니다.
    virtual bool touch(const std::string& instance_id, std::uint64_t heartbeat_ms) = 0;
    
    // 현재 등록된 모든 인스턴스 목록을 조회합니다.
    virtual std::vector<InstanceRecord> list_instances() const = 0;
};

class InMemoryStateBackend final : public IInstanceStateBackend {
public:
    bool upsert(const InstanceRecord& record) override;
    bool remove(const std::string& instance_id) override;
    bool touch(const std::string& instance_id, std::uint64_t heartbeat_ms) override;
    std::vector<InstanceRecord> list_instances() const override;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, InstanceRecord> records_;
};

namespace detail {
std::string serialize_json(const InstanceRecord& record);
std::optional<InstanceRecord> deserialize_json(std::string_view payload);
} // namespace detail

class RedisInstanceStateBackend final : public IInstanceStateBackend {
public:
    class IRedisClient {
    public:
        virtual ~IRedisClient() = default;
        virtual bool setex(const std::string& key, const std::string& value, unsigned int ttl_sec) = 0;
        virtual bool scan_keys(const std::string& pattern, std::vector<std::string>& keys) = 0;
        virtual std::optional<std::string> get(const std::string& key) = 0;
        virtual bool del(const std::string& key) = 0;
    };

    RedisInstanceStateBackend(std::shared_ptr<IRedisClient> client,
                              std::string key_prefix,
                              std::chrono::seconds ttl);

    bool upsert(const InstanceRecord& record) override;
    bool remove(const std::string& instance_id) override;
    bool touch(const std::string& instance_id, std::uint64_t heartbeat_ms) override;
    std::vector<InstanceRecord> list_instances() const override;

private:
    bool reload_cache_from_backend() const;
    bool write_record(const InstanceRecord& record);

    std::shared_ptr<IRedisClient> client_;
    std::string key_prefix_;
    std::chrono::seconds ttl_;

    // Cache refresh throttle: list_instances() may be called from hot paths (gateway accept).
    // Avoid doing SCAN+GET on every call.
    std::chrono::milliseconds reload_min_interval_{500};
    mutable std::chrono::steady_clock::time_point last_reload_attempt_{};
    mutable std::chrono::steady_clock::time_point last_reload_ok_{};
    mutable std::atomic<bool> reload_in_progress_{false};

    mutable std::mutex mutex_;
    mutable std::unordered_map<std::string, InstanceRecord> cache_;
};

class ConsulInstanceStateBackend final : public IInstanceStateBackend {
public:
    using http_callback = std::function<bool(const std::string& path, const std::string& payload)>;

    ConsulInstanceStateBackend(std::string base_path,
                               http_callback put_callback,
                               http_callback delete_callback);

    bool upsert(const InstanceRecord& record) override;
    bool remove(const std::string& instance_id) override;
    bool touch(const std::string& instance_id, std::uint64_t heartbeat_ms) override;
    std::vector<InstanceRecord> list_instances() const override;

private:
    std::string make_path(const std::string& instance_id) const;

    std::string base_path_;
    http_callback put_;
    http_callback del_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, InstanceRecord> cache_;
};

std::shared_ptr<RedisInstanceStateBackend::IRedisClient>
make_redis_state_client(std::shared_ptr<server::storage::redis::IRedisClient> client);

} // namespace server::state
