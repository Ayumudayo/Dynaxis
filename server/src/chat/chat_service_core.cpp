// UTF-8, 한국어 주석
#include "server/chat/chat_service.hpp"
#include "server/core/protocol/opcodes.hpp"
#include "server/core/protocol/frame.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/util/log.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "wire.pb.h"

using namespace server::core;
namespace proto = server::core::protocol;
namespace corelog = server::core::log;

namespace server::app::chat {

ChatService::ChatService(boost::asio::io_context& io,
                         server::core::JobQueue& job_queue,
                         std::shared_ptr<server::core::storage::IConnectionPool> db_pool,
                         std::shared_ptr<server::storage::redis::IRedisClient> redis)
    : io_(&io), job_queue_(job_queue), db_pool_(std::move(db_pool)), redis_(std::move(redis)) {}

ChatService::Strand& ChatService::strand_for(const std::string& room) {
    auto it = room_strands_.find(room);
    if (it == room_strands_.end()) {
        it = room_strands_.emplace(room, std::make_shared<Strand>(io_->get_executor())).first;
    }
    return *it->second;
}

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
    {
        std::string bytes; pb.SerializeToString(&bytes);
        body.assign(bytes.begin(), bytes.end());
    }
    s.async_send(proto::MSG_CHAT_BROADCAST, body, 0);
}

void ChatService::send_room_users(Session& s, const std::string& target) {
    std::vector<std::uint8_t> body;
    server::wire::v1::RoomUsers pb; pb.set_room(target);
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        auto itroom = state_.rooms.find(target);
        if (itroom != state_.rooms.end()) {
            for (auto wit = itroom->second.begin(); wit != itroom->second.end(); ) {
                if (auto p = wit->lock()) {
                    auto itu = state_.user.find(p.get());
                    std::string name = (itu != state_.user.end()) ? itu->second : std::string("guest");
                    pb.add_users(name); ++wit;
                } else { wit = itroom->second.erase(wit); }
            }
        }
    }
    {
        std::string bytes; pb.SerializeToString(&bytes);
        body.assign(bytes.begin(), bytes.end());
    }
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
    {
        std::string bytes; pb.SerializeToString(&bytes);
        body.assign(bytes.begin(), bytes.end());
    }
    s.async_send(proto::MSG_STATE_SNAPSHOT, body, 0);
}

} // namespace server::app::chat
