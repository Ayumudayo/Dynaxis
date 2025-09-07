#include <iostream>
#include <thread>
#include <vector>
#include <string>
#include <span>
#include <cstdint>
#include <algorithm>

#include <boost/asio.hpp>
#include <clocale>
#if defined(_WIN32)
#  include <windows.h>
#endif
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <mutex>
#include "server/core/protocol/frame.hpp"

#include "server/core/acceptor.hpp"
#include "server/core/dispatcher.hpp"
#include "server/core/session.hpp"
#include "server/core/protocol.hpp"
// flags
#include "server/core/protocol_flags.hpp"
// 에러 코드
#include "server/core/protocol_errors.hpp"
// 고정 헤더에 UTC/SEQ가 항상 포함되므로 별도 플래그 불필요
#include "server/core/util/log.hpp"
#include "server/core/options.hpp"
#include "server/core/shared_state.hpp"

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using server::core::Dispatcher;
using server::core::Acceptor;
using server::core::Session;
namespace protocol = server::core::protocol;
namespace corelog = server::core::log;

int main(int argc, char** argv) {
    try {
#if defined(_WIN32)
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        std::setlocale(LC_ALL, ".UTF-8");
#endif
        unsigned short port = 5000;
        if (argc >= 2) {
            port = static_cast<unsigned short>(std::stoi(argv[1]));
        }

        asio::io_context io;
        Dispatcher dispatcher;
        auto options = std::make_shared<server::core::SessionOptions>();
        options->read_timeout_ms = 60'000;     // 개발 편의: 타임아웃 여유
        options->heartbeat_interval_ms = 10'000;
        auto state   = std::make_shared<server::core::SharedState>();

        struct ChatState {
            std::mutex mu;
            using WeakSession = std::weak_ptr<Session>;
            using WeakLess = std::owner_less<WeakSession>;
            using RoomSet = std::set<WeakSession, WeakLess>;
            std::unordered_map<std::string, RoomSet> rooms;
            std::unordered_map<Session*, std::string> user;      // 세션별 사용자명
            std::unordered_map<Session*, std::string> cur_room;  // 세션별 현재 룸
            std::unordered_set<Session*> authed;                 // 로그인 완료 세션
            // 사용자명 기준 세션 목록(여러 세션 동시 접속 지원)
            std::unordered_map<std::string, RoomSet> by_user;
        } chat;

        // 핸들러 등록: PING -> PONG, CHAT_SEND -> CHAT_BROADCAST(자기 자신에게 에코)
        dispatcher.register_handler(protocol::MSG_PING,
            [](Session& s, std::span<const std::uint8_t> payload) {
                std::vector<std::uint8_t> body(payload.begin(), payload.end());
                s.async_send(protocol::MSG_PONG, body, 0);
            });

        dispatcher.register_handler(protocol::MSG_LOGIN_REQ,
            [&chat](Session& s, std::span<const std::uint8_t> payload) {
                corelog::info("LOGIN_REQ 수신");
                auto sp = std::span<const std::uint8_t>(payload.data(), payload.size());
                std::string user, token;
                if (!server::core::protocol::read_lp_utf8(sp, user) || !server::core::protocol::read_lp_utf8(sp, token)) {
                    s.send_error(protocol::errc::INVALID_PAYLOAD, "bad login payload");
                    return;
                }
                {
                    std::lock_guard<std::mutex> lk(chat.mu);
                    auto gen_hex = [&](){
                        auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        std::uint64_t v = (static_cast<std::uint64_t>(s.session_id()) << 32) ^ static_cast<std::uint64_t>(now64);
                        v &= 0xFFFFFFFFull; std::ostringstream oss; oss << std::hex; oss.width(8); oss.fill('0'); oss << v; return oss.str();
                    };
                    std::string new_user;
                    if (!user.empty() && user != "guest") {
                        // 유일 닉 정책: 이미 존재하면 거부
                        auto itset = chat.by_user.find(user);
                        if (itset != chat.by_user.end()) {
                            // live check
                            bool taken = false;
                            for (auto wit = itset->second.begin(); wit != itset->second.end(); ) {
                                if (auto p = wit->lock()) { taken = true; break; }
                                else { wit = itset->second.erase(wit); }
                            }
                            if (taken) { s.send_error(protocol::errc::NAME_TAKEN, "name taken"); return; }
                        }
                        new_user = user;
                    } else {
                        // 임시 닉 생성(충돌 회피)
                        for (int i=0;i<4;++i){
                            std::string cand = gen_hex();
                            if (!chat.by_user.count(cand) || chat.by_user[cand].empty()) { new_user = std::move(cand); break; }
                        }
                        if (new_user.empty()) new_user = gen_hex();
                    }
                    // 기존 사용자명이 있다면 사용자 맵에서 제거
                    if (auto itold = chat.user.find(&s); itold != chat.user.end()) {
                        auto itset = chat.by_user.find(itold->second);
                        if (itset != chat.by_user.end()) {
                            ChatState::WeakSession w = s.shared_from_this();
                            itset->second.erase(w);
                        }
                    }
                    chat.user[&s] = new_user;
                    chat.by_user[new_user].insert(s.shared_from_this());
                    chat.authed.insert(&s);
                    // 기본 방 자동 입장: lobby
                    std::string room = "lobby";
                    chat.cur_room[&s] = room;
                    auto& set = chat.rooms[room];
                    set.insert(s.shared_from_this());
                }
                std::vector<std::uint8_t> res;
                res.reserve(4 + 64);
                res.push_back(0); // status ok
                server::core::protocol::write_lp_utf8(res, "ok");
                // effective username
                {
                    std::lock_guard<std::mutex> lk(chat.mu);
                    auto it = chat.user.find(&s);
                    server::core::protocol::write_lp_utf8(res, (it!=chat.user.end()?it->second:std::string("guest")));
                }
                // 세션 ID 포함(u32)
                std::size_t off = res.size(); res.resize(off + 4);
                server::core::protocol::write_be32(s.session_id(), res.data() + off);
                s.async_send(protocol::MSG_LOGIN_RES, res, 0);
            });

        dispatcher.register_handler(protocol::MSG_JOIN_ROOM,
            [&chat](Session& s, std::span<const std::uint8_t> payload) {
                auto sp = std::span<const std::uint8_t>(payload.data(), payload.size());
                std::string room;
                if (!server::core::protocol::read_lp_utf8(sp, room)) { s.send_error(protocol::errc::INVALID_PAYLOAD, "bad join payload"); return; }
                if (room.empty()) room = "lobby";
                corelog::info(std::string("JOIN_ROOM: ") + room);
                std::vector<std::shared_ptr<Session>> targets;
                std::vector<std::uint8_t> body;
                {
                    std::lock_guard<std::mutex> lk(chat.mu);
                if (!chat.authed.count(&s)) { s.send_error(protocol::errc::UNAUTHORIZED, "unauthorized"); return; }
                    // 기존 방에서 현재 세션만 정확히 제거
                    auto itold = chat.cur_room.find(&s);
                    if (itold != chat.cur_room.end() && itold->second != room) {
                        auto itroom = chat.rooms.find(itold->second);
                        if (itroom != chat.rooms.end()) {
                            ChatState::WeakSession w = s.shared_from_this();
                            itroom->second.erase(w);
                        }
                    }
                    chat.cur_room[&s] = room;
                    chat.rooms[room].insert(s.shared_from_this());
                    // 입장 브로드캐스트(해당 방 전체): room, (system), text, u32 sender_sid(0), u64 ts
                    std::string sender;
                    auto it2 = chat.user.find(&s);
                    sender = (it2 != chat.user.end()) ? it2->second : std::string("guest");
                    server::core::protocol::write_lp_utf8(body, room);
                    server::core::protocol::write_lp_utf8(body, std::string("(system)"));
                    server::core::protocol::write_lp_utf8(body, sender + " 님이 입장했습니다");
                    { std::size_t off_sid = body.size(); body.resize(off_sid + 4); server::core::protocol::write_be32(0, body.data() + off_sid); }
                    auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    std::size_t off = body.size(); body.resize(off + 8);
                    std::uint64_t ts = static_cast<std::uint64_t>(now64);
                    for (int i = 7; i >= 0; --i) { body[off + i] = static_cast<std::uint8_t>(ts & 0xFF); ts >>= 8; }
                    // 대상 세션 수집
                    auto it = chat.rooms.find(room);
                    if (it != chat.rooms.end()) {
                        auto& set = it->second;
                        for (auto wit = set.begin(); wit != set.end(); ) {
                            if (auto p = wit->lock()) { targets.emplace_back(std::move(p)); ++wit; }
                            else { wit = set.erase(wit); }
                        }
                    }
                }
                for (auto& t : targets) t->async_send(protocol::MSG_CHAT_BROADCAST, body, 0);
            });

        dispatcher.register_handler(protocol::MSG_CHAT_SEND,
            [&chat](Session& s, std::span<const std::uint8_t> payload) {
                std::string room, text;
                auto sp = std::span<const std::uint8_t>(payload.data(), payload.size());
                if (!server::core::protocol::read_lp_utf8(sp, room) || !server::core::protocol::read_lp_utf8(sp, text)) {
                    s.send_error(protocol::errc::INVALID_PAYLOAD, "bad chat payload");
                    return;
                }
                std::string sender;
                {
                    std::lock_guard<std::mutex> lk(chat.mu);
                    corelog::info(std::string("CHAT_SEND: room=") + (room.empty()?"(empty)":room) + ", text=" + text);
                    // /refresh는 인증 없이 허용(스냅샷만 반환)
                    if (text == "/refresh") {
                        std::string current;
                        auto itcr = chat.cur_room.find(&s);
                        current = (itcr != chat.cur_room.end()) ? itcr->second : std::string("lobby");
                        std::vector<std::uint8_t> body;
                        server::core::protocol::write_lp_utf8(body, current);
                        // rooms 목록 + 인원
                        std::uint16_t rooms_count = 0;
                        std::size_t rooms_count_off = body.size();
                        body.resize(body.size() + 2);
                        for (auto& kv : chat.rooms) {
                            std::uint16_t alive = 0;
                            for (auto wit = kv.second.begin(); wit != kv.second.end(); ) {
                                if (auto p = wit->lock()) { ++alive; ++wit; }
                                else { wit = kv.second.erase(wit); }
                            }
                            server::core::protocol::write_lp_utf8(body, kv.first);
                            std::size_t off = body.size(); body.resize(off + 2);
                            server::core::protocol::write_be16(alive, body.data() + off);
                            ++rooms_count;
                        }
                        server::core::protocol::write_be16(rooms_count, body.data() + rooms_count_off);
                        // current 방 유저 목록
                        std::uint16_t users_count = 0;
                        std::size_t users_count_off = body.size();
                        body.resize(body.size() + 2);
                        auto itroom = chat.rooms.find(current);
                        if (itroom != chat.rooms.end()) {
                            for (auto wit = itroom->second.begin(); wit != itroom->second.end(); ) {
                                if (auto p = wit->lock()) {
                                    auto itu = chat.user.find(p.get());
                                    std::string name = (itu != chat.user.end()) ? itu->second : std::string("guest");
                                    server::core::protocol::write_lp_utf8(body, name);
                                    ++users_count;
                                    ++wit;
                                } else { wit = itroom->second.erase(wit); }
                            }
                        }
                        server::core::protocol::write_be16(users_count, body.data() + users_count_off);
                        s.async_send(protocol::MSG_STATE_SNAPSHOT, body, 0);
                        return;
                    }
                    if (!chat.authed.count(&s)) {
                        // 미인증: 거부
                        s.send_error(protocol::errc::UNAUTHORIZED, "unauthorized");
                        return;
                    }
                    if (room.empty()) {
                        auto it = chat.cur_room.find(&s);
                        if (it == chat.cur_room.end()) {
                            // 현재 방 없음 → 거부
                            s.send_error(protocol::errc::NO_ROOM, "no current room");
                            return;
                        }
                        room = it->second;
                    }
                    // 슬래시 명령 처리: /rooms, /who [room], /whisper <user> <msg>
                    if (!text.empty() && text[0] == '/') {
                        if (text == "/rooms") {
                            // 방 목록
                            std::string msg = "rooms:";
                            for (auto& kv : chat.rooms) {
                                // 생존 세션 수 계산
                                std::size_t alive = 0;
                                for (auto wit = kv.second.begin(); wit != kv.second.end(); ) {
                                    if (auto p = wit->lock()) { ++alive; ++wit; }
                                    else { wit = kv.second.erase(wit); }
                                }
                                msg += " " + kv.first + "(" + std::to_string(alive) + ")";
                            }
                            std::vector<std::uint8_t> body;
                            server::core::protocol::write_lp_utf8(body, std::string("(system)"));
                            server::core::protocol::write_lp_utf8(body, std::string("server"));
                            server::core::protocol::write_lp_utf8(body, msg);
                            auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                            std::size_t off = body.size(); body.resize(off + 8);
                            std::uint64_t ts = static_cast<std::uint64_t>(now64);
                            for (int i = 7; i >= 0; --i) { body[off + i] = static_cast<std::uint8_t>(ts & 0xFF); ts >>= 8; }
                            s.async_send(protocol::MSG_CHAT_BROADCAST, body, 0);
                            return;
                        } else if (text.rfind("/who", 0) == 0) {
                            // 구문: /who [room]
                            std::string target = room;
                            if (text.size() > 4) {
                                auto pos = text.find_first_not_of(' ', 4);
                                if (pos != std::string::npos) target = text.substr(pos);
                            }
                            std::vector<std::uint8_t> body;
                            server::core::protocol::write_lp_utf8(body, target);
                            std::uint16_t n = 0; std::size_t off_n = body.size(); body.resize(off_n + 2);
                            auto itroom = chat.rooms.find(target);
                            if (itroom != chat.rooms.end()) {
                                for (auto wit = itroom->second.begin(); wit != itroom->second.end(); ) {
                                    if (auto p = wit->lock()) {
                                        auto itu = chat.user.find(p.get());
                                        std::string name = (itu != chat.user.end()) ? itu->second : std::string("guest");
                                        server::core::protocol::write_lp_utf8(body, name);
                                        ++n; ++wit;
                                    } else { wit = itroom->second.erase(wit); }
                                }
                            }
                            server::core::protocol::write_be16(n, body.data() + off_n);
                            s.async_send(protocol::MSG_ROOM_USERS, body, 0);
                            return;
                        } else if (text.rfind("/whisper ", 0) == 0) {
                            // 구문: /whisper <user> <message...>
                            std::string rest = text.substr(9);
                            auto spc = rest.find(' ');
                            if (spc == std::string::npos) {
                                std::vector<std::uint8_t> body;
                                server::core::protocol::write_lp_utf8(body, std::string("(system)"));
                                server::core::protocol::write_lp_utf8(body, std::string("server"));
                                server::core::protocol::write_lp_utf8(body, std::string("usage: /whisper <user> <message>"));
                                auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch()).count();
                                std::size_t off = body.size(); body.resize(off + 8);
                                std::uint64_t ts = static_cast<std::uint64_t>(now64);
                                for (int i = 7; i >= 0; --i) { body[off + i] = static_cast<std::uint8_t>(ts & 0xFF); ts >>= 8; }
                                s.async_send(protocol::MSG_CHAT_BROADCAST, body, 0);
                                return;
                            }
                            std::string target_user = rest.substr(0, spc);
                            std::string wtext = rest.substr(spc + 1);
                            std::vector<std::shared_ptr<Session>> targets;
                            // 대상 사용자 세션 수집
                            {
                                auto itset = chat.by_user.find(target_user);
                                if (itset != chat.by_user.end()) {
                                    for (auto wit = itset->second.begin(); wit != itset->second.end(); ) {
                                        if (auto p = wit->lock()) { targets.emplace_back(std::move(p)); ++wit; }
                                        else { wit = itset->second.erase(wit); }
                                    }
                                }
                            }
                            // 보낸 사람 이름
                            auto it2 = chat.user.find(&s);
                            sender = (it2 != chat.user.end()) ? it2->second : std::string("guest");
                            // payload: room="(direct)", sender, text="(to <user>) <msg>", u32 sender_sid, ts
                            std::vector<std::uint8_t> body;
                            server::core::protocol::write_lp_utf8(body, std::string("(direct)"));
                            server::core::protocol::write_lp_utf8(body, sender);
                            server::core::protocol::write_lp_utf8(body, std::string("(to ") + target_user + ") " + wtext);
                            { std::size_t off_sid = body.size(); body.resize(off_sid + 4); server::core::protocol::write_be32(s.session_id(), body.data() + off_sid); }
                            auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                            std::size_t off = body.size(); body.resize(off + 8);
                            std::uint64_t ts = static_cast<std::uint64_t>(now64);
                            for (int i = 7; i >= 0; --i) { body[off + i] = static_cast<std::uint8_t>(ts & 0xFF); ts >>= 8; }
                            // 에코를 위해 발신자 추가
                            targets.emplace_back(s.shared_from_this());
                            if (targets.empty()) {
                                s.async_send(protocol::MSG_CHAT_BROADCAST, body, 0);
                            } else {
                                for (auto& t : targets) t->async_send(protocol::MSG_CHAT_BROADCAST, body, 0);
                            }
                            return;
                        }
                    }
                    // 방 멤버십 확인(현재 방과 불일치하면 거부)
                    auto itcr = chat.cur_room.find(&s);
                    if (itcr == chat.cur_room.end() || itcr->second != room) {
                        s.send_error(protocol::errc::NOT_MEMBER, "not a member of room");
                        return;
                    }
                    auto it2 = chat.user.find(&s);
                    sender = (it2 != chat.user.end()) ? it2->second : std::string("guest");
                    if (sender == "guest") {
                        s.send_error(protocol::errc::UNAUTHORIZED, "guest cannot chat");
                        return;
                    }
                }
                // 브로드캐스트 페이로드: room, sender, text, u32 sender_sid, u64 ts_ms
                std::vector<std::uint8_t> body;
                server::core::protocol::write_lp_utf8(body, room);
                server::core::protocol::write_lp_utf8(body, sender);
                server::core::protocol::write_lp_utf8(body, text);
                // sender session id
                {
                    std::size_t off_sid = body.size(); body.resize(off_sid + 4);
                    server::core::protocol::write_be32(s.session_id(), body.data() + off_sid);
                }
                auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                std::size_t off = body.size();
                body.resize(off + 8);
                std::uint64_t ts = static_cast<std::uint64_t>(now64);
                for (int i = 7; i >= 0; --i) { body[off + i] = static_cast<std::uint8_t>(ts & 0xFF); ts >>= 8; }

                // 대상 세션 수집 및 송신
                std::vector<std::shared_ptr<Session>> targets;
                {
                    std::lock_guard<std::mutex> lk(chat.mu);
                    auto it = chat.rooms.find(room);
                    if (it != chat.rooms.end()) {
                        auto& set = it->second;
                        for (auto wit = set.begin(); wit != set.end(); ) {
                            if (auto p = wit->lock()) { targets.emplace_back(std::move(p)); ++wit; }
                            else { wit = set.erase(wit); }
                        }
                    }
                }
                if (targets.empty()) {
                    // 구독자가 없다면 자기 자신에게만 에코(SELF 플래그)
                    s.async_send(protocol::MSG_CHAT_BROADCAST, body, server::core::protocol::FLAG_SELF);
                } else {
                    for (auto& t : targets) {
                        auto f = (t.get() == &s) ? server::core::protocol::FLAG_SELF : 0;
                        t->async_send(protocol::MSG_CHAT_BROADCAST, body, f);
                    }
                }
            });

        dispatcher.register_handler(protocol::MSG_LEAVE_ROOM,
            [&chat](Session& s, std::span<const std::uint8_t> payload) {
                // payload: [optional lp_utf8 room]
                std::string room;
                auto sp = std::span<const std::uint8_t>(payload.data(), payload.size());
                server::core::protocol::read_lp_utf8(sp, room);
                std::vector<std::shared_ptr<Session>> targets;
                std::vector<std::uint8_t> body;
                {
                    std::lock_guard<std::mutex> lk(chat.mu);
                if (!chat.authed.count(&s)) { s.send_error(protocol::errc::UNAUTHORIZED, "unauthorized"); return; }
                auto itcr = chat.cur_room.find(&s);
                if (itcr == chat.cur_room.end()) { s.send_error(protocol::errc::NO_ROOM, "no current room"); return; }
                if (!room.empty() && itcr->second != room) { s.send_error(protocol::errc::ROOM_MISMATCH, "room mismatch"); return; }
                room = itcr->second;
                // 퇴장 브로드캐스트(현재 방)
                auto itroom = chat.rooms.find(room);
                if (itroom != chat.rooms.end()) {
                    ChatState::WeakSession w = s.shared_from_this();
                    itroom->second.erase(w);
                    std::string sender;
                    auto it2 = chat.user.find(&s);
                    sender = (it2 != chat.user.end()) ? it2->second : std::string("guest");
                    server::core::protocol::write_lp_utf8(body, room);
                    server::core::protocol::write_lp_utf8(body, std::string("(system)"));
                    server::core::protocol::write_lp_utf8(body, sender + " 님이 퇴장했습니다");
                    { std::size_t off_sid = body.size(); body.resize(off_sid + 4); server::core::protocol::write_be32(0, body.data() + off_sid); }
                    auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    std::size_t off = body.size(); body.resize(off + 8);
                    std::uint64_t ts = static_cast<std::uint64_t>(now64);
                    for (int i = 7; i >= 0; --i) { body[off + i] = static_cast<std::uint8_t>(ts & 0xFF); ts >>= 8; }
                    auto itb = chat.rooms.find(room);
                    if (itb != chat.rooms.end()) {
                        auto& set = itb->second;
                        for (auto wit = set.begin(); wit != set.end(); ) {
                            if (auto p = wit->lock()) { targets.emplace_back(std::move(p)); ++wit; }
                            else { wit = set.erase(wit); }
                        }
                    }
                }
                // 로비로 자동 이동 설정
                chat.cur_room[&s] = std::string("lobby");
                chat.rooms["lobby"].insert(s.shared_from_this());
                }
                for (auto& t : targets) {
                    auto f = (t.get() == &s) ? server::core::protocol::FLAG_SELF : 0;
                    t->async_send(protocol::MSG_CHAT_BROADCAST, body, f);
                }
                // 로비 입장 브로드캐스트
                {
                    std::vector<std::shared_ptr<Session>> t2;
                    std::vector<std::uint8_t> body2;
                    {
                        std::lock_guard<std::mutex> lk(chat.mu);
                        std::string sender;
                        auto it2 = chat.user.find(&s);
                        sender = (it2 != chat.user.end()) ? it2->second : std::string("guest");
                        server::core::protocol::write_lp_utf8(body2, std::string("lobby"));
                        server::core::protocol::write_lp_utf8(body2, std::string("(system)"));
                        server::core::protocol::write_lp_utf8(body2, sender + " 님이 입장했습니다");
                        { std::size_t off_sid = body2.size(); body2.resize(off_sid + 4); server::core::protocol::write_be32(0, body2.data() + off_sid); }
                        auto itb = chat.rooms.find("lobby");
                        if (itb != chat.rooms.end()) {
                            auto& set = itb->second;
                            for (auto wit = set.begin(); wit != set.end(); ) {
                                if (auto p = wit->lock()) { t2.emplace_back(std::move(p)); ++wit; }
                                else { wit = set.erase(wit); }
                            }
                        }
                    }
                    for (auto& t : t2) t->async_send(protocol::MSG_CHAT_BROADCAST, body2, 0);
                }
            });

        tcp::endpoint ep(tcp::v4(), port);
        auto acceptor = std::make_shared<Acceptor>(io, ep, dispatcher, options, state,
            [&chat](std::shared_ptr<Session> sess){
                // 세션 종료 시 상태 정리
                sess->set_on_close([&chat](Session& s){
                    std::vector<std::shared_ptr<Session>> targets;
                    std::vector<std::uint8_t> body;
                    {
                        std::lock_guard<std::mutex> lk(chat.mu);
                        std::string name;
                        if (auto itname = chat.user.find(&s); itname != chat.user.end()) name = itname->second; else name = "guest";
                        chat.authed.erase(&s);
                        // by_user에서 제거
                        if (!name.empty()) {
                            auto itset = chat.by_user.find(name);
                            if (itset != chat.by_user.end()) {
                                ChatState::WeakSession w = s.shared_from_this();
                                itset->second.erase(w);
                            }
                        }
                        chat.user.erase(&s);
                        auto itcr = chat.cur_room.find(&s);
                        if (itcr != chat.cur_room.end()) {
                            auto room = itcr->second;
                            auto itroom = chat.rooms.find(room);
                            if (itroom != chat.rooms.end()) {
                                // 해당 세션만 정확히 제거
                                ChatState::WeakSession w = s.shared_from_this();
                                itroom->second.erase(w);
                // 퇴장 브로드캐스트(해당 방 전체): room, (system), text, u32 sender_sid(0), u64 ts
                server::core::protocol::write_lp_utf8(body, room);
                server::core::protocol::write_lp_utf8(body, std::string("(system)"));
                server::core::protocol::write_lp_utf8(body, name + " 님이 퇴장했습니다");
                { std::size_t off_sid = body.size(); body.resize(off_sid + 4); server::core::protocol::write_be32(0, body.data() + off_sid); }
                auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                std::size_t off = body.size(); body.resize(off + 8);
                std::uint64_t ts = static_cast<std::uint64_t>(now64);
                for (int i = 7; i >= 0; --i) { body[off + i] = static_cast<std::uint8_t>(ts & 0xFF); ts >>= 8; }
                                auto itb = chat.rooms.find(room);
                                if (itb != chat.rooms.end()) {
                                    auto& set = itb->second;
                                    for (auto wit = set.begin(); wit != set.end(); ) {
                                        if (auto p = wit->lock()) { targets.emplace_back(std::move(p)); ++wit; }
                                        else { wit = set.erase(wit); }
                                    }
                                }
                            }
                            chat.cur_room.erase(itcr);
                        }
                    }
                for (auto& t : targets) {
                    auto f = (t.get() == &s) ? server::core::protocol::FLAG_SELF : 0;
                    t->async_send(protocol::MSG_CHAT_BROADCAST, body, f);
                }
            });
            });
        acceptor->start();
        corelog::info("server_app 시작: 0.0.0.0:" + std::to_string(port));

        // 워커 스레드 풀
        unsigned int n = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> workers;
        workers.reserve(n);
        for (unsigned int i = 0; i < n; ++i) {
            workers.emplace_back([&io]() { io.run(); });
        }

        // 단순 대기(종료는 프로세스 종료로)
        for (auto& t : workers) t.join();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "server_app 예외: " << ex.what() << std::endl;
        return 1;
    }
}
