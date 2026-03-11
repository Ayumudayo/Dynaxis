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

#include "server/core/state/instance_registry.hpp"
#include "server/core/storage/redis/client.hpp"

namespace server::state {
using server::core::state::InstanceRecord;
using server::core::state::InstanceSelector;
using server::core::state::SelectorMatchStats;
using server::core::state::SelectorPolicyLayer;
using server::core::state::matches_selector;
using server::core::state::classify_selector_policy_layer;
using server::core::state::selector_policy_layer_name;
using server::core::state::select_instances;
using server::core::state::IInstanceStateBackend;
using server::core::state::InMemoryStateBackend;

namespace detail {
/**
 * @brief InstanceRecord를 JSON 문자열로 직렬화합니다.
 * @param record 직렬화할 인스턴스 정보
 * @return 직렬화된 JSON 본문
 */
std::string serialize_json(const InstanceRecord& record);
/**
 * @brief JSON 본문을 InstanceRecord로 역직렬화합니다.
 * @param payload 역직렬화할 JSON 문자열
 * @return 파싱 성공 시 `InstanceRecord`, 실패 시 `std::nullopt`
 */
std::optional<InstanceRecord> deserialize_json(std::string_view payload);
} // namespace detail

/**
 * @brief Redis 기반 인스턴스 상태 백엔드입니다.
 *
 * Redis keyspace에 인스턴스 상태를 저장하고, gateway 조회 경로의 부하를 줄이기 위해
 * 내부 캐시를 주기적으로 재적재합니다.
 */
class RedisInstanceStateBackend final : public IInstanceStateBackend {
public:
    /** @brief Redis 의존성 분리를 위한 최소 클라이언트 인터페이스입니다. */
    class IRedisClient {
    public:
        virtual ~IRedisClient() = default;
        /**
         * @brief TTL 키를 설정합니다.
         * @param key 대상 키
         * @param value 저장할 값
         * @param ttl_sec TTL(초)
         * @return 명령 성공 시 `true`
         */
        virtual bool setex(const std::string& key, const std::string& value, unsigned int ttl_sec) = 0;
        /**
         * @brief 패턴에 매칭되는 키 목록을 스캔합니다.
         * @param pattern 스캔 패턴
         * @param keys 조회된 키 목록 출력 버퍼
         * @return 명령 성공 시 `true`
         */
        virtual bool scan_keys(const std::string& pattern, std::vector<std::string>& keys) = 0;
        /**
         * @brief 단일 키 값을 조회합니다.
         * @param key 조회할 키
         * @return 조회 결과(미존재 시 `std::nullopt`)
         */
        virtual std::optional<std::string> get(const std::string& key) = 0;
        /**
         * @brief 다중 키 값을 조회합니다.
         * @param keys 조회할 키 목록
         * @param out 조회 결과 버퍼
         * @return 명령 성공 시 `true`
         */
        virtual bool mget(const std::vector<std::string>& keys,
                          std::vector<std::optional<std::string>>& out) = 0;
        /**
         * @brief 키를 삭제합니다.
         * @param key 삭제할 키
         * @return 명령 성공 시 `true`
         */
        virtual bool del(const std::string& key) = 0;
    };

    /**
     * @brief Redis 상태 백엔드를 생성합니다.
     * @param client Redis 접근 클라이언트
     * @param key_prefix 인스턴스 키 접두어
     * @param ttl 인스턴스 레코드 TTL
     */
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

    // 캐시 갱신 쓰로틀: list_instances()는 핫패스(gateway accept)에서 자주 호출될 수 있음.
    // 매 호출마다 SCAN+GET을 수행하지 않도록 최소 간격을 둡니다.
    std::chrono::milliseconds reload_min_interval_{500};
    mutable std::chrono::steady_clock::time_point last_reload_attempt_{};
    mutable std::chrono::steady_clock::time_point last_reload_ok_{};
    mutable std::atomic<bool> reload_in_progress_{false};

    mutable std::mutex mutex_;
    mutable std::unordered_map<std::string, InstanceRecord> cache_;
};

/** @brief Consul KV를 사용하는 인스턴스 상태 백엔드입니다. */
class ConsulInstanceStateBackend final : public IInstanceStateBackend {
public:
    using http_callback = std::function<bool(const std::string& path, const std::string& payload)>;

    /**
     * @brief Consul KV 기반 인스턴스 상태 백엔드를 생성합니다.
     * @param base_path KV 기본 경로
     * @param put_callback PUT 요청 콜백
     * @param delete_callback DELETE 요청 콜백
     */
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

/**
 * @brief 서버 Redis 클라이언트를 registry 전용 인터페이스로 감쌉니다.
 * @param client 원본 Redis 클라이언트
 * @return registry 백엔드에서 사용할 어댑터
 */
std::shared_ptr<RedisInstanceStateBackend::IRedisClient>
make_redis_state_client(std::shared_ptr<server::core::storage::redis::IRedisClient> client);

} // namespace server::state
