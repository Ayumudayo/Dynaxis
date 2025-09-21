// UTF-8, 한국어 주석
#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <mutex>
#include <memory>
#include <span>
#include <optional>
#include <unordered_map>

#include <boost/asio.hpp>

#include "server/core/net/session.hpp"
// Opcodes are defined in generated header
#include "server/core/protocol/opcodes.hpp"

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
    void on_ping(server::core::Session& s, std::span<const std::uint8_t> payload);
    void on_session_close(std::shared_ptr<server::core::Session> s);

    // 외부(분산 브로드캐스트 등)에서 준비된 브로드캐스트 payload를 룸에 전파
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

    struct State {
        std::mutex mu;
        std::unordered_map<std::string, RoomSet> rooms;
        std::unordered_map<Session*, std::string> user;      // 세션별 사용자명
        std::unordered_map<Session*, std::string> user_uuid;  // 세션별 사용자 UUID
        std::unordered_map<Session*, std::string> session_uuid; // 세션별 세션 UUID(v4)
        std::unordered_map<Session*, std::string> cur_room;  // 세션별 현재 룸
        std::unordered_set<Session*> authed;                 // 로그인 완료 세션
        std::unordered_map<std::string, RoomSet> by_user;    // 사용자명→세션들
        std::unordered_map<std::string, std::string> room_ids; // 룸 이름 -> room_id(UUID)
    } state_;

    boost::asio::io_context* io_{};
    server::core::JobQueue& job_queue_;
    std::shared_ptr<server::core::storage::IConnectionPool> db_pool_{}; // 선택 주입
    std::shared_ptr<server::storage::redis::IRedisClient> redis_{};      // 선택 주입
    std::string gateway_id_{"gw-default"};
    std::unordered_map<std::string, std::shared_ptr<Strand>> room_strands_;
    Strand& strand_for(const std::string& room);

    WriteBehindConfig write_behind_;

    bool write_behind_enabled() const;
    std::string generate_uuid_v4();
    std::string get_or_create_session_uuid(Session& s);
    void emit_write_behind_event(const std::string& type,
                                 const std::string& session_id,
                                 const std::optional<std::string>& user_id,
                                 const std::optional<std::string>& room_id,
                                 std::vector<std::pair<std::string, std::string>> extra_fields = {});

    // 내부 유틸
    std::string ensure_unique_or_error(Session& s, const std::string& desired);
    std::string gen_temp_name_uuid8();
    void send_room_users(Session& s, const std::string& room);
    void send_rooms_list(Session& s);
    void send_snapshot(Session& s, const std::string& current);

    // 저장소 보조: 룸 이름으로 UUID 확보(없으면 생성)
    std::string ensure_room_id_ci(const std::string& room_name);
};

} // namespace server::app::chat
