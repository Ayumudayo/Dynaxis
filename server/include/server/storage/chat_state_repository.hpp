#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace server::storage {

/**
 * @brief 채팅 앱의 작은 공유 제어 상태를 같은 DB 트랜잭션에서 읽고 갱신합니다.
 *
 * 방 권한·초대·활성 로그인·제재에 사용합니다. key는 앱이 소유하며 core에는 노출하지 않습니다.
 * lock()은 없는 key도 직렬화하고 commit/rollback 때 풀립니다. 읽기 뒤 갱신은 반드시 lock 안에서 합니다.
 */
class IChatStateRepository {
public:
    virtual ~IChatStateRepository() = default;
    /** @brief key의 트랜잭션 배타 잠금을 얻습니다. 여러 key는 정렬된 순서로 잠급니다.
     * @param key 앱이 소유한 제어 상태 키 */
    virtual void lock(const std::string& key) = 0;
    /** @brief JSON 제어 상태를 읽습니다.
     * @param key 조회할 키
     * @return JSON 문자열 또는 없는 키의 nullopt */
    virtual std::optional<std::string> get(const std::string& key) = 0;
    /** @brief JSON 제어 상태를 현재 트랜잭션에 씁니다.
     * @param key 갱신할 키
     * @param json 저장할 JSON */
    virtual void put(const std::string& key, const std::string& json) = 0;
    /** @brief 제어 상태를 현재 트랜잭션에서 제거합니다.
     * @param key 제거할 키 */
    virtual void erase(const std::string& key) = 0;
    /** @brief 노드 시계 차이에 영향받지 않는 시간을 조회합니다.
     * @return DB 기준 UTC 밀리초 */
    virtual std::uint64_t now_ms() = 0;
};

} // namespace server::storage
