#include "server/chat/chat_service.hpp"
#include "server/protocol/game_opcodes.hpp"
#include "server/core/protocol/packet.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/util/log.hpp"
#include "server/core/util/service_registry.hpp"
#include "server/core/concurrent/task_scheduler.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "chat_hook_plugin_chain.hpp"
#include "wire.pb.h"
// 저장소 연동 헤더
#include "server/core/storage/connection_pool.hpp"
#include "server/core/storage/repositories.hpp"
#include "server/storage/redis/client.hpp"

#include <algorithm>
#include <random>
#include <array>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <utility>
#include <unordered_set>
using namespace server::core;
namespace proto = server::core::protocol;
namespace game_proto = server::protocol;
namespace corelog = server::core::log;
namespace services = server::core::util::services;

/**
 * @brief ChatService 코어 상태/설정/플러그인 초기화 구현입니다.
 *
 * DB/Redis/Write-behind/Presence/History 설정을 한곳에서 해석해,
 * 핸들러 로직이 환경별 분기 없이 동일 인터페이스를 사용하도록 만듭니다.
 */
namespace server::app::chat {

struct ChatService::HookPluginState {
    explicit HookPluginState(ChatHookPluginChain::Config cfg)
        : chain(std::move(cfg)) {}
    ChatHookPluginChain chain;
};

// ChatService 생성자: 주요 의존성을 주입받고 환경 변수로부터 설정을 로드합니다.
ChatService::ChatService(boost::asio::io_context& io,
                         server::core::JobQueue& job_queue,
                         std::shared_ptr<server::core::storage::IConnectionPool> db_pool,
                         std::shared_ptr<server::storage::redis::IRedisClient> redis)
    : io_(&io), job_queue_(job_queue), db_pool_(std::move(db_pool)), redis_(std::move(redis)) {
    
    // 의존성이 주입되지 않은 경우 ServiceRegistry에서 가져옵니다.
    if (!db_pool_) {
        db_pool_ = services::get<server::core::storage::IConnectionPool>();
    }
    if (!redis_) {
        redis_ = services::get<server::storage::redis::IRedisClient>();
    }

    // 게이트웨이 ID 설정 (분산 환경 식별용)
    if (const char* gw = std::getenv("GATEWAY_ID"); gw && *gw) {
        gateway_id_ = gw;
    }

    // Write-behind 설정 로드
    // WRITE_BEHIND_ENABLED=1 이면 채팅 이벤트를 Redis Streams에 먼저 기록하고,
    // 별도의 워커가 이를 DB에 반영합니다.
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

    // Presence(접속 현황) TTL 설정
    if (const char* ttl = std::getenv("PRESENCE_TTL_SEC"); ttl && *ttl) {
        unsigned long t = std::strtoul(ttl, nullptr, 10);
        if (t > 0 && t < 3600) {
            presence_.ttl = static_cast<unsigned int>(t);
        }
    }
    if (const char* prefix = std::getenv("REDIS_CHANNEL_PREFIX"); prefix && *prefix) {
        presence_.prefix = prefix;
    }

    // 환경 변수 읽기 헬퍼 함수
    const auto read_env = [](const char* primary, const char* secondary = nullptr) -> const char* {
        if (primary) {
            if (const char* value = std::getenv(primary); value && *value) {
                return value;
            }
        }
        if (secondary) {
            if (const char* value = std::getenv(secondary); value && *value) {
                return value;
            }
        }
        return nullptr;
    };

    // 최근 대화 내역(History) 관련 설정
    if (const char* limit_env = read_env("RECENT_HISTORY_LIMIT", "SNAPSHOT_RECENT_LIMIT")) {
        char* end = nullptr;
        unsigned long value = std::strtoul(limit_env, &end, 10);
        if (limit_env != end && value >= 5 && value <= 2000) {
            history_.recent_limit = static_cast<std::size_t>(value);
        }
    }
    if (const char* maxlen_env = std::getenv("ROOM_RECENT_MAXLEN"); maxlen_env && *maxlen_env) {
        char* end = nullptr;
        unsigned long value = std::strtoul(maxlen_env, &end, 10);
        if (maxlen_env != end && value >= history_.recent_limit && value <= 5000) {
            history_.max_list_len = static_cast<std::size_t>(value);
        }
    }
    if (const char* ttl_env = std::getenv("CACHE_TTL_RECENT_MSGS"); ttl_env && *ttl_env) {
        char* end = nullptr;
        unsigned long value = std::strtoul(ttl_env, &end, 10);
        if (ttl_env != end && value >= 60 && value <= 604800) {
            history_.cache_ttl_sec = static_cast<unsigned int>(value);
        }
    }
    if (const char* fetch_env = read_env("RECENT_HISTORY_FETCH_FACTOR", "SNAPSHOT_FETCH_FACTOR")) {
        char* end = nullptr;
        unsigned long value = std::strtoul(fetch_env, &end, 10);
        if (fetch_env != end && value >= 1 && value <= 10) {
            history_.fetch_factor = static_cast<std::size_t>(value);
        }
    }
    if (history_.max_list_len < history_.recent_limit) {
        history_.max_list_len = history_.recent_limit;
    }

    if (write_behind_.enabled) {
        corelog::info(std::string("Write-behind enabled: stream=") + write_behind_.stream_key +
                      (write_behind_.maxlen ? (std::string(", maxlen=") + std::to_string(*write_behind_.maxlen)) : std::string(", maxlen=none")) +
                      std::string(write_behind_.approximate ? ", approx=~" : ", approx=exact"));
    } else {
        corelog::warn("Write-behind disabled (set WRITE_BEHIND_ENABLED=1 to enable)");
    }

    // Optional chat-hook plugins (hot-reloadable shared libraries)
    {
        ChatHookPluginChain::Config cfg;

        const auto split_paths = [](const char* raw) -> std::vector<std::filesystem::path> {
            std::vector<std::filesystem::path> out;
            if (!raw || !*raw) {
                return out;
            }

            auto trim = [](std::string& s) {
                while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
                    s.erase(s.begin());
                }
                while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
                    s.pop_back();
                }
            };

            std::string cur;
            for (const char* p = raw; *p; ++p) {
                const char c = *p;
                if (c == ';' || c == ',') {
                    trim(cur);
                    if (!cur.empty()) {
                        out.emplace_back(cur);
                    }
                    cur.clear();
                    continue;
                }
                cur.push_back(c);
            }
            trim(cur);
            if (!cur.empty()) {
                out.emplace_back(cur);
            }
            return out;
        };

        bool enabled = false;
        if (const char* list = std::getenv("CHAT_HOOK_PLUGIN_PATHS"); list && *list) {
            cfg.plugin_paths = split_paths(list);
            enabled = !cfg.plugin_paths.empty();
        }

        if (!enabled) {
            if (const char* dir = std::getenv("CHAT_HOOK_PLUGINS_DIR"); dir && *dir) {
                cfg.plugins_dir = std::filesystem::path(dir);
                enabled = true;
            }
        }

        if (!enabled) {
            if (const char* val = std::getenv("CHAT_HOOK_PLUGIN_PATH"); val && *val) {
                cfg.plugin_paths.emplace_back(val);
                enabled = true;
            }
        }

        if (enabled) {
            if (const char* cache = std::getenv("CHAT_HOOK_CACHE_DIR"); cache && *cache) {
                cfg.cache_dir = cache;
            }
            if (const char* lock = std::getenv("CHAT_HOOK_LOCK_PATH"); lock && *lock) {
                cfg.single_lock_path = lock;
            }

            hook_plugin_ = std::make_unique<HookPluginState>(std::move(cfg));
            hook_plugin_->chain.poll_reload();

            unsigned long interval_ms = 500;
            if (const char* interval = std::getenv("CHAT_HOOK_RELOAD_INTERVAL_MS"); interval && *interval) {
                interval_ms = std::strtoul(interval, nullptr, 10);
            }
            if (interval_ms > 0) {
                if (auto scheduler = services::get<server::core::concurrent::TaskScheduler>()) {
                    scheduler->schedule_every([this]() {
                        if (!hook_plugin_) {
                            return;
                        }
                        (void)job_queue_.TryPush([this]() {
                            if (hook_plugin_) {
                                hook_plugin_->chain.poll_reload();
                            }
                        });
                    }, std::chrono::milliseconds{static_cast<long long>(interval_ms)});
                }
            }
        }
    }
}

ChatService::~ChatService() = default;

ChatService::ChatHookPluginsMetrics ChatService::chat_hook_plugins_metrics() const {
    ChatHookPluginsMetrics out{};
    if (!hook_plugin_) {
        out.enabled = false;
        out.mode = "none";
        return out;
    }

    const auto snap = hook_plugin_->chain.metrics_snapshot();
    out.enabled = snap.configured;
    out.mode = snap.mode;
    out.plugins.reserve(snap.plugins.size());
    for (const auto& p : snap.plugins) {
        ChatHookPluginMetric m{};
        m.file = p.plugin_path.filename().string();
        m.loaded = p.loaded;
        m.name = p.name;
        m.version = p.version;
        m.reload_attempt_total = p.reload_attempt_total;
        m.reload_success_total = p.reload_success_total;
        m.reload_failure_total = p.reload_failure_total;
        out.plugins.push_back(std::move(m));
    }
    return out;
}

// 방별 Strand(직렬화된 실행 컨텍스트)를 반환합니다.
// 동일한 방에 대한 작업은 동일한 스레드에서 순차적으로 실행됨을 보장합니다.
// 이는 멀티스레드 환경에서 방 상태(참여자 목록 등)의 동시성 문제를 해결하는 핵심 메커니즘입니다.
// Mutex를 직접 사용하는 것보다 데드락 위험이 적고 코드가 간결해집니다.
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

// UUID v4 생성 (난수 기반)
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

// 세션별 고유 UUID를 조회하거나 생성합니다.
std::string ChatService::get_or_create_session_uuid(Session& s) {
    std::lock_guard<std::mutex> lk(state_.mu);
    auto it = state_.session_uuid.find(&s);
    if (it != state_.session_uuid.end() && !it->second.empty()) return it->second;
    std::string id = generate_uuid_v4();
    state_.session_uuid[&s] = id;
    return id;
}

// Write-behind 이벤트를 Redis Stream에 발행합니다.
// 이 이벤트들은 별도의 워커 프로세스에 의해 DB에 비동기적으로 저장됩니다.
// 즉, 채팅 서버는 DB 쓰기 지연을 기다리지 않고 즉시 응답할 수 있어 반응성이 향상됩니다.
// (CQRS 패턴의 변형으로 볼 수 있습니다)
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
    // XADD 명령을 사용하여 스트림에 추가합니다.
    // PUBLISH(Pub/Sub)와 달리 스트림은 데이터가 영구적으로 저장되며(설정에 따라),
    // 컨슈머 그룹을 통해 안정적인 처리가 가능합니다. (At-least-once Delivery)
    if (!redis_->xadd(write_behind_.stream_key, fields, nullptr, write_behind_.maxlen, write_behind_.approximate)) {
        corelog::warn(std::string("write-behind XADD failed: type=") + type);
    }
}

// WeakPtr로 관리되는 세션 목록에서 유효한 세션만 수집합니다.
void ChatService::collect_room_sessions(RoomSet& set, std::vector<std::shared_ptr<Session>>& out) {
    for (auto it = set.begin(); it != set.end(); ) {
        if (auto session_sp = it->lock()) {
            out.emplace_back(std::move(session_sp));
            ++it;
        } else {
            it = set.erase(it);
        }
    }
}

unsigned int ChatService::presence_ttl() const {
    return presence_.ttl;
}

std::string ChatService::make_presence_key(std::string_view category, const std::string& id) const {
    std::string key;
    key.reserve(presence_.prefix.size() + category.size() + id.size());
    key.append(presence_.prefix);
    key.append(category);
    key.append(id);
    return key;
}

// 사용자의 접속 상태(Presence)를 갱신합니다. (Redis SETEX)
void ChatService::touch_user_presence(const std::string& uid) {
    if (!redis_ || uid.empty()) {
        return;
    }
    redis_->setex(make_presence_key("presence:user:", uid), "1", presence_ttl());
}

// 임시 닉네임 생성 (UUID 기반 8자리)
std::string ChatService::gen_temp_name_uuid8() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uint32_t v = static_cast<std::uint32_t>(rng());
    std::ostringstream oss; oss << std::hex; oss.width(8); oss.fill('0'); oss << v; return oss.str();
}

// 닉네임 중복 검사 및 임시 닉네임 할당
// desired가 비어있거나 "guest"인 경우 고유한 임시 닉네임을 생성하여 반환합니다.
// 이미 사용 중인 닉네임인 경우 에러를 전송하고 빈 문자열을 반환합니다.
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
    // 임시 닉네임은 UUID의 앞 8자를 잘라 32비트 난수 근사로 사용한다.
    for (int i=0;i<4;++i) {
        std::string cand = gen_temp_name_uuid8();
        if (!state_.by_user.count(cand) || state_.by_user[cand].empty()) return cand;
    }
    return gen_temp_name_uuid8();
}

// 현재 활성화된 방 목록을 클라이언트에게 전송합니다.
// (system) 발신자로 채팅 메시지 형식을 빌려 목록을 전송합니다.
void ChatService::send_rooms_list(Session& s) {
    std::vector<std::uint8_t> body;
    std::string msg = "rooms:";
    
    // 1. Redis 데이터 미리 조회 (Lock 없이 수행)
    struct RoomInfo {
        std::string name;
        std::size_t count;
        bool is_locked;
    };
    std::vector<RoomInfo> redis_rooms;
    bool redis_available = false;

    if (redis_) {
        redis_available = true;
        std::vector<std::string> redis_rooms_list;
        redis_->smembers("rooms:active", redis_rooms_list);
        
        bool lobby_found = false;
            
        for (const auto& r : redis_rooms_list) {
            if (r == "lobby") lobby_found = true;
            
            std::vector<std::string> users;
            redis_->smembers("room:users:" + r, users);
            
            bool locked = false;
            auto pw = redis_->get("room:password:" + r);
            if (pw.has_value()) locked = true;
            
            redis_rooms.push_back({r, users.size(), locked});
        }
        
        if (!lobby_found) {
            std::vector<std::string> users;
            redis_->smembers("room:users:lobby", users);
            redis_rooms.push_back({"lobby", users.size(), false});
        }
    }

    {
        // 2. 로컬 상태 처리 (Lock 필요)
        std::lock_guard<std::mutex> lk(state_.mu);
        
        // 로컬 방 정리
        std::vector<std::string> to_remove;
        for (auto it = state_.rooms.begin(); it != state_.rooms.end(); ++it) {
            std::size_t alive = 0;
            for (auto wit = it->second.begin(); wit != it->second.end(); ) { if (auto p = wit->lock()) { ++alive; ++wit; } else { wit = it->second.erase(wit); } }
            if (alive == 0 && it->first != std::string("lobby")) { to_remove.push_back(it->first); continue; }
        }
        for (auto& name : to_remove) { state_.rooms.erase(name); state_.room_passwords.erase(name); }

        if (redis_available) {
            // Redis 데이터 기반으로 메시지 구성
            for (auto& info : redis_rooms) {
                std::string display_name = info.name;
                bool is_locked = info.is_locked;
                
                // Redis에 잠금 정보가 없어도 로컬에 있을 수 있음
                if (!is_locked && state_.room_passwords.count(info.name)) {
                    is_locked = true;
                }

                if (is_locked) {
                    display_name = "🔒" + display_name;
                }
                msg += " " + display_name + "(" + std::to_string(info.count) + ")";
            }
        } else {
            // Fallback to local state
            for (auto it = state_.rooms.begin(); it != state_.rooms.end(); ++it) {
                std::size_t alive = it->second.size();
                std::string display_name = it->first;
                if (state_.room_passwords.count(it->first)) {
                    display_name = "🔒" + display_name;
                }
                msg += " " + display_name + "(" + std::to_string(alive) + ")";
            }
        }
    }
    // ChatBroadcast 메시지를 수동으로 직렬화합니다.
    // 편의 함수 대신 수동 직렬화를 사용하는 이유는,
    // (system) sender와 현재 타임스탬프(ts_ms)를 정확히 설정하기 위함입니다.
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
    s.async_send(game_proto::MSG_CHAT_BROADCAST, body, 0);
}

// 해당 방의 로컬 세션에게만 상태 갱신 알림을 전송합니다.
void ChatService::broadcast_refresh_local(const std::string& room) {
    std::vector<std::uint8_t> empty_body;
    std::vector<std::shared_ptr<Session>> targets;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        auto it = state_.rooms.find(room);
        if (it != state_.rooms.end()) {
            collect_room_sessions(it->second, targets);
        }
    }
    
    for (auto& s : targets) {
        s->async_send(game_proto::MSG_REFRESH_NOTIFY, empty_body, 0);
    }
}

// 해당 방의 모든 유저에게 상태 갱신 알림을 전송합니다.
// 로컬 전송 + Redis Pub/Sub 전파
void ChatService::broadcast_refresh(const std::string& room) {
    // 1. 로컬 세션에게 전송
    broadcast_refresh_local(room);

    // 2. Redis Pub/Sub으로 다른 서버에 전파
    if (redis_) {
        try {
            if (const char* use = std::getenv("USE_REDIS_PUBSUB"); use && std::strcmp(use, "0") != 0) {
                // fanout:refresh:<room> 채널 사용
                std::string channel = presence_.prefix + std::string("fanout:refresh:") + room;
                // Payload는 gwid만 있으면 됨 (self-echo 방지용)
                std::string message = "gw=" + gateway_id_;
                redis_->publish(channel, std::move(message));
            }
        } catch (...) {}
    }
}

// 특정 방의 사용자 목록을 클라이언트에게 전송합니다.
// 잠긴 방의 경우 멤버가 아니면 목록을 볼 수 없습니다.
void ChatService::send_room_users(Session& s, const std::string& target) {
    std::vector<std::string> names;
    bool allow = true;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        auto itroom = state_.rooms.find(target);
        bool is_locked = state_.room_passwords.count(target) > 0;
        bool is_member = false;
        
        // 로컬 세션에서 멤버 여부 확인
        if (itroom != state_.rooms.end()) {
            for (auto wit = itroom->second.begin(); wit != itroom->second.end(); ) {
                if (auto p = wit->lock()) {
                    if (p.get() == &s) {
                        is_member = true;
                        break;
                    }
                    ++wit;
                } else {
                    wit = itroom->second.erase(wit);
                }
            }
        }
        
        if (is_locked && !is_member) {
            allow = false;
        } else {
            // Redis에서 전체 사용자 목록 조회 (분산 환경 지원)
            if (redis_) {
                std::vector<std::string> redis_users;
                if (redis_->smembers("room:users:" + target, redis_users)) {
                    names = std::move(redis_users);
                }
            }
            // Redis가 없거나 실패한 경우 로컬 상태를 fallback으로 사용
            if (names.empty() && itroom != state_.rooms.end()) {
                 for (auto wit = itroom->second.begin(); wit != itroom->second.end(); ) {
                    if (auto p = wit->lock()) {
                        auto itu = state_.user.find(p.get());
                        std::string name = (itu != state_.user.end()) ? itu->second : std::string("guest");
                        names.push_back(std::move(name));
                        ++wit;
                    } else {
                        wit = itroom->second.erase(wit);
                    }
                }
            }
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
    s.async_send(game_proto::MSG_ROOM_USERS, body, 0);
}

// 방 입장 시 현재 상태(방 목록, 유저 목록, 최근 메시지)를 스냅샷으로 전송합니다.
// 클라이언트는 이 정보를 받아 UI를 초기화합니다.
// 1. 활성 방 목록
// 2. 현재 방의 참여자 목록
// 3. 최근 대화 내역 (Redis 캐시 + DB Fallback)
// 4. 마지막으로 읽은 메시지 ID (Read Receipt)
void ChatService::send_snapshot(Session& s, const std::string& current) {
    std::vector<std::uint8_t> body;
    server::wire::v1::StateSnapshot pb; pb.set_current_room(current);
    
    // 1. Redis 데이터 미리 조회 (Lock 없이 수행)
    struct RoomInfo {
        std::string name;
        std::size_t count;
        bool is_locked;
    };
    std::vector<RoomInfo> redis_rooms;
    bool redis_available = false;

    if (redis_) {
        redis_available = true;
        std::vector<std::string> active_rooms;
        redis_->smembers("rooms:active", active_rooms);
        std::string room_list_str;
        for (const auto& r : active_rooms) room_list_str += r + ", ";
        bool lobby_found = false;
        for (const auto& r : active_rooms) {
            if (r == "lobby") lobby_found = true;
            
            std::vector<std::string> users;
            redis_->smembers("room:users:" + r, users);
            
            bool locked = false;
            auto pw = redis_->get("room:password:" + r);
            if (pw.has_value()) locked = true;
            
            redis_rooms.push_back({r, users.size(), locked});
        }
        
        if (!lobby_found) {
            std::vector<std::string> users;
            redis_->smembers("room:users:lobby", users);
            redis_rooms.push_back({"lobby", users.size(), false});
        }
    }

    {
        // 2. 로컬 상태 처리 (Lock 필요)
        std::lock_guard<std::mutex> lk(state_.mu);
        
        // 로컬 방 정리
        std::vector<std::string> to_remove;
        for (auto it = state_.rooms.begin(); it != state_.rooms.end(); ++it) {
            std::uint32_t alive = 0; for (auto wit = it->second.begin(); wit != it->second.end(); ) { if (auto p = wit->lock()) { ++alive; ++wit; } else { wit = it->second.erase(wit); } }
            if (alive == 0 && it->first != std::string("lobby")) { to_remove.push_back(it->first); continue; }
        }
        for (auto& name : to_remove) { state_.rooms.erase(name); state_.room_passwords.erase(name); }

        if (redis_available) {
            // Redis 데이터 기반으로 메시지 구성
            for (auto& info : redis_rooms) {
                auto* ri = pb.add_rooms(); 
                ri->set_name(info.name); 
                ri->set_members(info.count);
                
                bool locked = info.is_locked;
                if (!locked && state_.room_passwords.count(info.name)) {
                    locked = true;
                }
                ri->set_locked(locked);
            }
        } else {
            // Fallback to local state
            for (auto it = state_.rooms.begin(); it != state_.rooms.end(); ++it) {
                std::size_t alive = it->second.size();
                auto* ri = pb.add_rooms(); ri->set_name(it->first); ri->set_members(alive); ri->set_locked(state_.room_passwords.count(it->first) > 0);
            }
        }
    }

    // 3. 현재 방 유저 목록 조회 (Redis 우선, Fallback 로컬)
    {
        std::vector<std::string> user_list;
        bool loaded_from_redis = false;
        
        if (redis_) {
            if (redis_->smembers("room:users:" + current, user_list)) {
                loaded_from_redis = true;
            }
        }
        
        if (loaded_from_redis) {
            for (const auto& name : user_list) {
                pb.add_users(name);
            }
        } else {
            // Fallback to local state
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
    }
    
    // 최근 메시지를 Redis에서 우선 조회하고, 부족하면 DB에서 가져옵니다 (Fallback).
    std::string rid;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        auto it = state_.room_ids.find(current);
        if (it != state_.room_ids.end()) {
            rid = it->second;
        }
    }
    if (rid.empty() && db_pool_) {
        rid = ensure_room_id_ci(current);
    }

    std::unordered_set<std::uint64_t> added_ids;
    bool loaded_from_cache = false;
    std::size_t cached_messages = 0;
    if (redis_ && !rid.empty()) {
        std::vector<server::wire::v1::StateSnapshot::SnapshotMessage> cached;
        if (load_recent_messages_from_cache(rid, cached)) {
            for (auto& message : cached) {
                if (added_ids.count(message.id())) continue; // 중복 제거
                auto* sm = pb.add_messages();
                *sm = message;
                added_ids.insert(message.id());
            }
            cached_messages = cached.size();
            loaded_from_cache = (cached_messages >= history_.recent_limit);
        }
    }

    std::uint64_t last_seen_value = 0;
    bool last_seen_loaded = false;
    if (db_pool_ && !rid.empty()) {
        try {
            std::string uid;
            {
                std::lock_guard<std::mutex> lk(state_.mu);
                auto itu = state_.user_uuid.find(&s);
                if (itu != state_.user_uuid.end()) uid = itu->second;
            }

            auto uow = db_pool_->make_unit_of_work();
            if (!uid.empty()) {
                auto opt = uow->memberships().get_last_seen(uid, rid);
                last_seen_value = opt.value_or(0);
            }
            last_seen_loaded = true;
            pb.set_last_seen_id(last_seen_value);

            // 캐시된 메시지가 부족하면 DB에서 추가로 조회합니다.
            if (!loaded_from_cache || cached_messages < history_.recent_limit) {
                const std::size_t limit = history_.recent_limit;
                const std::size_t fetch_factor = history_.fetch_factor;
                const std::size_t fetch_span = limit * fetch_factor;
                const std::size_t fetch_count = std::min(history_.max_list_len, std::max(limit, fetch_span));

                auto last_id = uow->messages().get_last_id(rid);
                std::uint64_t since_id = 0;
                // 마지막으로 읽은 메시지(last_seen)를 기준으로 가져올 범위를 계산합니다.
                    if (last_id > 0) {
                        // last_seen_value(마지막으로 읽은 메시지)를 기준으로 Fetch 범위를 결정합니다.
                        // 1. last_seen이 없으면(0) 최신 N개를 가져옵니다.
                        // 2. last_seen이 너무 오래되어 Gap이 크면, 중간을 건너뛰고 최신 메시지 위주로 가져옵니다.
                        // 3. 정상적인 경우 last_seen 직후부터 가져옵니다.
                        if (last_seen_value == 0) {
                            since_id = (last_id > limit) ? (last_id - limit) : 0;
                        } else if (last_seen_value >= last_id) {
                            since_id = (last_id > limit) ? (last_id - limit) : 0;
                        } else {
                            std::uint64_t context = static_cast<std::uint64_t>(limit) * static_cast<std::uint64_t>(fetch_factor);
                            if (last_id > context) {
                                std::uint64_t cut = last_id - context;
                                since_id = (last_seen_value > cut) ? last_seen_value : cut;
                            } else {
                                since_id = last_seen_value;
                            }
                        }
                    }

                auto msgs = uow->messages().fetch_recent_by_room(rid, since_id, fetch_count);
                // DB에서 가져온 것 중 이미 캐시에 있는 것은 제외
                std::vector<server::core::storage::Message> filtered;
                filtered.reserve(msgs.size());
                for (const auto& m : msgs) {
                    if (added_ids.find(m.id) == added_ids.end()) {
                        server::core::storage::Message m_copy = m;
                        filtered.push_back(std::move(m_copy));
                    }
                }
                msgs = std::move(filtered);

                if (msgs.size() > limit) {
                    msgs.erase(msgs.begin(), msgs.end() - static_cast<std::ptrdiff_t>(limit));
                }
                std::size_t budget = (cached_messages >= limit) ? 0 : (limit - cached_messages);
                if (budget == 0) {
                    msgs.clear();
                } else if (msgs.size() > budget) {
                    msgs.erase(msgs.begin(), msgs.end() - static_cast<std::ptrdiff_t>(budget));
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
                    // DB에서 가져온 메시지를 Redis 캐시에 채워넣습니다 (Read-Repair).
                    if (redis_) {
                        cache_recent_message(rid, *sm);
                    }
                    added_ids.insert(m.id);
                }
            }
        } catch (const std::exception& e) {
            corelog::warn(std::string("recent history DB fallback failed: ") + e.what());
        } catch (...) {
            corelog::warn("recent history DB fallback failed: unknown error");
        }
    }
    if (!last_seen_loaded) {
        pb.set_last_seen_id(last_seen_value);
    }

    // 현재 세션의 이름(닉네임)을 클라이언트에게 알려줌 (Guest 식별용)
    std::string my_name;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        auto it = state_.user.find(&s);
        if (it != state_.user.end()) my_name = it->second;
    }
    if (!my_name.empty()) {
        pb.set_your_name(my_name);
    }

    {
        std::string bytes; pb.SerializeToString(&bytes);
        body.assign(bytes.begin(), bytes.end());
    }
    s.async_send(game_proto::MSG_STATE_SNAPSHOT, body, 0);
}

// 외부에서 수신한 브로드캐스트(예: Redis Pub/Sub)를 해당 방의 로컬 세션들에게 전달합니다.
void ChatService::broadcast_room(const std::string& room, const std::vector<std::uint8_t>& body, Session* self) {
    std::vector<std::shared_ptr<Session>> targets;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        auto it = state_.rooms.find(room);
        if (it != state_.rooms.end()) {
            collect_room_sessions(it->second, targets);
        }
    }
    for (auto& t : targets) {
        int f = 0; // 재전파에서는 self 플래그를 사용하지 않는다.
        t->async_send(game_proto::MSG_CHAT_BROADCAST, body, f);
    }
}

// 시스템 공지 메시지를 전송합니다.
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
    s.async_send(game_proto::MSG_CHAT_BROADCAST, body, 0);
}

bool ChatService::maybe_handle_chat_hook_plugin(Session& s,
                                                const std::string& room,
                                                const std::string& sender,
                                                std::string& text) {
    if (!hook_plugin_) {
        return false;
    }

    auto out = hook_plugin_->chain.on_chat_send(s.session_id(), room, sender, text);
    for (const auto& notice : out.notices) {
        if (!notice.empty()) {
            send_system_notice(s, notice);
        }
    }
    return out.stop_default;
}

void ChatService::admin_disconnect_users(const std::vector<std::string>& users, const std::string& reason) {
    if (users.empty()) {
        return;
    }

    std::vector<std::string> targets;
    targets.reserve(users.size());
    for (const auto& user : users) {
        if (!user.empty()) {
            targets.push_back(user);
        }
    }
    if (targets.empty()) {
        return;
    }

    const std::string notice = reason;
    if (!job_queue_.TryPush([this, targets = std::move(targets), notice]() {
        std::vector<std::shared_ptr<Session>> sessions;
        std::unordered_set<Session*> seen;

        {
            std::lock_guard<std::mutex> lk(state_.mu);
            for (const auto& user : targets) {
                auto itset = state_.by_user.find(user);
                if (itset == state_.by_user.end()) {
                    continue;
                }

                for (auto wit = itset->second.begin(); wit != itset->second.end();) {
                    if (auto session = wit->lock()) {
                        if (!state_.authed.count(session.get()) || state_.guest.count(session.get())) {
                            ++wit;
                            continue;
                        }
                        if (seen.insert(session.get()).second) {
                            sessions.push_back(std::move(session));
                        }
                        ++wit;
                    } else {
                        wit = itset->second.erase(wit);
                    }
                }
            }
        }

        for (auto& session : sessions) {
            if (!notice.empty()) {
                send_system_notice(*session, notice);
            }
            session->stop();
        }

        corelog::info("admin disconnect applied: requested=" + std::to_string(targets.size()) +
                      " disconnected=" + std::to_string(sessions.size()));
    })) {
        corelog::warn("admin disconnect dropped: job queue full");
    }
}

void ChatService::admin_broadcast_notice(const std::string& text) {
    if (text.empty()) {
        return;
    }

    const std::string notice = text;
    if (!job_queue_.TryPush([this, notice]() {
        std::vector<std::shared_ptr<Session>> sessions;
        std::unordered_set<Session*> seen;

        {
            std::lock_guard<std::mutex> lk(state_.mu);
            for (auto& [_, room_set] : state_.rooms) {
                for (auto wit = room_set.begin(); wit != room_set.end();) {
                    if (auto session = wit->lock()) {
                        if (!state_.authed.count(session.get()) || state_.guest.count(session.get())) {
                            ++wit;
                            continue;
                        }
                        if (seen.insert(session.get()).second) {
                            sessions.push_back(std::move(session));
                        }
                        ++wit;
                    } else {
                        wit = room_set.erase(wit);
                    }
                }
            }
        }

        for (auto& session : sessions) {
            send_system_notice(*session, notice);
        }

        corelog::info("admin announcement delivered: sessions=" + std::to_string(sessions.size()));
    })) {
        corelog::warn("admin announcement dropped: job queue full");
    }
}

void ChatService::admin_apply_runtime_setting(const std::string& key, const std::string& value) {
    if (key.empty() || value.empty()) {
        return;
    }

    if (!job_queue_.TryPush([this, key, value]() {
        auto trim_ascii_local = [](std::string_view input) {
            std::size_t begin = 0;
            while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
                ++begin;
            }
            std::size_t end = input.size();
            while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
                --end;
            }
            return std::string(input.substr(begin, end - begin));
        };

        auto to_lower_ascii_local = [](std::string_view input) {
            std::string out(input);
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return out;
        };

        auto parse_u32 = [](std::string_view raw) -> std::optional<std::uint32_t> {
            if (raw.empty()) {
                return std::nullopt;
            }
            try {
                std::size_t pos = 0;
                const auto parsed = std::stoull(std::string(raw), &pos, 10);
                if (pos != raw.size() || parsed > std::numeric_limits<std::uint32_t>::max()) {
                    return std::nullopt;
                }
                return static_cast<std::uint32_t>(parsed);
            } catch (...) {
                return std::nullopt;
            }
        };

        const std::string normalized_key = to_lower_ascii_local(trim_ascii_local(key));
        const std::string normalized_value = trim_ascii_local(value);
        const auto parsed = parse_u32(normalized_value);
        if (!parsed) {
            corelog::warn("admin setting rejected: key=" + normalized_key + " reason=invalid_value");
            return;
        }

        if (normalized_key == "presence_ttl_sec") {
            if (*parsed < 5 || *parsed > 3600) {
                corelog::warn("admin setting rejected: key=presence_ttl_sec reason=out_of_range");
                return;
            }
            presence_.ttl = *parsed;
            corelog::info("admin setting applied: key=presence_ttl_sec value=" + std::to_string(*parsed));
            return;
        }

        if (normalized_key == "recent_history_limit") {
            if (*parsed < 5 || *parsed > 2000) {
                corelog::warn("admin setting rejected: key=recent_history_limit reason=out_of_range");
                return;
            }
            history_.recent_limit = static_cast<std::size_t>(*parsed);
            if (history_.max_list_len < history_.recent_limit) {
                history_.max_list_len = history_.recent_limit;
            }
            corelog::info("admin setting applied: key=recent_history_limit value=" + std::to_string(*parsed));
            return;
        }

        if (normalized_key == "room_recent_maxlen") {
            const auto min_allowed = static_cast<std::uint32_t>(history_.recent_limit);
            if (*parsed < min_allowed || *parsed > 5000) {
                corelog::warn("admin setting rejected: key=room_recent_maxlen reason=out_of_range");
                return;
            }
            history_.max_list_len = static_cast<std::size_t>(*parsed);
            corelog::info("admin setting applied: key=room_recent_maxlen value=" + std::to_string(*parsed));
            return;
        }

        corelog::warn("admin setting rejected: key=" + normalized_key + " reason=unsupported_key");
    })) {
        corelog::warn("admin setting dropped: job queue full");
    }
}

// 귓속말 전송 결과를 클라이언트에게 알립니다.
void ChatService::send_whisper_result(Session& s, bool ok, const std::string& reason) {
    server::wire::v1::WhisperResult pb;
    pb.set_ok(ok);
    if (!reason.empty()) {
        pb.set_reason(reason);
    }
    std::string bytes;
    pb.SerializeToString(&bytes);
    std::vector<std::uint8_t> body(bytes.begin(), bytes.end());
    s.async_send(game_proto::MSG_WHISPER_RES, body, 0);
}

void ChatService::deliver_remote_whisper(const std::vector<std::uint8_t>& body) {
    if (body.empty()) {
        return;
    }

    server::wire::v1::WhisperNotice notice;
    if (!notice.ParseFromArray(body.data(), static_cast<int>(body.size()))) {
        corelog::warn("[whisper] failed to parse remote payload");
        return;
    }

    if (notice.sender().empty() || notice.recipient().empty() || notice.text().empty()) {
        corelog::warn("[whisper] invalid remote payload fields");
        return;
    }

    std::vector<std::shared_ptr<Session>> targets;
    {
        std::lock_guard<std::mutex> lk(state_.mu);
        auto itset = state_.by_user.find(notice.recipient());
        if (itset != state_.by_user.end()) {
            for (auto wit = itset->second.begin(); wit != itset->second.end(); ) {
                if (auto p = wit->lock()) {
                    if (!state_.authed.count(p.get()) || state_.guest.count(p.get())) {
                        ++wit;
                        continue;
                    }
                    targets.emplace_back(std::move(p));
                    ++wit;
                } else {
                    wit = itset->second.erase(wit);
                }
            }
        }
    }

    if (targets.empty()) {
        return;
    }

    notice.set_outgoing(false);
    std::string incoming_bytes;
    if (!notice.SerializeToString(&incoming_bytes)) {
        return;
    }
    std::vector<std::uint8_t> incoming(incoming_bytes.begin(), incoming_bytes.end());
    for (auto& target : targets) {
        target->async_send(game_proto::MSG_WHISPER_BROADCAST, incoming, 0);
    }

    corelog::info("[whisper] sender=" + notice.sender() +
                  " target=" + notice.recipient() +
                  " status=remote_delivered count=" + std::to_string(targets.size()));
}

// 귓속말(1:1 채팅)을 처리합니다.
// 대상 사용자가 같은 서버에 있으면 직접 전송하고, 없으면 Redis Pub/Sub을 통해 전파할 수도 있습니다(현재 구현은 로컬 우선).
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

    auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    server::wire::v1::WhisperNotice notice;
    notice.set_sender(sender);
    notice.set_recipient(target_user);
    notice.set_text(text);
    notice.set_ts_ms(static_cast<std::uint64_t>(now64));

    const auto send_outgoing = [&]() {
        notice.set_outgoing(true);
        std::string outgoing_bytes;
        notice.SerializeToString(&outgoing_bytes);
        std::vector<std::uint8_t> outgoing(outgoing_bytes.begin(), outgoing_bytes.end());
        session_sp->async_send(game_proto::MSG_WHISPER_BROADCAST, outgoing, 0);
    };

    if (targets.empty()) {
        bool routed_remote = false;
        if (redis_) {
            try {
                if (const char* use = std::getenv("USE_REDIS_PUBSUB"); use && std::strcmp(use, "0") != 0) {
                    notice.set_outgoing(false);
                    std::string incoming_bytes;
                    if (notice.SerializeToString(&incoming_bytes)) {
                        std::string message;
                        message.reserve(3 + gateway_id_.size() + 1 + incoming_bytes.size());
                        message.append("gw=").append(gateway_id_);
                        message.push_back('\n');
                        message.append(incoming_bytes);
                        const std::string channel = presence_.prefix + std::string("fanout:whisper");
                        routed_remote = redis_->publish(channel, std::move(message));
                    }
                }
            } catch (...) {}
        }

        if (!routed_remote) {
            send_system_notice(*session_sp, "user not found: " + target_user);
            send_whisper_result(*session_sp, false, "user not found");
            corelog::info("[whisper] sender=" + sender + " target=" + target_user + " status=not_found");
            return;
        }

        send_outgoing();
        send_whisper_result(*session_sp, true, "");
        corelog::info("[whisper] sender=" + sender + " target=" + target_user + " status=remote_routed");
        return;
    }

    notice.set_outgoing(false);
    std::string incoming_bytes;
    notice.SerializeToString(&incoming_bytes);
    std::vector<std::uint8_t> incoming(incoming_bytes.begin(), incoming_bytes.end());
    for (auto& target : targets) {
        target->async_send(game_proto::MSG_WHISPER_BROADCAST, incoming, 0);
    }

    send_outgoing();

    send_whisper_result(*session_sp, true, "");
}

// 방 비밀번호 해싱 (간단한 std::hash 사용, 실제 서비스에선 더 강력한 해시 권장)
std::string ChatService::hash_room_password(const std::string& password) {
    std::hash<std::string> hasher;
    std::size_t value = hasher(password);
    std::ostringstream oss;
    oss << std::hex << value;
    return oss.str();
}

// 방 이름으로 Room ID(UUID)를 조회하거나 생성합니다. (Case-Insensitive)
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


std::string ChatService::make_recent_list_key(const std::string& room_id) const {
    return std::string("room:") + room_id + ":recent";
}

std::string ChatService::make_recent_message_key(std::uint64_t message_id) const {
    return std::string("msg:") + std::to_string(message_id);
}

// 최근 메시지를 Redis 캐시에 저장합니다. (LIST + Key-Value)
bool ChatService::cache_recent_message(
    const std::string& room_id,
    const server::wire::v1::StateSnapshot::SnapshotMessage& message) {
    if (!redis_ || room_id.empty() || message.id() == 0) {
        return false;
    }
    std::string payload;
    if (!message.SerializeToString(&payload)) {
        return false;
    }
    if (!redis_->setex(make_recent_message_key(message.id()), payload, history_.cache_ttl_sec)) {
        return false;
    }
    return redis_->lpush_trim(make_recent_list_key(room_id),
                              std::to_string(message.id()),
                              history_.max_list_len);
}

// Redis 캐시에서 최근 메시지 목록을 로드합니다.
bool ChatService::load_recent_messages_from_cache(
    const std::string& room_id,
    std::vector<server::wire::v1::StateSnapshot::SnapshotMessage>& out) {
    if (!redis_ || room_id.empty() || history_.recent_limit == 0) {
        return false;
    }
    std::vector<std::string> ids;
    // LPUSH를 사용하므로 리스트의 앞부분(0)이 가장 최신 메시지입니다.
    // 따라서 최신 N개를 가져오려면 0부터 N-1까지 읽어야 합니다.
    // (기존 코드는 -limit ~ -1로 읽어서 가장 오래된 메시지를 가져오는 버그가 있었음)
    if (!redis_->lrange(make_recent_list_key(room_id), 0, static_cast<long long>(history_.recent_limit) - 1, ids)) {
        return false;
    }
    if (ids.empty()) {
        return false;
    }

    std::vector<std::uint64_t> valid_ids;
    valid_ids.reserve(ids.size());

    bool partial_hit = false;
    for (const auto& id_str : ids) {
        char* endptr = nullptr;
        auto value = std::strtoull(id_str.c_str(), &endptr, 10);
        if (endptr == id_str.c_str() || value == 0) {
            partial_hit = true;
            corelog::warn("recent history cache miss: invalid id entry=" + id_str);
            continue;
        }
        valid_ids.emplace_back(value);
    }

    if (valid_ids.empty()) {
        return false;
    }

    std::vector<server::wire::v1::StateSnapshot::SnapshotMessage> parsed;
    parsed.reserve(valid_ids.size());

    bool batch_loaded = false;
    {
        std::vector<std::string> keys;
        keys.reserve(valid_ids.size());
        for (const auto id : valid_ids) {
            keys.emplace_back(make_recent_message_key(id));
        }

        std::vector<std::optional<std::string>> payloads;
        if (redis_->mget(keys, payloads) && payloads.size() == keys.size()) {
            batch_loaded = true;
            for (std::size_t i = 0; i < payloads.size(); ++i) {
                if (!payloads[i].has_value()) {
                    partial_hit = true;
                    continue;
                }
                server::wire::v1::StateSnapshot::SnapshotMessage message;
                if (!message.ParseFromString(*payloads[i])) {
                    partial_hit = true;
                    corelog::warn("recent history cache miss: corrupted payload");
                    continue;
                }
                parsed.emplace_back(std::move(message));
            }
        }
    }

    if (!batch_loaded) {
        for (const auto id : valid_ids) {
            auto payload = redis_->get(make_recent_message_key(id));
            if (!payload) {
                partial_hit = true;
                continue;
            }
            server::wire::v1::StateSnapshot::SnapshotMessage message;
            if (!message.ParseFromString(*payload)) {
                partial_hit = true;
                corelog::warn("recent history cache miss: corrupted payload");
                continue;
            }
            parsed.emplace_back(std::move(message));
        }
    }
    if (parsed.empty()) {
        return false;
    }
    if (partial_hit) {
        corelog::info("recent history cache partial hit: room_id=" + room_id +
                      " kept=" + std::to_string(parsed.size()) +
                      " requested=" + std::to_string(ids.size()));
    }
    std::reverse(parsed.begin(), parsed.end());
    out = std::move(parsed);
    return true;
}

} // namespace server::app::chat
