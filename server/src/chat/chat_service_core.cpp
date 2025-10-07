// UTF-8, 한국어 주석
#include "server/chat/chat_service.hpp"
#include "server/core/protocol/opcodes.hpp"
#include "server/core/protocol/frame.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/util/log.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "wire.pb.h"
// storage
#include "server/core/storage/connection_pool.hpp"
#include "server/core/storage/repositories.hpp"
#include "server/storage/redis/client.hpp"

#include <cstdlib>
#include <sstream>

using namespace server::core;
namespace proto = server::core::protocol;
namespace corelog = server::core::log;

namespace server::app::chat {

ChatService::ChatService(boost::asio::io_context& io,
                         server::core::JobQueue& job_queue,
                         std::shared_ptr<server::core::storage::IConnectionPool> db_pool,
                         std::shared_ptr<server::storage::redis::IRedisClient> redis)
    : io_(&io), job_queue_(job_queue), db_pool_(std::move(db_pool)), redis_(std::move(redis)) {
    if (const char* gw = std::getenv("GATEWAY_ID"); gw && *gw) {
        gateway_id_ = gw;
    }
    if (const char* flag = std::getenv("WRITE_BEHIND_ENABLED"); flag && *flag && std::string(flag) != "0") {
        write_behind_.enabled = true;
    }
    if (const char* key = std::getenv("REDIS_STREAM_KEY"); key && *key) {
        write_behind_.stream_key = key;
    }
    if (const char* maxlen = std::getenv("REDIS_STREAM_MAXLEN"); maxlen && *maxlen) {
        char* end = nullptr;
        unsigned long long value = std::strtoull(maxlen, &end, 10);
        if (end != maxlen && value > 0) {
            write_behind_.maxlen = static_cast<std::size_t>(value);
        }
    }
    if (const char* approx = std::getenv("REDIS_STREAM_APPROX"); approx && *approx) {
        if (std::string(approx) == "0") {
            write_behind_.approximate = false;
        }
    }
    if (write_behind_.enabled) {
        corelog::info(std::string("Write-behind enabled: stream=") + write_behind_.stream_key +
                      (write_behind_.maxlen ? (std::string(", maxlen=") + std::to_string(*write_behind_.maxlen)) : std::string(", maxlen=none")) +
                      std::string(write_behind_.approximate ? ", approx=~" : ", approx=exact"));
    } else {
        corelog::warn("Write-behind disabled (set WRITE_BEHIND_ENABLED=1 to enable)");
    }
}

ChatService::Strand& ChatService::strand_for(const std::string& room) {
    auto it = room_strands_.find(room);
    if (it == room_strands_.end()) {
        it = room_strands_.emplace(room, std::make_shared<Strand>(io_->get_executor())).first;
    }
    return *it->second;
}

bool ChatService::write_behind_enabled() const {
    return write_behind_.enabled && static_cast<bool>(redis_);
}

std::string ChatService::generate_uuid_v4() {
    std::array<unsigned char, 16> b{};
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    for (size_t i = 0; i < b.size(); i += 8) {
        auto v = rng();
        for (int j = 0; j < 8 && (i + j) < b.size(); ++j) b[i + j] = static_cast<unsigned char>((v >> (j * 8)) & 0xFF);
    }
    // RFC 4122 variant & version
    b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40); // version 4
    b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80); // variant 10xx
    auto hex = [](unsigned char c) { const char* d = "0123456789abcdef"; return std::pair<char,char>{d[(c>>4)&0xF], d[c&0xF]}; };
    std::string s; s.resize(36);
    int k = 0;
    for (int i = 0; i < 16; ++i) {
        auto [h,l] = hex(b[i]); s[k++] = h; s[k++] = l;
        if (i==3 || i==5 || i==7 || i==9) s[k++] = '-';
    }
    return s;
}

std::string ChatService::get_or_create_session_uuid(Session& s) {
    std::lock_guard<std::mutex> lk(state_.mu);
    auto it = state_.session_uuid.find(&s);
    if (it != state_.session_uuid.end() && !it->second.empty()) return it->second;
    std::string id = generate_uuid_v4();
    state_.session_uuid[&s] = id;
    return id;
}

void ChatService::emit_write_behind_event(const std::string& type,
                                           const std::string& session_id,
                                           const std::optional<std::string>& user_id,
                                           const std::optional<std::string>& room_id,
                                           std::vector<std::pair<std::string, std::string>> extra_fields) {
    if (!write_behind_enabled() || type.empty() || session_id.empty()) {
        return;
    }
    std::vector<std::pair<std::string, std::string>> fields;
    fields.reserve(6 + extra_fields.size());
    fields.emplace_back("type", type);
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    fields.emplace_back("ts_ms", std::to_string(now_ms));
    fields.emplace_back("session_id", session_id);
    if (user_id && !user_id->empty()) {
        fields.emplace_back("user_id", *user_id);
    }
    if (room_id && !room_id->empty()) {
        fields.emplace_back("room_id", *room_id);
    }
    if (!gateway_id_.empty()) {
        fields.emplace_back("gateway_id", gateway_id_);
    }
    for (auto& kv : extra_fields) {
        if (!kv.first.empty() && !kv.second.empty()) {
            fields.emplace_back(std::move(kv));
        }
    }
    if (!redis_->xadd(write_behind_.stream_key, fields, nullptr, write_behind_.maxlen, write_behind_.approximate)) {
        corelog::warn(std::string("write-behind XADD failed: type=") + type);
    }
}

std::string ChatService::gen_temp_name_uuid8() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uint32_t v = static_cast<std::uint32_t>(rng());
    std::ostringstream oss; oss << std::hex; oss.width(8); oss.fill('0'); oss << v; return oss.str();
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
    // 임시 닉 생성: UUID 앞 8자(랜덤 32비트 근사)
    for (int i=0;i<4;++i) {
        std::string cand = gen_temp_name_uuid8();
        if (!state_.by_user.count(cand) || state_.by_user[cand].empty()) return cand;
    }
    return gen_temp_name_uuid8();
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
            std::string display_name = it->first;
            if (state_.room_passwords.count(it->first)) {
                display_name = "🔒" + display_name;
            }
            msg += " " + display_name + "(" + std::to_string(alive) + ")";
        }
        for (auto& name : to_remove) { state_.rooms.erase(name); state_.room_passwords.erase(name); }
    }
    server::wire::v1::ChatBroadcast pb; pb.set_room("(system)"); pb.set_sender("(system)"); pb.set_text(msg); pb.set_sender_sid(0);
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
    std::vector<std::string> names;
    bool allow = true;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        auto itroom = state_.rooms.find(target);
        bool is_locked = state_.room_passwords.count(target) > 0;
        bool is_member = false;
        std::vector<std::string> collected;
        if (itroom != state_.rooms.end()) {
            for (auto wit = itroom->second.begin(); wit != itroom->second.end(); ) {
                if (auto p = wit->lock()) {
                    auto itu = state_.user.find(p.get());
                    std::string name = (itu != state_.user.end()) ? itu->second : std::string("guest");
                    if (p.get() == &s) {
                        is_member = true;
                    }
                    collected.push_back(std::move(name));
                    ++wit;
                } else {
                    wit = itroom->second.erase(wit);
                }
            }
        }
        if (is_locked && !is_member) {
            allow = false;
        } else {
            names = std::move(collected);
        }
    }

    if (!allow) {
        send_system_notice(s, "room is locked");
        return;
    }

    server::wire::v1::RoomUsers pb;
    pb.set_room(target);
    for (const auto& name : names) {
        pb.add_users(name);
    }
    std::string bytes;
    pb.SerializeToString(&bytes);
    std::vector<std::uint8_t> body(bytes.begin(), bytes.end());
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
            auto* ri = pb.add_rooms(); ri->set_name(it->first); ri->set_members(alive); ri->set_locked(state_.room_passwords.count(it->first) > 0);
        }
        for (auto& name : to_remove) { state_.rooms.erase(name); state_.room_passwords.erase(name); }
    }
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        auto itroom = state_.rooms.find(current);
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
    // 최근 메시지 로딩(DB 경로)
    if (db_pool_) {
        try {
            std::string rid = ensure_room_id_ci(current);
            if (!rid.empty()) {
                // 환경 기반 제한값
                std::size_t limit = 20; if (const char* v = std::getenv("SNAPSHOT_RECENT_LIMIT")) { unsigned long n = std::strtoul(v, nullptr, 10); if (n >= 5 && n <= 200) limit = static_cast<std::size_t>(n); }
                std::size_t fetch_factor = 3; if (const char* v = std::getenv("SNAPSHOT_FETCH_FACTOR")) { unsigned long n = std::strtoul(v, nullptr, 10); if (n >= 1 && n <= 10) fetch_factor = static_cast<std::size_t>(n); }

                std::string uid;
                {
                    std::lock_guard<std::mutex> lk(state_.mu);
                    auto itu = state_.user_uuid.find(&s);
                    if (itu != state_.user_uuid.end()) uid = itu->second;
                }

                auto uow = db_pool_->make_unit_of_work();
                std::uint64_t last_seen = 0;
                if (!uid.empty()) {
                    auto opt = uow->memberships().get_last_seen(uid, rid);
                    last_seen = opt.value_or(0);
                }

                auto last_id = uow->messages().get_last_id(rid);
                std::uint64_t since_id = 0;
                if (last_id > 0) {
                    if (last_seen == 0) {
                        since_id = (last_id > limit) ? (last_id - limit) : 0;
                    } else if (last_seen >= last_id) {
                        // 모두 읽은 상태: 최근 limit만
                        since_id = (last_id > limit) ? (last_id - limit) : 0;
                    } else {
                        // 일부 미읽음 존재: 컨텍스트 포함하여 제한 배수만큼 여유롭게 조회
                        std::uint64_t context = static_cast<std::uint64_t>(limit) * static_cast<std::uint64_t>(fetch_factor);
                        if (last_id > context) {
                            std::uint64_t cut = last_id - context;
                            since_id = (last_seen > cut) ? last_seen : cut;
                        } else {
                            since_id = last_seen;
                        }
                    }
                }

                auto msgs = uow->messages().fetch_recent_by_room(rid, since_id, static_cast<std::size_t>(limit * fetch_factor));
                // 필요한 만큼만(마지막 limit개) 응답에 포함
                if (msgs.size() > limit) {
                    msgs.erase(msgs.begin(), msgs.end() - static_cast<std::ptrdiff_t>(limit));
                }
                for (const auto& m : msgs) {
                    auto* sm = pb.add_messages();
                    sm->set_id(m.id);
                    std::string sender;
                    if (m.user_name && !m.user_name->empty()) sender = *m.user_name;
                    else sender = std::string("(system)");
                    sm->set_sender(sender);
                    sm->set_text(m.content);
                    sm->set_ts_ms(static_cast<std::uint64_t>(m.created_at_ms));
                }
                pb.set_last_seen_id(last_seen);
            }
        } catch (...) {}
    }
    {
        std::string bytes; pb.SerializeToString(&bytes);
        body.assign(bytes.begin(), bytes.end());
    }
    s.async_send(proto::MSG_STATE_SNAPSHOT, body, 0);
}

} // namespace server::app::chat

// 추가 구현: 외부 수신 브로드캐스트를 룸에 전파
namespace server::app::chat {

void ChatService::broadcast_room(const std::string& room, const std::vector<std::uint8_t>& body, Session* self) {
    std::vector<std::shared_ptr<Session>> targets;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        auto it = state_.rooms.find(room);
        if (it != state_.rooms.end()) {
            auto& set = it->second;
            for (auto wit = set.begin(); wit != set.end(); ) {
                if (auto p = wit->lock()) { targets.emplace_back(std::move(p)); ++wit; }
                else { wit = set.erase(wit); }
            }
        }
    }
    for (auto& t : targets) {
        int f = 0; // 재전파에는 self 플래그 사용 안 함
        t->async_send(proto::MSG_CHAT_BROADCAST, body, f);
    }
}

} // namespace server::app::chat

// 별도 구현부: 저장소 보조 함수
namespace server::app::chat {

void ChatService::send_system_notice(Session& s, const std::string& text) {
    server::wire::v1::ChatBroadcast pb;
    pb.set_room("(system)");
    pb.set_sender("(system)");
    pb.set_text(text);
    pb.set_sender_sid(0);
    auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    pb.set_ts_ms(static_cast<std::uint64_t>(now64));
    std::string bytes;
    pb.SerializeToString(&bytes);
    std::vector<std::uint8_t> body(bytes.begin(), bytes.end());
    s.async_send(proto::MSG_CHAT_BROADCAST, body, 0);
}

void ChatService::send_whisper_result(Session& s, bool ok, const std::string& reason) {
    server::wire::v1::WhisperResult pb;
    pb.set_ok(ok);
    if (!reason.empty()) {
        pb.set_reason(reason);
    }
    std::string bytes;
    pb.SerializeToString(&bytes);
    std::vector<std::uint8_t> body(bytes.begin(), bytes.end());
    s.async_send(proto::MSG_WHISPER_RES, body, 0);
}

void ChatService::dispatch_whisper(std::shared_ptr<Session> session_sp, const std::string& target_user, const std::string& text) {
    if (!session_sp) return;
    if (target_user.empty() || text.empty()) {
        send_system_notice(*session_sp, "usage: /whisper <user> <message>");
        send_whisper_result(*session_sp, false, "invalid payload");
        return;
    }

    {
        std::lock_guard<std::mutex> lk(state_.mu);
        if (!state_.authed.count(session_sp.get())) {
            session_sp->send_error(proto::errc::UNAUTHORIZED, "unauthorized");
            return;
        }
    }

    std::string sender;
    bool sender_is_guest = false;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        auto it_sender = state_.user.find(session_sp.get());
        sender = (it_sender != state_.user.end()) ? it_sender->second : std::string("guest");
        sender_is_guest = state_.guest.count(session_sp.get()) > 0;
    }
    if (sender == "guest" || sender_is_guest) {
        send_system_notice(*session_sp, "login required for whisper");
        send_whisper_result(*session_sp, false, "login required");
        session_sp->send_error(proto::errc::UNAUTHORIZED, "guest cannot whisper");
        return;
    }

    if (target_user == sender) {
        send_system_notice(*session_sp, "cannot whisper to yourself");
        send_whisper_result(*session_sp, false, "cannot whisper to yourself");
        corelog::info("[whisper] sender=" + sender + " target=" + target_user + " status=self_target");
        return;
    }

    std::vector<std::shared_ptr<Session>> targets;
    bool ineligible_found = false;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        auto itset = state_.by_user.find(target_user);
        if (itset != state_.by_user.end()) {
            for (auto wit = itset->second.begin(); wit != itset->second.end(); ) {
                if (auto p = wit->lock()) {
                    if (p.get() == session_sp.get()) {
                        ++wit;
                        continue;
                    }
                    if (!state_.authed.count(p.get()) || state_.guest.count(p.get())) {
                        ineligible_found = true;
                        ++wit;
                        continue;
                    }
                    if (p.get() != session_sp.get()) {
                        targets.emplace_back(std::move(p));
                    }
                    ++wit;
                } else {
                    wit = itset->second.erase(wit);
                }
            }
        }
    }

    if (targets.empty() && ineligible_found) {
        send_system_notice(*session_sp, "user cannot receive whispers (login required): " + target_user);
        send_whisper_result(*session_sp, false, "recipient not eligible");
        corelog::info("[whisper] sender=" + sender + " target=" + target_user + " status=recipient_guest");
        return;
    }

    if (targets.empty()) {
        send_system_notice(*session_sp, "user not found: " + target_user);
        send_whisper_result(*session_sp, false, "user not found");
        corelog::info("[whisper] sender=" + sender + " target=" + target_user + " status=not_found");
        return;
    }

    auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    server::wire::v1::WhisperNotice notice;
    notice.set_sender(sender);
    notice.set_recipient(target_user);
    notice.set_text(text);
    notice.set_ts_ms(static_cast<std::uint64_t>(now64));

    notice.set_outgoing(false);
    std::string incoming_bytes;
    notice.SerializeToString(&incoming_bytes);
    std::vector<std::uint8_t> incoming(incoming_bytes.begin(), incoming_bytes.end());
    for (auto& target : targets) {
        target->async_send(proto::MSG_WHISPER_BROADCAST, incoming, 0);
    }

    notice.set_outgoing(true);
    std::string outgoing_bytes;
    notice.SerializeToString(&outgoing_bytes);
    std::vector<std::uint8_t> outgoing(outgoing_bytes.begin(), outgoing_bytes.end());
    session_sp->async_send(proto::MSG_WHISPER_BROADCAST, outgoing, 0);

    send_whisper_result(*session_sp, true, "");
    corelog::info("[whisper] sender=" + sender + " target=" + target_user + " recipients=" + std::to_string(targets.size()) + " text=" + text);
}

std::string ChatService::hash_room_password(const std::string& password) {
    std::hash<std::string> hasher;
    std::size_t value = hasher(password);
    std::ostringstream oss;
    oss << std::hex << value;
    return oss.str();
}

std::string ChatService::ensure_room_id_ci(const std::string& room_name) {
    if (!db_pool_) return std::string();
    // 캐시 확인
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        auto it = state_.room_ids.find(room_name);
        if (it != state_.room_ids.end()) return it->second;
    }
    try {
        auto uow = db_pool_->make_unit_of_work();
        auto found = uow->rooms().find_by_name_exact_ci(room_name);
        std::string id;
        if (found) {
            id = found->id;
        } else {
            auto created = uow->rooms().create(room_name, true);
            id = created.id;
        }
        uow->commit();
        std::lock_guard<std::mutex> lk2(state_.mu);
        state_.room_ids.emplace(room_name, id);
        return id;
    } catch (const std::exception& e) {
        corelog::error(std::string("ensure_room_id_ci failed: ") + e.what());
        return std::string();
    }
}

} // namespace server::app::chat



