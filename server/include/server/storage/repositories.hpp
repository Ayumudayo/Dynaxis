#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace server::storage {

struct User {
    std::string id;
    std::string name;
    std::int64_t created_at_ms{};
};

struct Room {
    std::string id;
    std::string name;
    bool is_public{true};
    bool is_active{true};
    std::optional<std::int64_t> closed_at_ms;
    std::int64_t created_at_ms{};
};

struct Message {
    std::uint64_t id{};
    std::string room_id;
    std::string room_name;
    std::optional<std::string> user_id;
    std::optional<std::string> user_name;
    std::string content;
    std::int64_t created_at_ms{};
};

struct Membership {
    std::string user_id;
    std::string room_id;
    std::string role;
    std::int64_t joined_at_ms{};
    std::optional<std::uint64_t> last_seen_msg_id;
    std::optional<std::int64_t> left_at_ms;
    bool is_member{true};
};

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

class IUserRepository {
public:
    virtual ~IUserRepository() = default;

    virtual std::optional<User> find_by_id(const std::string& user_id) = 0;
    virtual std::vector<User> find_by_name_ci(const std::string& name, std::size_t limit) = 0;
    virtual User create_guest(const std::string& name) = 0;
    virtual void update_last_login(const std::string& user_id, const std::string& ip) = 0;
};

class IRoomRepository {
public:
    virtual ~IRoomRepository() = default;

    virtual std::optional<Room> find_by_id(const std::string& room_id) = 0;
    virtual std::vector<Room> search_by_name_ci(const std::string& query, std::size_t limit) = 0;
    virtual std::optional<Room> find_by_name_exact_ci(const std::string& name) = 0;
    virtual Room create(const std::string& name, bool is_public) = 0;
    virtual void close(const std::string& room_id) = 0;
};

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
