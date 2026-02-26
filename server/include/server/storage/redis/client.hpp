#pragma once

#include <memory>
#include <string>
#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

namespace server::storage::redis {

/** @brief Redis 클라이언트 생성 옵션입니다. */
struct Options {
    std::size_t pool_max{10};  ///< 연결 풀 최대 크기
    bool use_streams{false};   ///< Streams 명령 사용 여부
};

/**
 * @brief Redis 클라이언트 추상 인터페이스
 * 
 * Redis의 다양한 명령어(String, Set, List, Pub/Sub, Streams)를 추상화하여
 * 실제 Redis 라이브러리(redis-plus-plus 등)와의 의존성을 격리합니다.
 * 이를 통해 테스트 시 모의 객체로 대체하거나 다른 클라이언트로 교체하기 쉽습니다.
 */
class IRedisClient {
public:
    virtual ~IRedisClient() = default;

    /**
     * @brief 연결 상태를 확인합니다.
     * @return 연결이 정상일 때 `true`
     */
    virtual bool health_check() = 0;

    /**
     * @brief List 왼쪽에 값을 추가하고 최대 길이를 유지합니다.
     * @param key 대상 리스트 키
     * @param value 추가할 값
     * @param maxlen 유지할 최대 길이
     * @return 명령 성공 시 `true`
     */
    virtual bool lpush_trim(const std::string& key, const std::string& value, std::size_t maxlen) = 0;

    /**
     * @brief Set 멤버를 추가합니다.
     * @param key 대상 Set 키
     * @param member 추가할 멤버
     * @return 명령 성공 시 `true`
     */
    virtual bool sadd(const std::string& key, const std::string& member) = 0;

    /**
     * @brief Set 멤버를 제거합니다.
     * @param key 대상 Set 키
     * @param member 제거할 멤버
     * @return 명령 성공 시 `true`
     */
    virtual bool srem(const std::string& key, const std::string& member) = 0;

    /**
     * @brief Set의 모든 멤버를 조회합니다.
     * @param key 대상 Set 키
     * @param out 조회 결과 버퍼
     * @return 명령 성공 시 `true`
     */
    virtual bool smembers(const std::string& key, std::vector<std::string>& out) = 0;

    /**
     * @brief Set 멤버 수(`SCARD`)를 조회합니다.
     * @param key 대상 Set 키
     * @param out 멤버 수 출력 버퍼
     * @return 명령 성공 시 `true`
     */
    virtual bool scard(const std::string& key, std::size_t& out) = 0;

    /**
     * @brief 멤버 수를 다중 조회합니다(batched `SCARD`).
     * @param keys 조회할 키 목록
     * @param out 키 순서와 동일한 멤버 수 출력 버퍼(`out.size()==keys.size()` 계약)
     * @return 명령 성공 시 `true`
     */
    virtual bool scard_many(const std::vector<std::string>& keys,
                            std::vector<std::size_t>& out) = 0;

    /**
     * @brief 키를 삭제합니다.
     * @param key 삭제할 키
     * @return 명령 성공 시 `true`
     */
    virtual bool del(const std::string& key) = 0;

    /**
     * @brief String 값을 조회합니다.
     * @param key 조회할 키
     * @return 조회 결과(미존재 시 `std::nullopt`)
     */
    virtual std::optional<std::string> get(const std::string& key) = 0;

    /**
     * @brief String 다중 조회(`MGET`)를 수행합니다.
     * @param keys 조회할 키 목록
     * @param out 키 순서와 동일한 결과 버퍼(`out.size()==keys.size()`, 누락은 `std::nullopt`)
     * @return 명령 성공 시 `true`
     */
    virtual bool mget(const std::vector<std::string>& keys,
                      std::vector<std::optional<std::string>>& out) = 0;

    /**
     * @brief 키가 없을 때만 값을 설정합니다(`SET NX EX`).
     * @param key 대상 키
     * @param value 저장할 값
     * @param ttl_sec TTL(초)
     * @return 저장 성공 시 `true`
     */
    virtual bool set_if_not_exists(const std::string& key, const std::string& value, unsigned int ttl_sec) = 0;

    /**
     * @brief 기존 값이 일치할 때만 값을 갱신합니다(CAS 유사).
     * @param key 대상 키
     * @param expected 기대 기존 값
     * @param value 새 값
     * @param ttl_sec TTL(초)
     * @return 갱신 성공 시 `true`
     */
    virtual bool set_if_equals(const std::string& key, const std::string& expected, const std::string& value, unsigned int ttl_sec) = 0;

    /**
     * @brief 기존 값이 일치할 때만 키를 삭제합니다(안전한 락 해제).
     * @param key 대상 키
     * @param expected 기대 기존 값
     * @return 삭제 성공 시 `true`
     */
    virtual bool del_if_equals(const std::string& key, const std::string& expected) = 0;

    /**
     * @brief 패턴 매칭으로 키 목록을 조회합니다(`SCAN`).
     * @param pattern 스캔 패턴
     * @param keys 조회 결과 버퍼
     * @return 명령 성공 시 `true`
     */
    virtual bool scan_keys(const std::string& pattern, std::vector<std::string>& keys) = 0;

    /**
     * @brief List 범위 조회(`LRANGE`)를 수행합니다.
     * @param key 대상 리스트 키
     * @param start 시작 인덱스
     * @param stop 종료 인덱스
     * @param out 조회 결과 버퍼
     * @return 명령 성공 시 `true`
     */
    virtual bool lrange(const std::string& key, long long start, long long stop, std::vector<std::string>& out) = 0;

    /**
     * @brief 패턴 스캔 후 일괄 삭제를 수행합니다(`SCAN` -> `DEL`).
     * @param pattern 삭제 대상 키 패턴
     * @return 명령 성공 시 `true`
     */
    virtual bool scan_del(const std::string& pattern) = 0;

    /**
     * @brief TTL을 가진 키를 설정합니다(초 단위).
     * @param key 대상 키
     * @param value 저장할 값
     * @param ttl_sec TTL(초)
     * @return 명령 성공 시 `true`
     */
    virtual bool setex(const std::string& key, const std::string& value, unsigned int ttl_sec) = 0;

    /**
     * @brief Pub/Sub 채널에 메시지를 발행합니다.
     * @param channel 발행할 채널
     * @param message 발행할 메시지
     * @return 명령 성공 시 `true`
     */
    virtual bool publish(const std::string& channel, const std::string& message) = 0;

    /**
     * @brief 패턴 기반 Pub/Sub 구독을 시작합니다.
     * @param pattern 구독 패턴
     * @param on_message 수신 시 호출될 콜백
     * @return 구독 시작 성공 시 `true`
     */
    virtual bool start_psubscribe(const std::string& pattern,
                                  std::function<void(const std::string& channel, const std::string& message)> on_message) = 0;

    /** @brief 실행 중인 Pub/Sub 구독을 중지합니다. */
    virtual void stop_psubscribe() = 0;

    /**
     * @brief 스트림 소비자 그룹을 생성합니다(스트림이 없으면 생성).
     * @param key 대상 스트림 키
     * @param group 생성할 소비자 그룹 이름
     * @return 명령 성공 시 `true`
     */
    virtual bool xgroup_create_mkstream(const std::string& key, const std::string& group) = 0;

    /**
     * @brief 스트림에 메시지를 추가합니다(`XADD`).
     * @param key 대상 스트림 키
     * @param fields 추가할 필드 목록
     * @param out_id 생성된 엔트리 ID 출력 버퍼(optional)
     * @param maxlen MAXLEN 제한(optional)
     * @param approximate MAXLEN 근사 적용 여부
     * @return 명령 성공 시 `true`
     */
    virtual bool xadd(const std::string& key,
                      const std::vector<std::pair<std::string, std::string>>& fields,
                      std::string* out_id = nullptr,
                      std::optional<std::size_t> maxlen = std::nullopt,
                      bool approximate = true) = 0;

    /** @brief Redis Stream 엔트리 표현입니다. */
    struct StreamEntry { std::string id; std::vector<std::pair<std::string,std::string>> fields; };

    /** @brief `XAUTOCLAIM` 결과 집합입니다. */
    struct StreamAutoClaimResult {
        std::string next_start_id;
        std::vector<StreamEntry> entries;
        // Redis 7+에서는 삭제된 ID의 3번째 배열이 올 수 있으므로 최선 노력으로 보존합니다.
        std::vector<std::string> deleted_ids;
    };

    /** @brief 소비자 그룹으로 메시지를 읽습니다(`XREADGROUP`). */
    virtual bool xreadgroup(const std::string& key, const std::string& group, const std::string& consumer,
                            long long block_ms, std::size_t count, std::vector<StreamEntry>& out) = 0;

    /**
     * @brief 메시지 처리 완료를 ACK합니다(`XACK`).
     * @param key 대상 스트림 키
     * @param group 소비자 그룹 이름
     * @param id ACK할 엔트리 ID
     * @return 명령 성공 시 `true`
     */
    virtual bool xack(const std::string& key, const std::string& group, const std::string& id) = 0;

    /**
     * @brief 컨슈머 그룹 대기(pending) 총량을 조회합니다(`XPENDING`).
     * @param key 대상 스트림 키
     * @param group 소비자 그룹 이름
     * @param total pending 총량 출력 버퍼
     * @return 명령 성공 시 `true`
     */
    virtual bool xpending(const std::string& key, const std::string& group, long long& total) = 0;

    /** @brief idle 시간이 지난 pending 메시지를 reclaim합니다(`XAUTOCLAIM`). */
    virtual bool xautoclaim(const std::string& key,
                            const std::string& group,
                            const std::string& consumer,
                            long long min_idle_ms,
                            const std::string& start,
                            std::size_t count,
                            StreamAutoClaimResult& out) = 0;
};

/**
 * @brief Redis 클라이언트/풀 구현체를 생성합니다.
 * @param uri Redis 접속 URI
 * @param opts 연결 풀/기능 옵션
 * @return 생성된 Redis 클라이언트
 */
std::shared_ptr<IRedisClient> make_redis_client(const std::string& uri, const Options& opts);

} // namespace server::storage::redis
