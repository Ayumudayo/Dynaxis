#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/asio.hpp>

#include "server/core/net/session.hpp"
#include "server/core/protocol/opcodes.hpp"
#include "wire.pb.h"

namespace server::core { class JobQueue; }
namespace server::core::storage { class IConnectionPool; }
namespace server::storage::redis { class IRedisClient; }

namespace server::app::chat {

class ChatService {
public:
    ChatService(boost::asio::io_context& io,
                server::core::JobQueue& job_queue,
                std::shared_ptr<server::core::storage::IConnectionPool> db_pool = {},
                std::shared_ptr<server::storage::redis::IRedisClient> redis = {});

    void on_login(server::core::Session& s, std::span<const std::uint8_t> payload);
    void on_join(server::core::Session& s, std::span<const std::uint8_t> payload);
    void on_leave(server::core::Session& s, std::span<const std::uint8_t> payload);
    void on_chat_send(server::core::Session& s, std::span<const std::uint8_t> payload);
    void on_whisper(server::core::Session& s, std::span<const std::uint8_t> payload);
    void on_ping(server::core::Session& s, std::span<const std::uint8_t> payload);
    void on_session_close(std::shared_ptr<server::core::Session> s);

    // 외부(fanout 등)에서 재사용할 룸 브로드캐스트 helper
    void broadcast_room(const std::string& room, const std::vector<std::uint8_t>& body, server::core::Session* self = nullptr);

private:
    using Session = server::core::Session;
    using WeakSession = std::weak_ptr<Session>;
    using WeakLess = std::owner_less<WeakSession>;
    using RoomSet = std::set<WeakSession, WeakLess>;

    using Exec = boost::asio::io_context::executor_type;
    using Strand = boost::asio::strand<Exec>;

    struct WriteBehindConfig {
        bool enabled{false};
        std::string stream_key{"session_events"};
        std::optional<std::size_t> maxlen{};
        bool approximate{true};
    };

    struct PresenceConfig {
        unsigned int ttl{30};
        std::string prefix;
    };

    struct State {
        std::mutex mu;
        std::unordered_map<std::string, RoomSet> rooms;
        std::unordered_map<Session*, std::string> user;          // 세션별 사용자 닉네임
        std::unordered_map<Session*, std::string> user_uuid;     // 세션별 사용자 UUID
        std::unordered_map<Session*, std::string> session_uuid;  // 세션별 세션 UUID(v4)
        std::unordered_map<Session*, std::string> cur_room;      // 세션별 현재 방 이름
        std::unordered_set<Session*> authed;                     // 인증 완료 세션
        std::unordered_set<Session*> guest;                      // 게스트 모드 세션
        std::unordered_map<std::string, RoomSet> by_user;        // 닉네임 -> 세션 목록
        std::unordered_map<std::string, std::string> room_ids;   // 방 이름 -> room_id(UUID)
        std::unordered_map<std::string, std::string> room_passwords;
    } state_;

    boost::asio::io_context* io_{};
    server::core::JobQueue& job_queue_;
    std::shared_ptr<server::core::storage::IConnectionPool> db_pool_{};
    std::shared_ptr<server::storage::redis::IRedisClient> redis_{};
    std::string gateway_id_{"gw-default"};
    std::unordered_map<std::string, std::shared_ptr<Strand>> room_strands_;
    Strand& strand_for(const std::string& room);

    WriteBehindConfig write_behind_;
    PresenceConfig presence_{};
    struct HistoryConfig {
        std::size_t recent_limit{20};
        std::size_t max_list_len{200};
        std::size_t fetch_factor{3};
        unsigned int cache_ttl_sec{6 * 60 * 60};
    } history_;

    bool write_behind_enabled() const;
    std::string generate_uuid_v4();
    std::string get_or_create_session_uuid(Session& s);
    void emit_write_behind_event(const std::string& type,
                                 const std::string& session_id,
                                 const std::optional<std::string>& user_id,
                                 const std::optional<std::string>& room_id,
                                 std::vector<std::pair<std::string, std::string>> extra_fields = {});

    // 내부 유틸리티
    std::string ensure_unique_or_error(Session& s, const std::string& desired);
    std::string gen_temp_name_uuid8();
    void send_room_users(Session& s, const std::string& room);
    void send_rooms_list(Session& s);
    void send_snapshot(Session& s, const std::string& current);
    void dispatch_whisper(std::shared_ptr<Session> sender, const std::string& target_user, const std::string& text);
    void send_system_notice(Session& s, const std::string& text);
    std::string hash_room_password(const std::string& password);
    void send_whisper_result(Session& s, bool ok, const std::string& reason);
    std::string ensure_room_id_ci(const std::string& room_name);
    std::string make_recent_list_key(const std::string& room_id) const;
    std::string make_recent_message_key(std::uint64_t message_id) const;
    bool cache_recent_message(const std::string& room_id,
                              const server::wire::v1::StateSnapshot::SnapshotMessage& message);
    bool load_recent_messages_from_cache(const std::string& room_id,
                                         std::vector<server::wire::v1::StateSnapshot::SnapshotMessage>& out);

    friend struct ChatServiceHistoryTester;

    static void collect_room_sessions(RoomSet& set, std::vector<std::shared_ptr<Session>>& out);
    unsigned int presence_ttl() const;
    std::string make_presence_key(std::string_view category, const std::string& id) const;
    void touch_user_presence(const std::string& uid);
};

} // namespace server::app::chat
