#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace server::storage {

/**
 * @brief 영속 저장소에서 읽고 쓰는 사용자 레코드입니다.
 *
 * 이 DTO는 앱 메모리 상태를 그대로 노출하려는 것이 아니라,
 * "저장소가 책임지는 사실"만 전달하기 위한 경계 객체입니다.
 * 세션 포인터나 런타임 캐시를 여기까지 끌고 오지 않아야 저장소 계층이
 * transport/메모리 수명 규칙에 오염되지 않습니다.
 */
struct User {
    std::string id;
    std::string name;
    std::int64_t created_at_ms{};
};

/**
 * @brief 방 검색과 생성 흐름이 공유하는 채팅방 레코드입니다.
 *
 * room DTO를 별도로 두는 이유는, 런타임의 room state와 영속 room state가
 * 항상 같은 필드를 가져야 하는 것은 아니기 때문입니다.
 * 예를 들어 참여 세션 집합 같은 런타임 전용 상태를 저장소 계약에 섞으면
 * DB/캐시/메모리 경계가 흐려집니다.
 */
struct Room {
    std::string id;
    std::string name;
    bool is_public{true};
    bool is_active{true};
    std::optional<std::int64_t> closed_at_ms;
    std::int64_t created_at_ms{};
};

/**
 * @brief 최근 메시지 조회와 적재가 공유하는 메시지 레코드입니다.
 *
 * 메시지 영속 레코드는 fanout payload와 비슷해 보여도 동일하지 않습니다.
 * 저장 시각, room identity, nullable user 같은 감사/복구 정보가 있어야
 * refresh와 write-behind 검증을 안정적으로 수행할 수 있습니다.
 */
struct Message {
    std::uint64_t id{};
    std::string room_id;
    std::string room_name;
    std::optional<std::string> user_id;
    std::optional<std::string> user_name;
    std::string content;
    std::int64_t created_at_ms{};
};

/**
 * @brief 사용자-방 관계와 읽음 위치를 함께 표현하는 멤버십 레코드입니다.
 *
 * 입장 여부와 last_seen을 한 계약에 묶는 이유는 둘 다 "사용자가 방을 어떻게 소비했는가"를
 * 설명하는 저장소 사실이기 때문입니다. 이를 별도 임시 구조로 나누면
 * join/leave와 refresh가 서로 다른 기준 시점을 읽게 될 수 있습니다.
 */
struct Membership {
    std::string user_id;
    std::string room_id;
    std::string role;
    std::int64_t joined_at_ms{};
    std::optional<std::uint64_t> last_seen_msg_id;
    std::optional<std::int64_t> left_at_ms;
    bool is_member{true};
};

/**
 * @brief 토큰 기반 인증 세션의 영속 레코드입니다.
 *
 * 네트워크 연결 자체는 휘발성이지만, 인증 세션은 폐기 여부와 만료 시각을 포함해
 * 더 오래 남아야 하는 운영 사실입니다. 이 구분이 없으면 "연결은 끊겼지만 토큰은 아직 유효한"
 * 상태를 표현하기 어렵습니다.
 */
struct Session {
    std::string id;
    std::string user_id;
    std::string token_hash;
    std::optional<std::string> client_ip;
    std::optional<std::string> user_agent;
    std::int64_t created_at_ms{};
    std::int64_t expires_at_ms{};
    std::optional<std::int64_t> revoked_at_ms;
};

/**
 * @brief 사용자 조회와 게스트 생성을 담당하는 저장소 계약입니다.
 *
 * 이 인터페이스는 범용 user domain이 아니라 chat app이 실제로 필요한 최소 표면만 노출합니다.
 * 너무 많은 메서드를 미리 넣으면 구현마다 부분 지원이 생기고,
 * 반대로 app-local 규칙을 `server_core`까지 끌어올리면 재사용 경계가 흐려집니다.
 */
class IUserRepository {
public:
    virtual ~IUserRepository() = default;

    virtual std::optional<User> find_by_id(const std::string& user_id) = 0;
    virtual std::vector<User> find_by_name_ci(const std::string& name, std::size_t limit) = 0;
    virtual User create_guest(const std::string& name) = 0;
    virtual void update_last_login(const std::string& user_id, const std::string& ip) = 0;
};

/**
 * @brief 방 검색, 생성, 종료를 담당하는 저장소 계약입니다.
 *
 * create/close를 명시적으로 분리해 두면 "같은 이름 방 재생성"과 "이전 방 폐쇄"를
 * 다른 감사 이벤트로 다룰 수 있습니다. 이를 upsert 하나로 뭉개면
 * 방 수명주기 분석과 개인정보 분리가 어려워집니다.
 */
class IRoomRepository {
public:
    virtual ~IRoomRepository() = default;

    virtual std::optional<Room> find_by_id(const std::string& room_id) = 0;
    virtual std::vector<Room> search_by_name_ci(const std::string& query, std::size_t limit) = 0;
    virtual std::optional<Room> find_by_name_exact_ci(const std::string& name) = 0;
    virtual Room create(const std::string& name, bool is_public) = 0;
    virtual void close(const std::string& room_id) = 0;
};

/**
 * @brief 방 메시지 조회, 추가, 정리를 담당하는 저장소 계약입니다.
 *
 * 메시지 저장소를 membership이나 room 저장소와 분리하는 이유는
 * 쓰기량과 조회 패턴이 전혀 다르기 때문입니다.
 * 분리하지 않으면 최근 메시지 조회 최적화가 room/멤버십 스키마까지 끌고 흔들리게 됩니다.
 */
class IMessageRepository {
public:
    virtual ~IMessageRepository() = default;

    virtual std::vector<Message> fetch_recent_by_room(const std::string& room_id,
                                                      std::uint64_t since_id,
                                                      std::size_t limit) = 0;
    virtual Message create(const std::string& room_id,
                           const std::string& room_name,
                           const std::optional<std::string>& user_id,
                           const std::string& content) = 0;
    virtual std::uint64_t get_last_id(const std::string& room_id) = 0;
    virtual void delete_by_room(const std::string& room_id) = 0;
};

/**
 * @brief 입장/퇴장과 읽음 위치 갱신을 담당하는 저장소 계약입니다.
 *
 * join/leave와 last_seen이 같은 저장소 경계에 있는 이유는,
 * 둘 다 membership row를 기준으로 설명되는 사실이기 때문입니다.
 * 서로 다른 저장소로 나누면 같은 사용자의 방 소비 상태를 원자적으로 갱신하기 어려워집니다.
 */
class IMembershipRepository {
public:
    virtual ~IMembershipRepository() = default;

    virtual void upsert_join(const std::string& user_id,
                             const std::string& room_id,
                             const std::string& role) = 0;
    virtual void update_last_seen(const std::string& user_id,
                                  const std::string& room_id,
                                  std::uint64_t last_seen_msg_id) = 0;
    virtual void leave(const std::string& user_id, const std::string& room_id) = 0;
    virtual std::optional<std::uint64_t> get_last_seen(const std::string& user_id,
                                                       const std::string& room_id) = 0;
};

/**
 * @brief 세션 발급, 조회, 폐기를 담당하는 저장소 계약입니다.
 *
 * revoke를 별도 연산으로 두는 이유는 "즉시 폐기"와 "자연 만료"를 구분하기 위해서입니다.
 * 운영 감사나 보안 분석에서 이 둘을 구분하지 못하면 강제 로그아웃 원인을 추적하기 어렵습니다.
 */
class ISessionRepository {
public:
    virtual ~ISessionRepository() = default;

    virtual std::optional<Session> find_by_token_hash(const std::string& token_hash) = 0;
    virtual Session create(const std::string& user_id,
                           const std::chrono::system_clock::time_point& expires_at,
                           const std::optional<std::string>& client_ip,
                           const std::optional<std::string>& user_agent,
                           const std::string& token_hash) = 0;
    virtual void revoke(const std::string& session_id) = 0;
};

} // namespace server::storage
