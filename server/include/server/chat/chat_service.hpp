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
 #include <unordered_map>

#include <boost/asio.hpp>

#include "server/core/session.hpp"
#include "server/core/protocol.hpp"

namespace server::app::chat {

class ChatService {
public:
    explicit ChatService(boost::asio::io_context& io);

    void on_login(server::core::Session& s, std::span<const std::uint8_t> payload);
    void on_join(server::core::Session& s, std::span<const std::uint8_t> payload);
    void on_leave(server::core::Session& s, std::span<const std::uint8_t> payload);
    void on_chat_send(server::core::Session& s, std::span<const std::uint8_t> payload);
    void on_session_close(server::core::Session& s);

private:
    using Session = server::core::Session;
    using WeakSession = std::weak_ptr<Session>;
    using WeakLess = std::owner_less<WeakSession>;
    using RoomSet = std::set<WeakSession, WeakLess>;

    using Exec = boost::asio::io_context::executor_type;
    using Strand = boost::asio::strand<Exec>;

    struct State {
        std::mutex mu;
        std::unordered_map<std::string, RoomSet> rooms;
        std::unordered_map<Session*, std::string> user;      // 세션별 사용자명
        std::unordered_map<Session*, std::string> cur_room;  // 세션별 현재 룸
        std::unordered_set<Session*> authed;                 // 로그인 완료 세션
        std::unordered_map<std::string, RoomSet> by_user;    // 사용자명→세션들
    } state_;

    boost::asio::io_context* io_{};
    std::unordered_map<std::string, std::shared_ptr<Strand>> room_strands_;
    Strand& strand_for(const std::string& room);

    // 내부 유틸
    std::string ensure_unique_or_error(Session& s, const std::string& desired);
    std::string gen_hex_name(Session& s);
    void broadcast_room(const std::string& room, const std::vector<std::uint8_t>& body, Session* self = nullptr);
    void send_room_users(Session& s, const std::string& room);
    void send_rooms_list(Session& s);
    void send_snapshot(Session& s, const std::string& current);
};

} // namespace server::app::chat
