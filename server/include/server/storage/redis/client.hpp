#pragma once

#include <memory>
#include <string>
#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

namespace server::storage::redis {


struct Options {
    std::size_t pool_max{10};
    bool use_streams{false};
};

/**
 * @brief Redis 클라이언트 추상 인터페이스
 * 
 * Redis의 다양한 명령어(String, Set, List, Pub/Sub, Streams)를 추상화하여
 * 실제 Redis 라이브러리(redis-plus-plus 등)와의 의존성을 격리합니다.
 * 이를 통해 테스트 시 Mock 객체로 대체하거나 다른 클라이언트로 교체하기 쉽습니다.
 */
class IRedisClient {
public:
    virtual ~IRedisClient() = default;
    
    // 연결 상태 확인
    virtual bool health_check() = 0;
    
    // List: 왼쪽으로 푸시하고 최대 길이 유지 (로그/메시지 큐 용도)
    virtual bool lpush_trim(const std::string& key, const std::string& value, std::size_t maxlen) = 0;
    
    // Set: 멤버 추가
    virtual bool sadd(const std::string& key, const std::string& member) = 0;
    
    // Set: 멤버 제거
    virtual bool srem(const std::string& key, const std::string& member) = 0;
    
    // Set: 모든 멤버 조회
    virtual bool smembers(const std::string& key, std::vector<std::string>& out) = 0;

    // Set: 멤버 수 조회 (SCARD)
    virtual bool scard(const std::string& key, std::size_t& out) = 0;
    
    // 키 삭제
    virtual bool del(const std::string& key) = 0;
    
    // String: 값 조회
    virtual std::optional<std::string> get(const std::string& key) = 0;

    // String: 다중 값 조회 (MGET)
    // out.size() 는 keys.size() 와 동일해야 하며, miss 는 std::nullopt 로 반환합니다.
    virtual bool mget(const std::vector<std::string>& keys,
                      std::vector<std::optional<std::string>>& out) = 0;
    
    // String: 키가 없을 때만 설정 (분산 락 구현 등에 사용)
    virtual bool set_if_not_exists(const std::string& key, const std::string& value, unsigned int ttl_sec) = 0;
    
    // String: 값이 일치할 때만 설정 (CAS - Compare And Swap 유사)
    virtual bool set_if_equals(const std::string& key, const std::string& expected, const std::string& value, unsigned int ttl_sec) = 0;
    
    // String: 값이 일치할 때만 삭제 (안전한 락 해제)
    virtual bool del_if_equals(const std::string& key, const std::string& expected) = 0;
    
    // Keys: 패턴 매칭으로 키 목록 조회 (주의: 성능에 영향을 줄 수 있음)
    virtual bool scan_keys(const std::string& pattern, std::vector<std::string>& keys) = 0;
    
    // List: 범위 조회
    virtual bool lrange(const std::string& key, long long start, long long stop, std::vector<std::string>& out) = 0;
    
    // 패턴 스캔 후 일괄 삭제(naive): SCAN pattern -> DEL
    virtual bool scan_del(const std::string& pattern) = 0;
    
    // TTL을 가진 키 설정(초 단위)
    virtual bool setex(const std::string& key, const std::string& value, unsigned int ttl_sec) = 0;
    
    // Pub/Sub publish
    virtual bool publish(const std::string& channel, const std::string& message) = 0;
    
    // Pub/Sub 구독(패턴): 백그라운드로 consume 루프를 돌며 콜백 호출
    virtual bool start_psubscribe(const std::string& pattern,
                                  std::function<void(const std::string& channel, const std::string& message)> on_message) = 0;
    
    // 구독 중지(소유 스레드 종료)
    virtual void stop_psubscribe() = 0;

    // Streams API (write-behind 최소 구현)
    // Redis Streams는 로그 기반의 강력한 메시지 큐 기능을 제공합니다.
    
    // 소비자 그룹 생성 (스트림이 없으면 생성)
    virtual bool xgroup_create_mkstream(const std::string& key, const std::string& group) = 0;
    
    // 스트림에 메시지 추가
    virtual bool xadd(const std::string& key,
                      const std::vector<std::pair<std::string, std::string>>& fields,
                      std::string* out_id = nullptr,
                      std::optional<std::size_t> maxlen = std::nullopt,
                      bool approximate = true) = 0;
                      
    struct StreamEntry { std::string id; std::vector<std::pair<std::string,std::string>> fields; };

    struct StreamAutoClaimResult {
        std::string next_start_id;
        std::vector<StreamEntry> entries;
        // Redis 7+ may return a 3rd array of deleted IDs; keep as best-effort.
        std::vector<std::string> deleted_ids;
    };
    
    // 소비자 그룹을 통해 메시지 읽기
    virtual bool xreadgroup(const std::string& key, const std::string& group, const std::string& consumer,
                            long long block_ms, std::size_t count, std::vector<StreamEntry>& out) = 0;
                            
    // 메시지 처리 완료 확인 (ACK)
    virtual bool xack(const std::string& key, const std::string& group, const std::string& id) = 0;
    
    // Pending length 조회(컨슈머 그룹의 대기 메시지 총량)
    virtual bool xpending(const std::string& key, const std::string& group, long long& total) = 0;

    // Pending reclaim (Redis 6.2+): idle time이 충분히 지난 pending 메시지를 reclaim합니다.
    virtual bool xautoclaim(const std::string& key,
                            const std::string& group,
                            const std::string& consumer,
                            long long min_idle_ms,
                            const std::string& start,
                            std::size_t count,
                            StreamAutoClaimResult& out) = 0;
};

// Redis 클라이언트/풀 팩토리(스켈레톤)
std::shared_ptr<IRedisClient> make_redis_client(const std::string& uri, const Options& opts);

} // namespace server::storage::redis
