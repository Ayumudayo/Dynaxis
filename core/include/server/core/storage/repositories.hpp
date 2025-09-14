#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <chrono>

namespace server::core::storage {

// 도메인 DTO(저장소 경계 전용)
struct User {
    std::string id;          // UUID (text)
    std::string name;        // 중복 허용 라벨
    std::int64_t created_at_ms{}; // UTC epoch millis(표기 편의를 위한 변환 값)
};

struct Room {
    std::string id;          // UUID (text)
    std::string name;        // 라벨(중복 허용)
    bool        is_public{true};
    bool        is_active{true};
    std::optional<std::int64_t> closed_at_ms; // UTC epoch millis
    std::int64_t created_at_ms{};
};

struct Message {
    std::uint64_t id{};      // bigserial
    std::string   room_id;   // UUID (text)
    std::string   room_name; // 라벨(중복 허용) — denormalized for audit/UX
    std::optional<std::string> user_id; // NULL 허용
    std::string   content;
    std::int64_t  created_at_ms{}; // UTC epoch millis
};

struct Session {
    std::string id;          // UUID (text)
    std::string user_id;     // UUID (text)
    std::string token_hash;  // 저장소에는 해시만 저장
    std::optional<std::string> client_ip; // inet -> string 표현
    std::optional<std::string> user_agent;
    std::int64_t created_at_ms{};
    std::int64_t expires_at_ms{};
    std::optional<std::int64_t> revoked_at_ms;
};

// Repository 인터페이스
class IUserRepository {
public:
    virtual ~IUserRepository() = default;
    virtual std::optional<User> find_by_id(const std::string& user_id) = 0;
    virtual std::vector<User> find_by_name_ci(const std::string& name, std::size_t limit) = 0;
    virtual User create_guest(const std::string& name) = 0;
};

class IRoomRepository {
public:
    virtual ~IRoomRepository() = default;
    virtual std::optional<Room> find_by_id(const std::string& room_id) = 0;
    virtual std::vector<Room> search_by_name_ci(const std::string& query, std::size_t limit) = 0;
    virtual std::optional<Room> find_by_name_exact_ci(const std::string& name) = 0;
    virtual Room create(const std::string& name, bool is_public) = 0;
};

class IMessageRepository {
public:
    virtual ~IMessageRepository() = default;
    // 워터마크(since_id) 초과분만 오름차순으로 최대 limit개 반환
    virtual std::vector<Message> fetch_recent_by_room(const std::string& room_id,
                                                      std::uint64_t since_id,
                                                      std::size_t limit) = 0;
    virtual Message create(const std::string& room_id,
                           const std::string& room_name,
                           const std::optional<std::string>& user_id,
                           const std::string& content) = 0;
};

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

} // namespace server::core::storage
