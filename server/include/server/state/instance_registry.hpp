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
    std::string instance_id;          ///< 고유 인스턴스 ID(UUID 등)
    std::string host;                 ///< 접속 가능한 호스트 주소(IP 또는 도메인)
    std::uint16_t port{0};            ///< 접속 포트
    std::string role;                 ///< 역할(예: `chat-server`)
    std::uint32_t capacity{0};        ///< 최대 수용 가능한 세션 수
    std::uint32_t active_sessions{0}; ///< 현재 활성 세션 수
    bool ready{true};                 ///< 라우팅 가능 상태(readiness)
    std::uint64_t last_heartbeat_ms{0}; ///< 마지막 생존 신호 시간(Epoch ms)
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

    /**
     * @brief 인스턴스 정보를 등록하거나 갱신합니다.
     * @param record 저장할 인스턴스 스냅샷
     * @return 저장 성공 시 `true`
     */
    virtual bool upsert(const InstanceRecord& record) = 0;

    /**
     * @brief 인스턴스 정보를 삭제합니다(종료 시 호출).
     * @param instance_id 제거할 인스턴스 ID
     * @return 삭제 성공 시 `true`
     */
    virtual bool remove(const std::string& instance_id) = 0;

    /**
     * @brief 생존 신호(하트비트)를 갱신합니다.
     * @param instance_id 갱신할 인스턴스 ID
     * @param heartbeat_ms 하트비트 시각(Epoch ms)
     * @return 갱신 성공 시 `true`
     */
    virtual bool touch(const std::string& instance_id, std::uint64_t heartbeat_ms) = 0;

    /**
     * @brief 현재 등록된 모든 인스턴스 목록을 조회합니다.
     * @return 조회된 인스턴스 목록
     */
    virtual std::vector<InstanceRecord> list_instances() const = 0;
};

/** @brief 테스트/단일 프로세스 용도의 인메모리 인스턴스 상태 백엔드입니다. */
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
make_redis_state_client(std::shared_ptr<server::storage::redis::IRedisClient> client);

} // namespace server::state
