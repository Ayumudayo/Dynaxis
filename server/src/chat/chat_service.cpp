// UTF-8, 한국어 주석
#include "server/chat/chat_service.hpp"
#include "server/core/protocol/frame.hpp"
#include "server/core/protocol_errors.hpp"
#include "server/core/protocol_flags.hpp"
#include "server/core/util/log.hpp"
#include "server/wire/v1/wire.pb.h"

using namespace server::core;
namespace proto = server::core::protocol;
namespace corelog = server::core::log;

namespace server::app::chat {

ChatService::ChatService() = default;

std::string ChatService::gen_hex_name(Session& s) {
    auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::uint64_t v = (static_cast<std::uint64_t>(s.session_id()) << 32) ^ static_cast<std::uint64_t>(now64);
    v &= 0xFFFFFFFFull; std::ostringstream oss; oss << std::hex; oss.width(8); oss.fill('0'); oss << v; return oss.str();
}

std::string ChatService::ensure_unique_or_error(Session& s, const std::string& desired) {
    std::lock_guard<std::mutex> lk(state_.mu);
    if (!desired.empty() && desired != "guest") {
        auto itset = state_.by_user.find(desired);
        if (itset != state_.by_user.end()) {
            bool taken = false;
            for (auto wit = itset->second.begin(); wit != itset->second.end(); ) {
                if (auto p = wit->lock()) { taken = true; break; }
                else { wit = itset->second.erase(wit); }
            }
            if (taken) {
                s.send_error(proto::errc::NAME_TAKEN, "name taken");
                return {};
            }
        }
        return desired;
    }
    // 임시 닉 생성
    for (int i=0;i<4;++i) {
        std::string cand = gen_hex_name(s);
        if (!state_.by_user.count(cand) || state_.by_user[cand].empty()) return cand;
    }
    return gen_hex_name(s);
}

void ChatService::on_login(Session& s, std::span<const std::uint8_t> payload) {
    auto sp = std::span<const std::uint8_t>(payload.data(), payload.size());
    std::string user, token;
    if (!proto::read_lp_utf8(sp, user) || !proto::read_lp_utf8(sp, token)) {
        s.send_error(proto::errc::INVALID_PAYLOAD, "bad login payload");
        return;
    }
    corelog::info("LOGIN_REQ 수신");
    std::string new_user = ensure_unique_or_error(s, user);
    if (new_user.empty()) return; // NAME_TAKEN 등 에러 이미 송신됨

    {
        std::lock_guard<std::mutex> lk(state_.mu);
        // 기존 이름 제거
        if (auto itold = state_.user.find(&s); itold != state_.user.end()) {
            auto itset = state_.by_user.find(itold->second);
            if (itset != state_.by_user.end()) {
                WeakSession w = s.shared_from_this(); itset->second.erase(w);
            }
        }
        state_.user[&s] = new_user;
        state_.by_user[new_user].insert(s.shared_from_this());
        state_.authed.insert(&s);
        // 기본 방 자동 입장: lobby
        std::string room = "lobby";
        state_.cur_room[&s] = room;
        state_.rooms[room].insert(s.shared_from_this());
    }
    server::wire::v1::LoginRes pb; pb.set_effective_user(new_user); pb.set_session_id(s.session_id()); pb.set_message("ok");
    std::string bytes; pb.SerializeToString(&bytes);
    std::vector<std::uint8_t> res(bytes.begin(), bytes.end());
    s.async_send(proto::MSG_LOGIN_RES, res, 0);
}

void ChatService::on_join(Session& s, std::span<const std::uint8_t> payload) {
    auto sp = std::span<const std::uint8_t>(payload.data(), payload.size());
    std::string room; if (!proto::read_lp_utf8(sp, room)) { s.send_error(proto::errc::INVALID_PAYLOAD, "bad join payload"); return; }
    if (room.empty()) room = "lobby";
    corelog::info(std::string("JOIN_ROOM: ") + room);

    std::vector<std::shared_ptr<Session>> targets;
    std::vector<std::uint8_t> body;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        if (!state_.authed.count(&s)) { s.send_error(proto::errc::UNAUTHORIZED, "unauthorized"); return; }
        // 기존 방에서 제거
        auto itold = state_.cur_room.find(&s);
        if (itold != state_.cur_room.end() && itold->second != room) {
            auto itroom = state_.rooms.find(itold->second);
            if (itroom != state_.rooms.end()) {
                WeakSession w = s.shared_from_this();
                itroom->second.erase(w);
                // 기존 방이 비면(lobby 제외) 제거
                for (auto wit = itroom->second.begin(); wit != itroom->second.end(); ) {
                    if (wit->expired()) wit = itroom->second.erase(wit); else ++wit;
                }
                if (itroom->second.empty() && itold->second != "lobby") {
                    state_.rooms.erase(itroom);
                }
            }
        }
        state_.cur_room[&s] = room;
        state_.rooms[room].insert(s.shared_from_this());
        // 입장 브로드캐스트(Protobuf)
        std::string sender; auto it2 = state_.user.find(&s); sender = (it2 != state_.user.end()) ? it2->second : std::string("guest");
        server::wire::v1::ChatBroadcast pb; pb.set_room(room); pb.set_sender("(system)"); pb.set_text(sender + " 님이 입장했습니다"); pb.set_sender_sid(0);
        {
            auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            pb.set_ts_ms(static_cast<std::uint64_t>(now64));
        }
        std::string bytes; pb.SerializeToString(&bytes); body.assign(bytes.begin(), bytes.end());
        // 타겟 수집
        auto it = state_.rooms.find(room);
        if (it != state_.rooms.end()) {
            auto& set = it->second;
            for (auto wit = set.begin(); wit != set.end(); ) { if (auto p = wit->lock()) { targets.emplace_back(std::move(p)); ++wit; } else { wit = set.erase(wit); } }
        }
    }
    for (auto& t : targets) t->async_send(proto::MSG_CHAT_BROADCAST, body, 0);
}

void ChatService::on_leave(Session& s, std::span<const std::uint8_t> payload) {
    std::string room;
    auto sp = std::span<const std::uint8_t>(payload.data(), payload.size());
    proto::read_lp_utf8(sp, room);
    std::vector<std::shared_ptr<Session>> targets;
    std::vector<std::uint8_t> body;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        if (!state_.authed.count(&s)) { s.send_error(proto::errc::UNAUTHORIZED, "unauthorized"); return; }
        auto itcr = state_.cur_room.find(&s);
        if (itcr == state_.cur_room.end()) { s.send_error(proto::errc::NO_ROOM, "no current room"); return; }
        if (!room.empty() && itcr->second != room) { s.send_error(proto::errc::ROOM_MISMATCH, "room mismatch"); return; }
        room = itcr->second;
        // 퇴장 방송
        auto itroom = state_.rooms.find(room);
        if (itroom != state_.rooms.end()) {
            WeakSession w = s.shared_from_this(); itroom->second.erase(w);
            std::string sender; auto it2 = state_.user.find(&s); sender = (it2 != state_.user.end()) ? it2->second : std::string("guest");
            proto::write_lp_utf8(body, room); proto::write_lp_utf8(body, std::string("(system)")); proto::write_lp_utf8(body, sender + " 님이 퇴장했습니다");
            { std::size_t off_sid = body.size(); body.resize(off_sid + 4); proto::write_be32(0, body.data() + off_sid); }
            auto itb = state_.rooms.find(room);
            if (itb != state_.rooms.end()) {
                auto& set = itb->second;
                for (auto wit = set.begin(); wit != set.end(); ) { if (auto p = wit->lock()) { targets.emplace_back(std::move(p)); ++wit; } else { wit = set.erase(wit); } }
                // 방이 비면(lobby 제외) 제거
                if (set.empty() && room != "lobby") state_.rooms.erase(itb);
            }
        }
        state_.cur_room[&s] = std::string("lobby");
        state_.rooms["lobby"].insert(s.shared_from_this());
    }
    for (auto& t : targets) { auto f = (t.get() == &s) ? proto::FLAG_SELF : 0; t->async_send(proto::MSG_CHAT_BROADCAST, body, f); }
    // 로비 입장 알림
    std::vector<std::shared_ptr<Session>> t2; std::vector<std::uint8_t> body2;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        std::string sender; auto it2 = state_.user.find(&s); sender = (it2 != state_.user.end()) ? it2->second : std::string("guest");
        proto::write_lp_utf8(body2, std::string("lobby")); proto::write_lp_utf8(body2, std::string("(system)")); proto::write_lp_utf8(body2, sender + " 님이 입장했습니다");
        { std::size_t off_sid = body2.size(); body2.resize(off_sid + 4); proto::write_be32(0, body2.data() + off_sid); }
        auto itb = state_.rooms.find("lobby"); if (itb != state_.rooms.end()) { auto& set = itb->second; for (auto wit = set.begin(); wit != set.end(); ) { if (auto p = wit->lock()) { t2.emplace_back(std::move(p)); ++wit; } else { wit = set.erase(wit); } } }
    }
    for (auto& t : t2) t->async_send(proto::MSG_CHAT_BROADCAST, body2, 0);
}

void ChatService::send_rooms_list(Session& s) {
    std::vector<std::uint8_t> body;
    std::string msg = "rooms:";
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        std::vector<std::string> to_remove;
        for (auto it = state_.rooms.begin(); it != state_.rooms.end(); ++it) {
            std::size_t alive = 0;
            for (auto wit = it->second.begin(); wit != it->second.end(); ) { if (auto p = wit->lock()) { ++alive; ++wit; } else { wit = it->second.erase(wit); } }
            if (alive == 0 && it->first != std::string("lobby")) { to_remove.push_back(it->first); continue; }
            msg += " " + it->first + "(" + std::to_string(alive) + ")";
        }
        for (auto& name : to_remove) state_.rooms.erase(name);
    }
    server::wire::v1::ChatBroadcast pb; pb.set_room("(system)"); pb.set_sender("server"); pb.set_text(msg); pb.set_sender_sid(0);
    {
        auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        pb.set_ts_ms(static_cast<std::uint64_t>(now64));
    }
    std::string bytes; pb.SerializeToString(&bytes); body.assign(bytes.begin(), bytes.end());
    s.async_send(proto::MSG_CHAT_BROADCAST, body, 0);
}

void ChatService::send_room_users(Session& s, const std::string& target) {
    std::vector<std::uint8_t> body;
    server::wire::v1::RoomUsers pb; pb.set_room(target);
    std::uint16_t n = 0;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        auto itroom = state_.rooms.find(target);
        if (itroom != state_.rooms.end()) {
            for (auto wit = itroom->second.begin(); wit != itroom->second.end(); ) {
                if (auto p = wit->lock()) {
                    auto itu = state_.user.find(p.get());
                    std::string name = (itu != state_.user.end()) ? itu->second : std::string("guest");
                    pb.add_users(name); ++n; ++wit;
                } else { wit = itroom->second.erase(wit); }
            }
        }
    }
    std::string bytes; pb.SerializeToString(&bytes); body.assign(bytes.begin(), bytes.end());
    s.async_send(proto::MSG_ROOM_USERS, body, 0);
}

void ChatService::send_snapshot(Session& s, const std::string& current) {
    std::vector<std::uint8_t> body;
    server::wire::v1::StateSnapshot pb; pb.set_current_room(current);
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        std::vector<std::string> to_remove;
        for (auto it = state_.rooms.begin(); it != state_.rooms.end(); ++it) {
            std::uint32_t alive = 0; for (auto wit = it->second.begin(); wit != it->second.end(); ) { if (auto p = wit->lock()) { ++alive; ++wit; } else { wit = it->second.erase(wit); } }
            if (alive == 0 && it->first != std::string("lobby")) { to_remove.push_back(it->first); continue; }
            auto* ri = pb.add_rooms(); ri->set_name(it->first); ri->set_members(alive);
        }
        for (auto& name : to_remove) state_.rooms.erase(name);
    }
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        auto itroom = state_.rooms.find(current);
        if (itroom != state_.rooms.end()) {
            for (auto wit = itroom->second.begin(); wit != itroom->second.end(); ) { if (auto p = wit->lock()) { auto itu = state_.user.find(p.get()); std::string name = (itu != state_.user.end()) ? itu->second : std::string("guest"); pb.add_users(name); ++wit; } else { wit = itroom->second.erase(wit); } }
        }
    }
    std::string bytes; pb.SerializeToString(&bytes); body.assign(bytes.begin(), bytes.end());
    s.async_send(proto::MSG_STATE_SNAPSHOT, body, 0);
}

void ChatService::on_chat_send(Session& s, std::span<const std::uint8_t> payload) {
    std::string room, text; auto sp = std::span<const std::uint8_t>(payload.data(), payload.size());
    if (!proto::read_lp_utf8(sp, room) || !proto::read_lp_utf8(sp, text)) { s.send_error(proto::errc::INVALID_PAYLOAD, "bad chat payload"); return; }
    corelog::info(std::string("CHAT_SEND: room=") + (room.empty()?"(empty)":room) + ", text=" + text);
    // /refresh는 인증 없이도 허용(스냅샷 반환)
    if (text == "/refresh") {
        std::string current;
        {
            std::lock_guard<std::mutex> lk(state_.mu); auto itcr = state_.cur_room.find(&s); current = (itcr != state_.cur_room.end()) ? itcr->second : std::string("lobby");
        }
        send_snapshot(s, current); return;
    }
    // 인증 체크
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        if (!state_.authed.count(&s)) { s.send_error(proto::errc::UNAUTHORIZED, "unauthorized"); return; }
        if (room.empty()) {
            auto it = state_.cur_room.find(&s); if (it == state_.cur_room.end()) { s.send_error(proto::errc::NO_ROOM, "no current room"); return; } room = it->second;
        }
    }
    // 슬래시 명령
    if (!text.empty() && text[0] == '/') {
        if (text == "/rooms") { send_rooms_list(s); return; }
        if (text.rfind("/who", 0) == 0) {
            std::string target = room; if (text.size() > 4) { auto pos = text.find_first_not_of(' ', 4); if (pos != std::string::npos) target = text.substr(pos); }
            send_room_users(s, target); return;
        }
        if (text.rfind("/whisper ", 0) == 0) {
            std::string rest = text.substr(9); auto spc = rest.find(' ');
            if (spc == std::string::npos) {
                std::vector<std::uint8_t> body; proto::write_lp_utf8(body, std::string("(system)")); proto::write_lp_utf8(body, std::string("server")); proto::write_lp_utf8(body, std::string("usage: /whisper <user> <message>"));
                auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); std::size_t off = body.size(); body.resize(off + 8); std::uint64_t ts = static_cast<std::uint64_t>(now64); for (int i = 7; i >= 0; --i) { body[off + i] = static_cast<std::uint8_t>(ts & 0xFF); ts >>= 8; } s.async_send(proto::MSG_CHAT_BROADCAST, body, 0); return;
            }
            std::string target_user = rest.substr(0, spc); std::string wtext = rest.substr(spc + 1);
            std::vector<std::shared_ptr<Session>> targets;
            {
                std::lock_guard<std::mutex> lk(state_.mu); auto itset = state_.by_user.find(target_user); if (itset != state_.by_user.end()) { for (auto wit = itset->second.begin(); wit != itset->second.end(); ) { if (auto p = wit->lock()) { targets.emplace_back(std::move(p)); ++wit; } else { wit = itset->second.erase(wit); } } }
            }
            std::string sender; { std::lock_guard<std::mutex> lk(state_.mu); auto it2 = state_.user.find(&s); sender = (it2 != state_.user.end()) ? it2->second : std::string("guest"); }
            server::wire::v1::ChatBroadcast pb; pb.set_room("(direct)"); pb.set_sender(sender); pb.set_text(std::string("(to ") + target_user + ") " + wtext); pb.set_sender_sid(s.session_id());
            auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); pb.set_ts_ms(static_cast<std::uint64_t>(now64));
            std::string bytes; pb.SerializeToString(&bytes); std::vector<std::uint8_t> body(bytes.begin(), bytes.end());
            targets.emplace_back(s.shared_from_this()); for (auto& t : targets) t->async_send(proto::MSG_CHAT_BROADCAST, body, 0); return;
        }
    }
    // 일반 채팅: 방별 strand에 포스트하여 순차 처리
    {
        auto self = s.shared_from_this();
        auto room_copy = room;
        auto text_copy = text;
        boost::asio::post(strand_for(room_copy), [this, self, room_copy, text_copy]() {
            std::vector<std::shared_ptr<Session>> targets;
            std::string sender;
            {
                std::lock_guard<std::mutex> lk(state_.mu);
                auto it2 = state_.user.find(self.get()); sender = (it2 != state_.user.end()) ? it2->second : std::string("guest");
                if (sender == "guest") { self->send_error(proto::errc::UNAUTHORIZED, "guest cannot chat"); return; }
                auto it = state_.rooms.find(room_copy); if (it != state_.rooms.end()) { auto& set = it->second; for (auto wit = set.begin(); wit != set.end(); ) { if (auto p = wit->lock()) { targets.emplace_back(std::move(p)); ++wit; } else { wit = set.erase(wit); } } }
            }
            server::wire::v1::ChatBroadcast pb; pb.set_room(room_copy); pb.set_sender(sender); pb.set_text(text_copy); pb.set_sender_sid(self->session_id());
            auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); pb.set_ts_ms(static_cast<std::uint64_t>(now64));
            std::vector<std::uint8_t> body = server::wire::codec::Encode(pb);
            if (targets.empty()) { self->async_send(proto::MSG_CHAT_BROADCAST, body, proto::FLAG_SELF); }
            else { for (auto& t : targets) { auto f = (t.get() == self.get()) ? proto::FLAG_SELF : 0; t->async_send(proto::MSG_CHAT_BROADCAST, body, f); } }
        });
    }
}

void ChatService::on_session_close(Session& s) {
    std::vector<std::shared_ptr<Session>> targets; std::vector<std::uint8_t> body; std::string name;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        if (auto itname = state_.user.find(&s); itname != state_.user.end()) name = itname->second; else name = "guest";
        state_.authed.erase(&s);
        if (!name.empty()) { auto itset = state_.by_user.find(name); if (itset != state_.by_user.end()) { WeakSession w = s.shared_from_this(); itset->second.erase(w); } }
        state_.user.erase(&s);
        auto itcr = state_.cur_room.find(&s);
        if (itcr != state_.cur_room.end()) {
            auto room = itcr->second; auto itroom = state_.rooms.find(room);
            if (itroom != state_.rooms.end()) {
                WeakSession w = s.shared_from_this();
                itroom->second.erase(w);
                server::wire::v1::ChatBroadcast pb; pb.set_room(room); pb.set_sender("(system)"); pb.set_text(name + " 님이 퇴장했습니다"); pb.set_sender_sid(0);
                auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); pb.set_ts_ms(static_cast<std::uint64_t>(now64));
                std::string bytes; pb.SerializeToString(&bytes); body.assign(bytes.begin(), bytes.end());
                auto itb = state_.rooms.find(room);
                if (itb != state_.rooms.end()) {
                    auto& set = itb->second;
                    for (auto wit = set.begin(); wit != set.end(); ) { if (auto p = wit->lock()) { targets.emplace_back(std::move(p)); ++wit; } else { wit = set.erase(wit); } }
                    if (set.empty() && room != std::string("lobby")) state_.rooms.erase(itb);
                }
            }
            state_.cur_room.erase(itcr);
        }
    }
    for (auto& t : targets) { auto f = (t.get() == &s) ? proto::FLAG_SELF : 0; t->async_send(proto::MSG_CHAT_BROADCAST, body, f); }
}

} // namespace server::app::chat
