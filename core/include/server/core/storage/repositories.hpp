#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace server::core::storage {

// ==============================================================================
// DTO (Data Transfer Object) 정의
// 데이터베이스나 저장소에서 가져온 데이터를 담는 구조체들입니다.
// ==============================================================================

/** @brief 사용자 엔터티 스냅샷입니다. */
struct User {
    std::string id;               ///< 사용자 UUID(text)
    std::string name;             ///< 고유 닉네임
    std::int64_t created_at_ms{}; ///< 생성 시각(UTC epoch ms)
};

/** @brief 채팅방 엔터티 스냅샷입니다. */
struct Room {
    std::string id;                          ///< 방 UUID(text)
    std::string name;                        ///< 방 이름(고유)
    bool        is_public{true};             ///< 공개 방 여부
    bool        is_active{true};             ///< 활성 상태 여부
    std::optional<std::int64_t> closed_at_ms; ///< 방 종료 시각(NULL 가능)
    std::int64_t created_at_ms{};            ///< 생성 시각(UTC epoch ms)
};

/** @brief 메시지 엔터티 스냅샷입니다. */
struct Message {
    std::uint64_t id{};                    ///< 메시지 고유 ID(Sequence/Auto Increment)
    std::string   room_id;                 ///< 방 ID(UUID)
    std::string   room_name;               ///< 방 이름(조회 성능용 중복 저장)
    std::optional<std::string> user_id;    ///< 발신 유저 ID(NULL이면 시스템 메시지)
    std::optional<std::string> user_name;  ///< 발신 유저 이름
    std::string   content;                 ///< 메시지 본문
    std::int64_t  created_at_ms{};         ///< 생성 시각(UTC epoch ms)
};

/** @brief 방 멤버십 엔터티 스냅샷입니다. */
struct Membership {
    std::string user_id;                         ///< 유저 ID
    std::string room_id;                         ///< 방 ID
    std::string role;                            ///< 역할(예: `member`, `admin`)
    std::int64_t joined_at_ms{};                 ///< 입장 시각(UTC epoch ms)
    std::optional<std::uint64_t> last_seen_msg_id; ///< 마지막 읽은 메시지 ID
    std::optional<std::int64_t> left_at_ms;      ///< 퇴장 시각(NULL이면 현재 참여 중)
    bool is_member{true};                        ///< 현재 멤버 여부(편의 필드)
};

/** @brief 인증 세션 엔터티 스냅샷입니다. */
struct Session {
    std::string id;                             ///< 세션 ID(UUID)
    std::string user_id;                        ///< 유저 ID
    std::string token_hash;                     ///< 해시된 인증 토큰
    std::optional<std::string> client_ip;       ///< 클라이언트 IP 주소
    std::optional<std::string> user_agent;      ///< 클라이언트 정보(앱/브라우저)
    std::int64_t created_at_ms{};               ///< 세션 생성 시각(UTC epoch ms)
    std::int64_t expires_at_ms{};               ///< 세션 만료 시각(UTC epoch ms)
    std::optional<std::int64_t> revoked_at_ms;  ///< 강제 만료 시각(NULL이면 유효)
};

// ==============================================================================
// Repository 인터페이스
// 데이터 저장소에 접근하는 메서드들을 추상화한 인터페이스입니다.
// 이를 통해 실제 DB(PostgreSQL, Redis 등) 구현체와 비즈니스 로직을 분리합니다.
// (Dependency Inversion Principle 적용)
// ==============================================================================

/**
 * @brief 유저 정보 저장소 인터페이스
 */
class IUserRepository {
public:
    virtual ~IUserRepository() = default;

    // ID로 유저 찾기
    virtual std::optional<User> find_by_id(const std::string& user_id) = 0;
    
    // 이름으로 유저 검색 (대소문자 구분 없음 - CI: Case Insensitive)
    virtual std::vector<User> find_by_name_ci(const std::string& name, std::size_t limit) = 0;
    
    // 게스트 유저 생성
    virtual User create_guest(const std::string& name) = 0;
    
    // 마지막 로그인 정보 업데이트
    virtual void update_last_login(const std::string& user_id,
                                   const std::string& ip) = 0;
};

/**
 * @brief 채팅방 정보 저장소 인터페이스
 */
class IRoomRepository {
public:
    virtual ~IRoomRepository() = default;

    // ID로 방 찾기
    virtual std::optional<Room> find_by_id(const std::string& room_id) = 0;
    
    // 이름으로 방 검색 (부분 일치, 대소문자 구분 없음)
    virtual std::vector<Room> search_by_name_ci(const std::string& query, std::size_t limit) = 0;
    
    // 정확한 이름으로 방 찾기 (대소문자 구분 없음)
    virtual std::optional<Room> find_by_name_exact_ci(const std::string& name) = 0;
    
    // 새로운 방 생성
    virtual Room create(const std::string& name, bool is_public) = 0;

    // 방 닫기 (비활성화)
    virtual void close(const std::string& room_id) = 0;
};

/**
 * @brief 채팅 메시지 저장소 인터페이스
 */
class IMessageRepository {
public:
    virtual ~IMessageRepository() = default;

    /**
     * @brief 특정 방의 최근 메시지들을 가져옵니다.
     * @param room_id 방 ID
     * @param since_id 이 ID 이후의 메시지만 가져옵니다 (Paging 용도)
     * @param limit 가져올 최대 메시지 수
     */
    virtual std::vector<Message> fetch_recent_by_room(const std::string& room_id,
                                                      std::uint64_t since_id,
                                                      std::size_t limit) = 0;
    
    // 메시지 저장
    virtual Message create(const std::string& room_id,
                           const std::string& room_name,
                           const std::optional<std::string>& user_id,
                           const std::string& content) = 0;
    
    // 해당 방의 마지막 메시지 ID 조회 (없으면 0)
    virtual std::uint64_t get_last_id(const std::string& room_id) = 0;

    // 방의 모든 메시지 삭제 (방 삭제 시)
    virtual void delete_by_room(const std::string& room_id) = 0;
};


/**
 * @brief 방 참여 정보(멤버십) 저장소 인터페이스
 */
class IMembershipRepository {
public:
    virtual ~IMembershipRepository() = default;

    // 방 참여 또는 정보 업데이트 (Upsert: Update + Insert)
    virtual void upsert_join(const std::string& user_id,
                             const std::string& room_id,
                             const std::string& role) = 0;
    
    // 마지막으로 읽은 메시지 ID 업데이트 (Read Receipt)
    virtual void update_last_seen(const std::string& user_id,
                                  const std::string& room_id,
                                  std::uint64_t last_seen_msg_id) = 0;
    
    // 방 나가기 처리
    virtual void leave(const std::string& user_id,
                       const std::string& room_id) = 0;
    
    // 마지막으로 읽은 메시지 ID 조회
    virtual std::optional<std::uint64_t> get_last_seen(const std::string& user_id,
                                                       const std::string& room_id) = 0;
};

/**
 * @brief 세션 정보 저장소 인터페이스
 */
class ISessionRepository {
public:
    virtual ~ISessionRepository() = default;

    // 토큰 해시로 세션 찾기
    virtual std::optional<Session> find_by_token_hash(const std::string& token_hash) = 0;
    
    // 세션 생성
    virtual Session create(const std::string& user_id,
                           const std::chrono::system_clock::time_point& expires_at,
                           const std::optional<std::string>& client_ip,
                           const std::optional<std::string>& user_agent,
                           const std::string& token_hash) = 0;
    
    // 세션 만료/취소 처리
    virtual void revoke(const std::string& session_id) = 0;
};

} // namespace server::core::storage

