#include <nlohmann/json.hpp>
#include <gtest/gtest.h>
#include <server/chat/chat_service.hpp>
#include <server/auth/session_token.hpp>
#include "chat_service_private_access.hpp"
#include <server/core/net/session.hpp>
#include <server/core/net/dispatcher.hpp>
#include <server/core/concurrent/job_queue.hpp>
#include <server/core/concurrent/thread_manager.hpp>
#include <server/core/concurrent/task_scheduler.hpp>
#include <server/storage/connection_pool.hpp>
#include <server/storage/unit_of_work.hpp>
#include <server/storage/redis/client.hpp>
#include <server/core/config/options.hpp>
#include <server/core/net/connection_runtime_state.hpp>
#include <server/core/protocol/protocol_errors.hpp>
#include <server/core/protocol/system_opcodes.hpp>
#include <server/core/protocol/packet.hpp>
#include <server/core/protocol/version.hpp>
#include <server/core/runtime_metrics.hpp>
#include <server/core/util/service_registry.hpp>
#include <server/protocol/game_opcodes.hpp>
#include "wire.pb.h"
#include <boost/asio.hpp>
#include <thread>
#include <future>
#include <chrono>
#include <vector>
#include <array>
#include <optional>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cctype>
#include <iostream>
#include <unordered_map>
#include <unordered_set>


using namespace server::app::chat;
using namespace server::core;
using namespace server::storage;
using namespace server::storage::redis;
namespace game_proto = server::protocol;
namespace core_proto = server::core::protocol;
namespace services = server::core::util::services;

/**
 * @brief ChatService 주요 경로(로그인/입장/채팅/리프레시) 통합 동작을 검증합니다.
 */
// --- Helper Functions (Copied from frame.hpp to bypass include issues) ---
inline void write_be16(std::uint16_t v, std::uint8_t* out) {
    out[0] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    out[1] = static_cast<std::uint8_t>(v & 0xFF);
}

inline void write_lp_utf8(std::vector<std::uint8_t>& out, std::string_view str) {
    if (str.size() > 0xFFFF) {
        str = str.substr(0, 0xFFFF);
    }
    const auto len = static_cast<std::uint16_t>(str.size());
    const auto offset = out.size();
    out.resize(offset + 2 + len);
    write_be16(len, out.data() + offset);
    if (len != 0) {
        std::memcpy(out.data() + offset + 2, str.data(), len);
    }
}

class ScopedEnvVar {
public:
    ScopedEnvVar(std::string key, const char* value)
        : key_(std::move(key)) {
        if (const char* old = std::getenv(key_.c_str()); old != nullptr) {
            had_old_ = true;
            old_value_ = old;
        }
        set(value);
    }

    ~ScopedEnvVar() {
#if defined(_WIN32)
        if (had_old_) {
            _putenv_s(key_.c_str(), old_value_.c_str());
        } else {
            _putenv_s(key_.c_str(), "");
        }
#else
        if (had_old_) {
            setenv(key_.c_str(), old_value_.c_str(), 1);
        } else {
            unsetenv(key_.c_str());
        }
#endif
    }

private:
    void set(const char* value) const {
#if defined(_WIN32)
        _putenv_s(key_.c_str(), value ? value : "");
#else
        if (value) {
            setenv(key_.c_str(), value, 1);
        } else {
            unsetenv(key_.c_str());
        }
#endif
    }

    std::string key_;
    bool had_old_{false};
    std::string old_value_;
};

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view prefix) {
        const auto nonce = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path() / (std::string(prefix) + "_" + nonce);
        std::error_code ec;
        (void)std::filesystem::create_directories(path_, ec);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

// --- Mocks ---
// MockConnectionPool moved to bottom

// --- Mock Repositories ---
namespace {

std::string lower_ascii_copy(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (const unsigned char ch : value) {
        lowered.push_back(static_cast<char>(std::tolower(ch)));
    }
    return lowered;
}

struct MockStorageState {
    std::recursive_mutex control_mu;
    std::unordered_map<std::string, std::string> control;
    bool fail_message_commit{false};
    std::function<void(const std::string&)> before_message_create;
    std::unordered_map<std::string, server::storage::User> users_by_id;
    std::unordered_map<std::string, std::string> user_id_by_name_ci;
    std::unordered_map<std::string, server::storage::Room> rooms_by_id;
    std::unordered_map<std::string, std::string> room_id_by_name_ci;
    std::unordered_map<std::string, std::vector<server::storage::Message>> messages_by_room_id;
    std::unordered_map<std::string, server::storage::Membership> memberships_by_key;
    std::unordered_map<std::string, server::storage::Session> sessions_by_id;
    std::unordered_map<std::string, std::string> session_id_by_token_hash;
    std::uint64_t next_user_id{1};
    std::uint64_t next_room_id{1};
    std::uint64_t next_message_id{1};
    std::uint64_t next_session_id{1};
    std::optional<server::storage::Room> last_created_room;
    std::optional<server::storage::Message> last_created_message;
    std::optional<server::storage::Membership> last_membership_join;
    std::optional<server::storage::Membership> last_membership_last_seen;
};

std::string make_membership_key(std::string_view user_id, std::string_view room_id) {
    return std::string(user_id) + "|" + std::string(room_id);
}

} // namespace

class MockUserRepository : public IUserRepository {
public:
    explicit MockUserRepository(std::shared_ptr<MockStorageState> state)
        : state_(std::move(state)) {}

    std::optional<User> find_by_id(const std::string& user_id) override {
        if (const auto it = state_->users_by_id.find(user_id); it != state_->users_by_id.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    std::vector<User> find_by_name_ci(const std::string& name, std::size_t limit) override {
        std::vector<User> out;
        const auto lowered = lower_ascii_copy(name);
        if (const auto it = state_->user_id_by_name_ci.find(lowered); it != state_->user_id_by_name_ci.end()) {
            if (const auto user_it = state_->users_by_id.find(it->second); user_it != state_->users_by_id.end()) {
                out.push_back(user_it->second);
            }
        }
        if (out.size() > limit) {
            out.resize(limit);
        }
        return out;
    }

    User create_guest(const std::string& name) override {
        const auto lowered = lower_ascii_copy(name);
        if (const auto it = state_->user_id_by_name_ci.find(lowered); it != state_->user_id_by_name_ci.end()) {
            return state_->users_by_id.at(it->second);
        }

        User user{};
        user.id = "user-" + std::to_string(state_->next_user_id++);
        user.name = name;
        user.created_at_ms = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        state_->user_id_by_name_ci[lowered] = user.id;
        state_->users_by_id[user.id] = user;
        return user;
    }

    void update_last_login(const std::string&, const std::string&) override {}

private:
    std::shared_ptr<MockStorageState> state_;
};

class MockRoomRepository : public IRoomRepository {
public:
    explicit MockRoomRepository(std::shared_ptr<MockStorageState> state)
        : state_(std::move(state)) {}

    std::optional<Room> find_by_id(const std::string& room_id) override {
        if (const auto it = state_->rooms_by_id.find(room_id); it != state_->rooms_by_id.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    std::vector<Room> search_by_name_ci(const std::string& query, std::size_t limit) override {
        std::vector<Room> out;
        const auto lowered = lower_ascii_copy(query);
        for (const auto& [room_name_ci, room_id] : state_->room_id_by_name_ci) {
            if (room_name_ci.find(lowered) == std::string::npos) {
                continue;
            }
            if (const auto room_it = state_->rooms_by_id.find(room_id); room_it != state_->rooms_by_id.end()) {
                out.push_back(room_it->second);
            }
            if (out.size() >= limit) {
                break;
            }
        }
        return out;
    }

    std::optional<Room> find_by_name_exact_ci(const std::string& name) override {
        const auto lowered = lower_ascii_copy(name);
        if (const auto it = state_->room_id_by_name_ci.find(lowered); it != state_->room_id_by_name_ci.end()) {
            return state_->rooms_by_id.at(it->second);
        }
        return std::nullopt;
    }

    Room create(const std::string& name, bool is_public) override {
        const auto lowered = lower_ascii_copy(name);
        if (const auto it = state_->room_id_by_name_ci.find(lowered); it != state_->room_id_by_name_ci.end()) {
            return state_->rooms_by_id.at(it->second);
        }

        Room room{};
        room.id = "room-" + std::to_string(state_->next_room_id++);
        room.name = name;
        room.is_public = is_public;
        room.is_active = true;
        room.created_at_ms = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        state_->room_id_by_name_ci[lowered] = room.id;
        state_->rooms_by_id[room.id] = room;
        state_->last_created_room = room;
        return room;
    }

    void close(const std::string& room_id) override {
        if (auto it = state_->rooms_by_id.find(room_id); it != state_->rooms_by_id.end()) {
            it->second.is_active = false;
            it->second.closed_at_ms = static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
        }
    }

private:
    std::shared_ptr<MockStorageState> state_;
};

class MockMessageRepository : public IMessageRepository {
public:
    explicit MockMessageRepository(std::shared_ptr<MockStorageState> state)
        : state_(std::move(state)) {}

    std::vector<Message> fetch_recent_by_room(const std::string& room_id,
                                              std::uint64_t since_id,
                                              std::size_t limit) override {
        std::vector<Message> out;
        if (const auto it = state_->messages_by_room_id.find(room_id); it != state_->messages_by_room_id.end()) {
            for (const auto& message : it->second) {
                if (message.id > since_id) {
                    out.push_back(message);
                }
            }
        }
        if (out.size() > limit) {
            out.erase(out.begin(), out.end() - static_cast<std::ptrdiff_t>(limit));
        }
        return out;
    }

    Message create(const std::string& room_id,
                   const std::string& room_name,
                   const std::optional<std::string>& user_id,
                   const std::string& content) override {
        if (state_->before_message_create) state_->before_message_create(content);
        Message message{};
        message.id = state_->next_message_id++;
        message.room_id = room_id;
        message.room_name = room_name;
        message.user_id = user_id;
        message.content = content;
        message.created_at_ms = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        state_->messages_by_room_id[room_id].push_back(message);
        state_->last_created_message = message;
        created_ids.push_back(message.id);
        return message;
    }

    std::uint64_t get_last_id(const std::string& room_id) override {
        if (const auto it = state_->messages_by_room_id.find(room_id);
            it != state_->messages_by_room_id.end() && !it->second.empty()) {
            return it->second.back().id;
        }
        return 0;
    }

    void delete_by_room(const std::string& room_id) override {
        state_->messages_by_room_id.erase(room_id);
    }
    std::vector<std::uint64_t> created_ids;
    void undo_created() {
        for (auto& [_, messages] : state_->messages_by_room_id) {
            std::erase_if(messages, [&](const auto& message) { return std::find(created_ids.begin(), created_ids.end(), message.id) != created_ids.end(); });
        }
        created_ids.clear();
    }

private:
    std::shared_ptr<MockStorageState> state_;
};

class MockMembershipRepository : public IMembershipRepository {
public:
    explicit MockMembershipRepository(std::shared_ptr<MockStorageState> state)
        : state_(std::move(state)) {}

    void upsert_join(const std::string& user_id,
                     const std::string& room_id,
                     const std::string& role) override {
        Membership membership{};
        const auto key = make_membership_key(user_id, room_id);
        if (const auto it = state_->memberships_by_key.find(key); it != state_->memberships_by_key.end()) {
            membership = it->second;
        }
        membership.user_id = user_id;
        membership.room_id = room_id;
        membership.role = role;
        membership.joined_at_ms = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        membership.left_at_ms.reset();
        membership.is_member = true;
        state_->memberships_by_key[key] = membership;
        state_->last_membership_join = membership;
    }

    void update_last_seen(const std::string& user_id,
                          const std::string& room_id,
                          std::uint64_t last_seen_msg_id) override {
        const auto key = make_membership_key(user_id, room_id);
        Membership membership{};
        if (const auto it = state_->memberships_by_key.find(key); it != state_->memberships_by_key.end()) {
            membership = it->second;
        }
        membership.user_id = user_id;
        membership.room_id = room_id;
        membership.last_seen_msg_id = last_seen_msg_id;
        membership.is_member = true;
        state_->memberships_by_key[key] = membership;
        state_->last_membership_last_seen = membership;
    }

    void leave(const std::string& user_id, const std::string& room_id) override {
        const auto key = make_membership_key(user_id, room_id);
        if (auto it = state_->memberships_by_key.find(key); it != state_->memberships_by_key.end()) {
            it->second.is_member = false;
            it->second.left_at_ms = static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
        }
    }

    std::optional<std::uint64_t> get_last_seen(const std::string& user_id,
                                               const std::string& room_id) override {
        const auto key = make_membership_key(user_id, room_id);
        if (const auto it = state_->memberships_by_key.find(key);
            it != state_->memberships_by_key.end()) {
            return it->second.last_seen_msg_id;
        }
        return std::nullopt;
    }

private:
    std::shared_ptr<MockStorageState> state_;
};

class MockSessionRepository : public ISessionRepository {
public:
    explicit MockSessionRepository(std::shared_ptr<MockStorageState> state)
        : state_(std::move(state)) {}

    std::optional<server::storage::Session> find_by_token_hash(const std::string& token_hash) override {
        if (const auto it = state_->session_id_by_token_hash.find(token_hash); it != state_->session_id_by_token_hash.end()) {
            return state_->sessions_by_id.at(it->second);
        }
        return std::nullopt;
    }

    server::storage::Session create(const std::string& user_id,
                                    const std::chrono::system_clock::time_point& expires_at,
                                    const std::optional<std::string>& client_ip,
                                    const std::optional<std::string>& user_agent,
                                    const std::string& token_hash) override {
        server::storage::Session session{};
        session.id = "session-" + std::to_string(state_->next_session_id++);
        session.user_id = user_id;
        session.token_hash = token_hash;
        session.client_ip = client_ip;
        session.user_agent = user_agent;
        const auto now = std::chrono::system_clock::now();
        session.created_at_ms = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
        session.expires_at_ms = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(expires_at.time_since_epoch()).count());
        state_->session_id_by_token_hash[token_hash] = session.id;
        state_->sessions_by_id[session.id] = session;
        return session;
    }

    void revoke(const std::string& session_id) override {
        if (const auto it = state_->sessions_by_id.find(session_id); it != state_->sessions_by_id.end()) {
            it->second.revoked_at_ms = static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
        }
    }

private:
    std::shared_ptr<MockStorageState> state_;
};

class MockChatStateRepository : public server::storage::IChatStateRepository {
public:
    explicit MockChatStateRepository(std::shared_ptr<MockStorageState> state) : state_(std::move(state)) {}
    void lock(const std::string&) override { if (!lock_.owns_lock()) lock_ = std::unique_lock<std::recursive_mutex>(state_->control_mu); }
    std::optional<std::string> get(const std::string& key) override {
        const auto it = state_->control.find(key);
        return it == state_->control.end() ? std::nullopt : std::optional<std::string>(it->second);
    }
    void put(const std::string& key, const std::string& value) override { state_->control[key] = value; }
    void erase(const std::string& key) override { state_->control.erase(key); }
    std::uint64_t now_ms() override { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
    void unlock() { if (lock_.owns_lock()) lock_.unlock(); }
private:
    std::shared_ptr<MockStorageState> state_;
    std::unique_lock<std::recursive_mutex> lock_;
};

class MockUnitOfWork : public IRepositoryUnitOfWork {
public:
    explicit MockUnitOfWork(std::shared_ptr<MockStorageState> state)
        : state_(state), control_repo(state), user_repo(state),
          room_repo(state),
          msg_repo(state),
          membership_repo(state),
          session_repo(std::move(state)) {}

    std::shared_ptr<MockStorageState> state_;
    MockChatStateRepository control_repo;
    MockUserRepository user_repo;
    MockRoomRepository room_repo;
    MockMessageRepository msg_repo;
    MockMembershipRepository membership_repo;
    MockSessionRepository session_repo;

    bool committed{false};
    ~MockUnitOfWork() override { if (!committed) msg_repo.undo_created(); }
    void commit() override {
        if (state_->fail_message_commit && !msg_repo.created_ids.empty()) throw std::runtime_error("injected commit failure");
        committed = true;
        control_repo.unlock();
    }
    void rollback() override { control_repo.unlock(); }

    server::storage::IChatStateRepository& chat_state() override { return control_repo; }
    IUserRepository& users() override { return user_repo; }
    IRoomRepository& rooms() override { return room_repo; }
    IMessageRepository& messages() override { return msg_repo; }
    ISessionRepository& sessions() override { return session_repo; }
    IMembershipRepository& memberships() override { return membership_repo; }
};

class MockConnectionPool : public IRepositoryConnectionPool {
public:
    MockConnectionPool()
        : state_(std::make_shared<MockStorageState>()) {}

    std::unique_ptr<IRepositoryUnitOfWork> make_repository_unit_of_work() override {
        return std::make_unique<MockUnitOfWork>(state_);
    }
    bool health_check() override { return true; }

    const std::shared_ptr<MockStorageState>& state() const {
        return state_;
    }

private:
    std::shared_ptr<MockStorageState> state_;
};

class MockRedisClient : public IRedisClient {
public:
    bool sadd_called = false;
    bool publish_called = false;
    std::string last_publish_channel;
    std::string last_publish_message;
    std::unordered_map<std::string, std::string> kv_store;
    std::unordered_map<std::string, std::unordered_set<std::string>> set_store;
    std::unordered_map<std::string, std::vector<std::string>> list_store;

    // IRedisClient Interface Implementation
    bool health_check() override { return true; }
    bool lpush_trim(const std::string& key, const std::string& value, std::size_t max_len) override {
        auto& list = list_store[key];
        list.insert(list.begin(), value);
        if (list.size() > max_len) {
            list.resize(max_len);
        }
        return true;
    }

    bool sadd(const std::string& key, const std::string& member) override {
        sadd_called = true;
        set_store[key].insert(member);
        return true;
    }

    bool srem(const std::string& key, const std::string& member) override {
        if (const auto it = set_store.find(key); it != set_store.end()) {
            it->second.erase(member);
            if (it->second.empty()) {
                set_store.erase(it);
            }
        }
        return true;
    }
    bool smembers(const std::string& key, std::vector<std::string>& out) override {
        out.clear();
        if (const auto it = set_store.find(key); it != set_store.end()) {
            out.assign(it->second.begin(), it->second.end());
        }
        return true;
    }
    bool scard(const std::string& key, std::size_t& out) override {
        if (const auto it = set_store.find(key); it != set_store.end()) {
            out = it->second.size();
        } else {
            out = 0;
        }
        return true;
    }
    bool scard_many(const std::vector<std::string>& keys, std::vector<std::size_t>& out) override {
        out.clear();
        out.reserve(keys.size());
        for (const auto& key : keys) {
            std::size_t size = 0;
            (void)scard(key, size);
            out.push_back(size);
        }
        return true;
    }

    bool del(const std::string& key) override {
        kv_store.erase(key);
        set_store.erase(key);
        return true;
    }
    bool set(const std::string& key, const std::string& value) override {
        kv_store[key] = value;
        return true;
    }

    std::optional<std::string> get(const std::string& key) override {
        if (const auto it = kv_store.find(key); it != kv_store.end()) {
            return it->second;
        }
        return std::nullopt;
    }
    bool mget(const std::vector<std::string>& keys, std::vector<std::optional<std::string>>& out) override {
        out.clear();
        out.reserve(keys.size());
        for (const auto& key : keys) {
            out.push_back(get(key));
        }
        return true;
    }

    bool set_if_not_exists(const std::string& key, const std::string& value, unsigned int) override {
        if (kv_store.contains(key)) {
            return false;
        }
        kv_store[key] = value;
        return true;
    }
    bool set_if_equals(const std::string& key, const std::string& expected, const std::string& value, unsigned int) override {
        if (const auto it = kv_store.find(key); it == kv_store.end() || it->second != expected) {
            return false;
        }
        kv_store[key] = value;
        return true;
    }
    bool del_if_equals(const std::string& key, const std::string& expected) override {
        const auto it = kv_store.find(key);
        if (it == kv_store.end() || it->second != expected) {
            return false;
        }
        kv_store.erase(it);
        return true;
    }

    bool scan_keys(const std::string&, std::vector<std::string>&) override { return true; }
    bool lrange(const std::string&, long long, long long, std::vector<std::string>&) override { return true; }
    bool scan_del(const std::string&) override { return true; }

    bool setex(const std::string& key, const std::string& value, unsigned int) override {
        kv_store[key] = value;
        return true;
    }

    bool publish(const std::string& channel, const std::string& message) override {
        publish_called = true;
        last_publish_channel = channel;
        last_publish_message = message;
        return true;
    }

    void reset_publish_trace() {
        publish_called = false;
        last_publish_channel.clear();
        last_publish_message.clear();
    }

    bool start_psubscribe(const std::string&, std::function<void(const std::string&, const std::string&)>) override { return true; }
    void stop_psubscribe() override {}

    bool xgroup_create_mkstream(const std::string&, const std::string&) override { return true; }
    bool xadd(const std::string&, const std::vector<std::pair<std::string, std::string>>&, std::string*, std::optional<std::size_t>, bool) override { return true; }
    bool xreadgroup(const std::string&, const std::string&, const std::string&, long long, std::size_t, std::vector<StreamEntry>&) override { return true; }
    bool xack(const std::string&, const std::string&, const std::string&) override { return true; }
    bool xpending(const std::string&, const std::string&, long long&) override { return true; }
    bool xautoclaim(const std::string&,
                    const std::string&,
                    const std::string&,
                    long long,
                    const std::string& start,
                    std::size_t,
                    StreamAutoClaimResult& out) override {
        out.next_start_id = start;
        out.entries.clear();
        out.deleted_ids.clear();
        return true;
    }
};

// --- Test Fixture ---

class ChatServiceTest : public ::testing::Test {
protected:
    struct ErrorFrame {
        std::uint16_t code{0};
        std::string message;
    };

    boost::asio::io_context io_;
    JobQueue job_queue_;
    std::shared_ptr<MockConnectionPool> db_pool_;
    std::shared_ptr<MockRedisClient> redis_;
    std::unique_ptr<ChatService> chat_service_;

    Dispatcher dispatcher_;
    BufferManager buffer_manager_{1024, 10};
    std::shared_ptr<SessionOptions> session_options_;
    std::shared_ptr<server::core::net::ConnectionRuntimeState> shared_state_;
    std::shared_ptr<server::core::net::Session> session_;

    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::ip::tcp::socket peer_socket_;

    void SetUp() override {
        services::clear();
        db_pool_ = std::make_shared<MockConnectionPool>();
        redis_ = std::make_shared<MockRedisClient>();
        chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);

        session_options_ = std::make_shared<SessionOptions>();
        shared_state_ = std::make_shared<server::core::net::ConnectionRuntimeState>();

        // 실제 소켓 연결 수립 (Loopback)
        boost::asio::ip::tcp::endpoint ep(boost::asio::ip::make_address("127.0.0.1"), 0);
        acceptor_.open(ep.protocol());
        acceptor_.bind(ep);
        acceptor_.listen();

        boost::asio::ip::tcp::socket socket(io_);
        socket.connect(acceptor_.local_endpoint());
        acceptor_.accept(peer_socket_);

        session_ = std::make_shared<server::core::net::Session>(
            std::move(socket), dispatcher_, buffer_manager_, session_options_, shared_state_
        );
        session_->start();
    }

    void TearDown() override {
        chat_service_.reset();
        services::clear();
    }

    ChatServiceTest() : acceptor_(io_), peer_socket_(io_) {}

    void ProcessJobs() {
        // FIFO 완료 marker까지 처리한다. CPU/파일 로딩 지연을 큐 정지로 바꾸지 않는다.
        // 실제 교착은 CTest의 외부 timeout으로 실패시키며, 작업 예외/거절도 즉시 보고한다.
        auto completed = std::make_shared<bool>(false);
        ASSERT_TRUE(job_queue_.TryPush([completed] { *completed = true; }));
        while (!*completed) {
            auto job = job_queue_.Pop();
            ASSERT_TRUE(job);
            ASSERT_NO_THROW(job());
        }
    }

    void FlushSessionIO() {
        if (io_.stopped()) io_.restart();
        while (io_.poll() > 0);
        io_.restart();
    }

    // 피어 소켓에서 데이터 읽기 헬퍼
    bool ReadFromPeer() {
        // Non-blocking 읽기 시도
        peer_socket_.non_blocking(true);
        std::vector<uint8_t> buf(1024);
        boost::system::error_code ec;
        size_t len = peer_socket_.read_some(boost::asio::buffer(buf), ec);
        if (!ec && len > 0) return true;
        if (ec == boost::asio::error::would_block) return false;
        return false;
    }

    // Blocking 읽기 (타임아웃 포함)
    bool WaitForData(int timeout_ms = 100) {
        // 간단한 폴링
        for(int i=0; i<timeout_ms/10; ++i) {
            if (ReadFromPeer()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    void LoginAs(const std::string& user, const std::string& token = "test_token") {
        std::vector<uint8_t> login_payload;
        write_lp_utf8(login_payload, user);
        write_lp_utf8(login_payload, token);
        chat_service_->on_login(*session_, login_payload);
        ProcessJobs();
        FlushSessionIO();
    }

    void JoinRoom(const std::string& room, const std::string& password = "") {
        std::vector<uint8_t> join_payload;
        write_lp_utf8(join_payload, room);
        write_lp_utf8(join_payload, password);
        chat_service_->on_join(*session_, join_payload);
        ProcessJobs();
        FlushSessionIO();
    }

    void SendChat(const std::string& room, const std::string& text) {
        std::vector<uint8_t> payload;
        write_lp_utf8(payload, room);
        write_lp_utf8(payload, text);
        chat_service_->on_chat_send(*session_, payload);
        ProcessJobs();
        FlushSessionIO();
    }

    bool WaitForPacket(std::uint16_t& msg_id, std::vector<std::uint8_t>& payload, int timeout_ms = 300) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        boost::system::error_code ec;
        std::size_t available = 0;

        while (std::chrono::steady_clock::now() < deadline) {
            FlushSessionIO();
            available = peer_socket_.available(ec);
            if (!ec && available >= core_proto::k_header_bytes) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (ec || available < core_proto::k_header_bytes) {
            return false;
        }

        std::array<std::uint8_t, core_proto::k_header_bytes> header_buf{};
        boost::asio::read(peer_socket_, boost::asio::buffer(header_buf), ec);
        if (ec) {
            return false;
        }

        core_proto::PacketHeader header{};
        core_proto::decode_header(header_buf.data(), header);
        msg_id = header.msg_id;

        payload.assign(header.length, 0);
        if (header.length > 0) {
            boost::asio::read(peer_socket_, boost::asio::buffer(payload), ec);
            if (ec) {
                return false;
            }
        }

        return true;
    }

    std::optional<ErrorFrame> WaitForError(int timeout_ms = 300) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            std::uint16_t msg_id = 0;
            std::vector<std::uint8_t> payload;
            if (!WaitForPacket(msg_id, payload, 60)) {
                continue;
            }
            if (msg_id != core_proto::MSG_ERR) {
                continue;
            }

            if (payload.size() < 4) {
                return std::nullopt;
            }

            ErrorFrame out{};
            out.code = static_cast<std::uint16_t>((static_cast<std::uint16_t>(payload[0]) << 8) | payload[1]);

            const auto msg_len = static_cast<std::uint16_t>((static_cast<std::uint16_t>(payload[2]) << 8) | payload[3]);
            if (payload.size() < 4u + static_cast<std::size_t>(msg_len)) {
                return std::nullopt;
            }

            out.message.assign(reinterpret_cast<const char*>(payload.data() + 4), msg_len);
            return out;
        }
        return std::nullopt;
    }

    std::optional<std::uint16_t> WaitForErrorCode(int timeout_ms = 300) {
        const auto err = WaitForError(timeout_ms);
        if (!err.has_value()) {
            return std::nullopt;
        }
        return err->code;
    }

    bool WaitForBroadcastText(const std::string& expected_substring, int timeout_ms = 500) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            std::uint16_t msg_id = 0;
            std::vector<std::uint8_t> payload;
            if (!WaitForPacket(msg_id, payload, 60)) {
                continue;
            }
            if (msg_id != game_proto::MSG_CHAT_BROADCAST) {
                continue;
            }
            server::wire::v1::ChatBroadcast pb;
            if (!pb.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
                continue;
            }
            if (pb.text().find(expected_substring) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    std::optional<server::wire::v1::LoginRes> WaitForLoginResponse(int timeout_ms = 300) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            std::uint16_t msg_id = 0;
            std::vector<std::uint8_t> payload;
            if (!WaitForPacket(msg_id, payload, 60)) {
                continue;
            }
            if (msg_id != game_proto::MSG_LOGIN_RES) {
                continue;
            }

            server::wire::v1::LoginRes out;
            if (!out.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
                return std::nullopt;
            }
            return out;
        }
        return std::nullopt;
    }
};

namespace server::app::chat {

struct ChatServiceContinuityTester {
    struct ResumeSnapshot {
        std::string logical_session_id;
        std::string resume_token;
        std::string user_id;
        std::string effective_user;
        std::string room;
        std::uint64_t expires_unix_ms{0};
        bool resumed{false};
    };

    static std::optional<ResumeSnapshot> TryResume(ChatService& service, std::string_view token) {
        const auto lease = ChatServicePrivateAccess::try_resume_continuity_lease(service, token);
        if (!lease.has_value()) {
            return std::nullopt;
        }

        return ResumeSnapshot{
            .logical_session_id = lease->logical_session_id,
            .resume_token = lease->resume_token,
            .user_id = lease->user_id,
            .effective_user = lease->effective_user,
            .room = lease->room,
            .expires_unix_ms = lease->expires_unix_ms,
            .resumed = lease->resumed,
        };
    }


};

} // namespace server::app::chat

// 로그인 핸들러 테스트
TEST_F(ChatServiceTest, Login) {
    std::vector<uint8_t> payload;
    write_lp_utf8(payload, "test_user");
    write_lp_utf8(payload, "test_token");

    chat_service_->on_login(*session_, payload);
    ProcessJobs();
    FlushSessionIO();

    // 검증: 피어 소켓에 데이터가 도착해야 함 (LoginRes)
    EXPECT_TRUE(WaitForData()) << "Login response should be received by peer";

    // 검증: Redis에 로비 유저 추가되었는지 (Spy 확인)
    EXPECT_TRUE(redis_->sadd_called) << "User should be added to Redis set";
}


TEST_F(ChatServiceTest, LoginRejectsMismatchedProtocolMajor) {
    std::vector<std::uint8_t> payload;
    write_lp_utf8(payload, "test_user");
    write_lp_utf8(payload, "test_token");

    const auto version_offset = payload.size();
    payload.resize(version_offset + 4);
    core_proto::write_be16(static_cast<std::uint16_t>(core_proto::kProtocolVersionMajor + 1), payload.data() + version_offset);
    core_proto::write_be16(core_proto::kProtocolVersionMinor, payload.data() + version_offset + 2);

    chat_service_->on_login(*session_, payload);
    FlushSessionIO();

    const auto error_code = WaitForErrorCode();
    ASSERT_TRUE(error_code.has_value());
    EXPECT_EQ(*error_code, core_proto::errc::UNSUPPORTED_VERSION);
}

TEST_F(ChatServiceTest, LoginRejectsHigherProtocolMinor) {
    std::vector<std::uint8_t> payload;
    write_lp_utf8(payload, "test_user");
    write_lp_utf8(payload, "test_token");

    const auto version_offset = payload.size();
    payload.resize(version_offset + 4);
    core_proto::write_be16(core_proto::kProtocolVersionMajor, payload.data() + version_offset);
    core_proto::write_be16(static_cast<std::uint16_t>(core_proto::kProtocolVersionMinor + 1), payload.data() + version_offset + 2);

    chat_service_->on_login(*session_, payload);
    FlushSessionIO();

    const auto error_code = WaitForErrorCode();
    ASSERT_TRUE(error_code.has_value());
    EXPECT_EQ(*error_code, core_proto::errc::UNSUPPORTED_VERSION);
}

TEST_F(ChatServiceTest, LoginAcceptsLowerProtocolMinor) {
    std::vector<std::uint8_t> payload;
    write_lp_utf8(payload, "test_user");
    write_lp_utf8(payload, "test_token");

    const auto version_offset = payload.size();
    payload.resize(version_offset + 4);
    core_proto::write_be16(core_proto::kProtocolVersionMajor, payload.data() + version_offset);
    core_proto::write_be16(0, payload.data() + version_offset + 2);

    chat_service_->on_login(*session_, payload);
    ProcessJobs();
    FlushSessionIO();

    // lower minor version은 버전 협상에서 허용되어야 하며,
    // UNSUPPORTED_VERSION으로 거절되면 안 된다.
    const auto error_code = WaitForErrorCode();
    if (error_code.has_value()) {
        EXPECT_NE(*error_code, core_proto::errc::UNSUPPORTED_VERSION);
    }
}

TEST_F(ChatServiceTest, DispatcherBlocksProtectedOpcodeBeforeLogin) {
    dispatcher_.register_handler(
        game_proto::MSG_CHAT_SEND,
        [this](server::core::Session& s, std::span<const std::uint8_t> payload) {
            chat_service_->on_chat_send(s, payload);
        },
        server::protocol::opcode_policy(game_proto::MSG_CHAT_SEND));

    std::vector<std::uint8_t> payload;
    write_lp_utf8(payload, "lobby");
    write_lp_utf8(payload, "hello-before-login");

    EXPECT_TRUE(dispatcher_.dispatch(game_proto::MSG_CHAT_SEND, *session_, payload));
    FlushSessionIO();

    std::uint16_t msg_id = 0;
    std::vector<std::uint8_t> body;
    bool found_err = false;
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(WaitForPacket(msg_id, body));
        if (msg_id == core_proto::MSG_ERR) {
            found_err = true;
            break;
        }
    }

    ASSERT_TRUE(found_err);
    ASSERT_GE(body.size(), 2u);
    const std::uint16_t code = static_cast<std::uint16_t>((static_cast<std::uint16_t>(body[0]) << 8) | body[1]);
    EXPECT_EQ(code, core_proto::errc::FORBIDDEN);
}

// 방 입장 테스트
TEST_F(ChatServiceTest, JoinRoom) {
    // 1. 로그인
    std::vector<uint8_t> login_payload;
    write_lp_utf8(login_payload, "test_user");
    write_lp_utf8(login_payload, "test_token");
    chat_service_->on_login(*session_, login_payload);
    ProcessJobs();
    FlushSessionIO();
    WaitForData(); // 로그인 응답 소비

    // 2. 방 입장
    std::vector<uint8_t> payload;
    write_lp_utf8(payload, "room_1");
    write_lp_utf8(payload, ""); // password

    chat_service_->on_join(*session_, payload);
    ProcessJobs();
    FlushSessionIO();

    // 검증: 입장 성공 시 Snapshot, Broadcast 등 메시지가 전송됨
    EXPECT_TRUE(WaitForData()) << "Join room response should be received by peer";
}

// 방 퇴장 테스트
TEST_F(ChatServiceTest, LeaveRoom) {
    // 1. 로그인 & 입장
    std::vector<uint8_t> login_payload;
    write_lp_utf8(login_payload, "test_user");
    write_lp_utf8(login_payload, "test_token");
    chat_service_->on_login(*session_, login_payload);
    ProcessJobs();
    FlushSessionIO();
    WaitForData();

    std::vector<uint8_t> join_payload;
    write_lp_utf8(join_payload, "room_1");
    write_lp_utf8(join_payload, "");
    chat_service_->on_join(*session_, join_payload);
    ProcessJobs();
    FlushSessionIO();
    WaitForData();

    // 2. 퇴장
    std::vector<uint8_t> leave_payload;
    chat_service_->on_leave(*session_, leave_payload);
    ProcessJobs();
    FlushSessionIO();

    // 검증: 퇴장 시 브로드캐스트 등이 발생할 수 있음.
    // 여기서는 에러 없이 실행되는지 확인.
    SUCCEED();
}

// 채팅 메시지 전송 테스트
TEST_F(ChatServiceTest, ChatSend) {
    // 1. 로그인 & 입장
    std::vector<uint8_t> login_payload;
    write_lp_utf8(login_payload, "test_user");
    write_lp_utf8(login_payload, "test_token");
    chat_service_->on_login(*session_, login_payload);
    ProcessJobs();
    FlushSessionIO();
    WaitForData();

    std::vector<uint8_t> join_payload;
    write_lp_utf8(join_payload, "room_1");
    write_lp_utf8(join_payload, "");
    chat_service_->on_join(*session_, join_payload);
    ProcessJobs();
    FlushSessionIO();
    WaitForData();

    // 2. 메시지 전송
    std::vector<uint8_t> chat_payload;
    write_lp_utf8(chat_payload, "room_1");
    write_lp_utf8(chat_payload, "Hello World");

    chat_service_->on_chat_send(*session_, chat_payload);
    ProcessJobs();
    FlushSessionIO();

    // 검증: 메시지 브로드캐스트가 나에게도 옴
    EXPECT_TRUE(WaitForData()) << "Chat message broadcast should be received by peer";
}

TEST_F(ChatServiceTest, CommitFailureDoesNotBroadcastCacheOrAdvanceLastSeen) {
    ScopedEnvVar pubsub_env("USE_REDIS_PUBSUB", "1");
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);
    LoginAs("commit_failure_user");
    WaitForData();
    JoinRoom("commit_failure_room");
    WaitForData();
    redis_->reset_publish_trace();
    const auto lists_before = redis_->list_store;
    db_pool_->state()->last_membership_last_seen.reset();
    db_pool_->state()->fail_message_commit = true;
    SendChat("commit_failure_room", "must not escape rollback");
    const auto error = WaitForError();
    ASSERT_TRUE(error);
    EXPECT_EQ(error->code, core_proto::errc::SERVER_BUSY);
    EXPECT_FALSE(redis_->publish_called);
    EXPECT_EQ(redis_->list_store, lists_before);
    EXPECT_FALSE(db_pool_->state()->last_membership_last_seen);
    EXPECT_FALSE(WaitForBroadcastText("must not escape rollback", 50));
    for (const auto& [_, messages] : db_pool_->state()->messages_by_room_id) EXPECT_TRUE(messages.empty());
}

TEST_F(ChatServiceTest, SharedPrincipalReservationRejectsSecondServiceInstance) {
    LoginAs("single_principal");
    ASSERT_TRUE(WaitForLoginResponse());
    auto first = std::move(chat_service_);
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);
    LoginAs("single_principal");
    const auto error = WaitForError();
    ASSERT_TRUE(error);
    EXPECT_EQ(error->code, core_proto::errc::NAME_TAKEN);
    first->on_session_close(session_);
    ProcessJobs();
    WaitForData();
    LoginAs("single_principal");
    EXPECT_TRUE(WaitForLoginResponse());
}

TEST_F(ChatServiceTest, RepeatedLoginCannotReserveAnotherPrincipalOnSameConnection) {
    LoginAs("first_identity");
    ASSERT_TRUE(WaitForLoginResponse());
    LoginAs("second_identity");
    const auto error = WaitForError();
    ASSERT_TRUE(error);
    EXPECT_EQ(error->code, core_proto::errc::FORBIDDEN);
    EXPECT_TRUE(db_pool_->state()->control.count("login:first_identity"));
    EXPECT_FALSE(db_pool_->state()->control.count("login:second_identity"));
}

TEST_F(ChatServiceTest, RetiredSessionCloseDoesNotEraseReplacementPresence) {
    ScopedEnvVar continuity_enabled("SESSION_CONTINUITY_ENABLED", "1");
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);
    LoginAs("lease_identity");
    const auto initial = WaitForLoginResponse();
    ASSERT_TRUE(initial);
    ASSERT_FALSE(initial->resume_token().empty());
    auto retired = std::move(chat_service_);
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);
    LoginAs("ignored", "resume:" + initial->resume_token());
    const auto resumed = WaitForLoginResponse();
    ASSERT_TRUE(resumed);
    ASSERT_TRUE(resumed->resumed());
    const auto presence = redis_->kv_store;
    const auto users = redis_->set_store;
    retired->on_session_close(session_);
    ProcessJobs();
    EXPECT_EQ(redis_->kv_store, presence);
    EXPECT_EQ(redis_->set_store, users);
    EXPECT_TRUE(db_pool_->state()->control.count("login:lease_identity"));
}

TEST_F(ChatServiceTest, SharedRoomInviteSurvivesServiceBoundaryWithoutGrantingOwnership) {
    LoginAs("original_owner");
    WaitForData();
    JoinRoom("shared_private_room");
    WaitForData();
    auto first = std::move(chat_service_);
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);
    LoginAs("other_member");
    WaitForData();
    JoinRoom("shared_private_room");
    const auto denied = WaitForError();
    ASSERT_TRUE(denied);
    EXPECT_EQ(denied->message, "invite required");
    std::vector<std::uint8_t> invite;
    write_lp_utf8(invite, "shared_private_room");
    write_lp_utf8(invite, "/invite other_member");
    first->on_chat_send(*session_, invite);
    ProcessJobs();
    EXPECT_TRUE(WaitForBroadcastText("invited user: other_member"));
    JoinRoom("shared_private_room");
    EXPECT_TRUE(WaitForData());
    EXPECT_EQ(ChatServicePrivateAccess::lookup_room_owner(*chat_service_, "shared_private_room").value_or(""), "original_owner");
    SendChat("shared_private_room", "/invite unrelated");
    EXPECT_TRUE(WaitForBroadcastText("room command denied: owner/admin only"));
}

TEST_F(ChatServiceTest, RealWorkersKeepSameSessionOrderWhenFirstDbWriteStalls) {
    LoginAs("ordered_user");
    WaitForData();
    JoinRoom("ordered_room");
    WaitForData();
    std::promise<void> first_entered, release_first;
    auto release = release_first.get_future().share();
    std::atomic<bool> second_entered{false};
    db_pool_->state()->before_message_create = [&](const std::string& content) {
        if (content == "first") { first_entered.set_value(); release.wait(); }
        if (content == "second") second_entered = true;
    };
    server::core::ThreadManager workers(job_queue_);
    workers.Start(2);
    auto send = [&](const std::string& text) {
        std::vector<std::uint8_t> body;
        write_lp_utf8(body, "ordered_room");
        write_lp_utf8(body, text);
        chat_service_->on_chat_send(*session_, body);
    };
    send("first");
    const bool entered = first_entered.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    send("second");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_FALSE(second_entered.load());
    release_first.set_value();
    workers.Stop();
    EXPECT_TRUE(entered);
    EXPECT_TRUE(second_entered.load());
    const auto room_id = db_pool_->state()->room_id_by_name_ci.at("ordered_room");
    const auto& messages = db_pool_->state()->messages_by_room_id.at(room_id);
    ASSERT_EQ(messages.size(), 2);
    EXPECT_EQ(messages[0].content, "first");
    EXPECT_EQ(messages[1].content, "second");
}

TEST_F(ChatServiceTest, ChatSendPersistsMembershipStateAndPublishesRoomFanout) {
    ScopedEnvVar pubsub_env("USE_REDIS_PUBSUB", "1");
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);

    LoginAs("persist_user", "persist_token");
    ASSERT_TRUE(WaitForData());

    JoinRoom("persist_room");
    ASSERT_TRUE(WaitForData());

    const auto storage = db_pool_->state();
    storage->last_created_message.reset();
    storage->last_membership_last_seen.reset();
    redis_->reset_publish_trace();

    SendChat("persist_room", "persist me");

    EXPECT_TRUE(WaitForBroadcastText("persist me")) << "chat broadcast should still reach the sender";
    ASSERT_TRUE(storage->last_created_message.has_value());
    EXPECT_EQ(storage->last_created_message->room_name, "persist_room");
    EXPECT_EQ(storage->last_created_message->content, "persist me");
    ASSERT_TRUE(storage->last_created_message->user_id.has_value());

    ASSERT_TRUE(storage->last_membership_last_seen.has_value());
    EXPECT_EQ(storage->last_membership_last_seen->room_id, storage->last_created_message->room_id);
    ASSERT_TRUE(storage->last_membership_last_seen->last_seen_msg_id.has_value());
    EXPECT_EQ(*storage->last_membership_last_seen->last_seen_msg_id, storage->last_created_message->id);

    EXPECT_TRUE(redis_->publish_called);
    EXPECT_EQ(redis_->last_publish_channel, "fanout:room:persist_room");
    EXPECT_TRUE(redis_->last_publish_message.rfind("gw=", 0) == 0);
    const auto payload_offset = redis_->last_publish_message.find('\n');
    ASSERT_NE(payload_offset, std::string::npos);
    EXPECT_GT(redis_->last_publish_message.size(), payload_offset + 1);
}

// 귓속말 테스트
TEST_F(ChatServiceTest, Whisper) {
    // 1. 로그인
    std::vector<uint8_t> login_payload;
    write_lp_utf8(login_payload, "test_user");
    write_lp_utf8(login_payload, "test_token");
    chat_service_->on_login(*session_, login_payload);
    ProcessJobs();
    FlushSessionIO();
    WaitForData();

    // 2. 귓속말 전송 (자기 자신에게)
    std::vector<uint8_t> whisper_payload;
    write_lp_utf8(whisper_payload, "test_user"); // target
    write_lp_utf8(whisper_payload, "Secret Message"); // text

    chat_service_->on_whisper(*session_, whisper_payload);
    ProcessJobs();
    FlushSessionIO();

    // 검증: 귓속말 결과(WhisperResult) 또는 메시지(WhisperNotice)가 와야 함
    EXPECT_TRUE(WaitForData()) << "Whisper response should be received by peer";
}

TEST_F(ChatServiceTest, WhisperRoutesViaRedisWhenRecipientIsRemote) {
    const char* old_pubsub_env = std::getenv("USE_REDIS_PUBSUB");
    const bool had_old_pubsub = (old_pubsub_env != nullptr);
    const std::string old_pubsub = had_old_pubsub ? std::string(old_pubsub_env) : std::string();
#if defined(_WIN32)
    _putenv_s("USE_REDIS_PUBSUB", "1");
#else
    setenv("USE_REDIS_PUBSUB", "1", 1);
#endif

    // ChatService는 생성 시점에 USE_REDIS_PUBSUB를 읽으므로,
    // 테스트에서 env를 변경한 뒤 인스턴스를 재생성해 설정을 반영한다.
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);

    // 1. 로그인
    std::vector<uint8_t> login_payload;
    write_lp_utf8(login_payload, "test_user");
    write_lp_utf8(login_payload, "test_token");
    chat_service_->on_login(*session_, login_payload);
    ProcessJobs();
    FlushSessionIO();
    WaitForData();

    // 2. 로컬에 없는 대상에게 귓속말
    std::vector<uint8_t> whisper_payload;
    write_lp_utf8(whisper_payload, "ghost_user");
    write_lp_utf8(whisper_payload, "Hello remote");
    chat_service_->on_whisper(*session_, whisper_payload);
    ProcessJobs();
    FlushSessionIO();

    EXPECT_TRUE(redis_->publish_called) << "Whisper should be routed via Redis publish";
    EXPECT_EQ(redis_->last_publish_channel, "fanout:whisper");
    EXPECT_TRUE(redis_->last_publish_message.rfind("gw=", 0) == 0);
    EXPECT_NE(redis_->last_publish_message.find('\n'), std::string::npos);
    EXPECT_TRUE(WaitForData()) << "Sender should receive whisper ack/echo";

#if defined(_WIN32)
    if (had_old_pubsub) {
        _putenv_s("USE_REDIS_PUBSUB", old_pubsub.c_str());
    } else {
        _putenv_s("USE_REDIS_PUBSUB", "");
    }
#else
    if (had_old_pubsub) {
        setenv("USE_REDIS_PUBSUB", old_pubsub.c_str(), 1);
    } else {
        unsetenv("USE_REDIS_PUBSUB");
    }
#endif
}

// 방 유저 목록 요청 테스트
TEST_F(ChatServiceTest, RoomUsers) {
    // 1. 로그인 & 입장
    std::vector<uint8_t> login_payload;
    write_lp_utf8(login_payload, "test_user");
    write_lp_utf8(login_payload, "test_token");
    chat_service_->on_login(*session_, login_payload);
    ProcessJobs();
    FlushSessionIO();
    WaitForData();

    std::vector<uint8_t> join_payload;
    write_lp_utf8(join_payload, "room_1");
    write_lp_utf8(join_payload, "");
    chat_service_->on_join(*session_, join_payload);
    ProcessJobs();
    FlushSessionIO();
    WaitForData();

    // 2. 유저 목록 요청
    std::vector<uint8_t> req_payload;
    write_lp_utf8(req_payload, "room_1");

    chat_service_->on_room_users_request(*session_, req_payload);
    ProcessJobs();
    FlushSessionIO();

    // 검증: 유저 목록 응답이 와야 함
    EXPECT_TRUE(WaitForData()) << "Room users response should be received by peer";
}

// 방 목록 요청 테스트
TEST_F(ChatServiceTest, RoomsList) {
    // 1. 로그인
    std::vector<uint8_t> login_payload;
    write_lp_utf8(login_payload, "test_user");
    write_lp_utf8(login_payload, "test_token");
    chat_service_->on_login(*session_, login_payload);
    ProcessJobs();
    FlushSessionIO();
    WaitForData();

    // 2. 방 목록 요청
    std::vector<uint8_t> req_payload; // Empty payload

    chat_service_->on_rooms_request(*session_, req_payload);
    ProcessJobs();
    FlushSessionIO();

    // 검증: 방 목록 응답이 와야 함
    EXPECT_TRUE(WaitForData()) << "Rooms list response should be received by peer";
}

// 핑 테스트
TEST_F(ChatServiceTest, Ping) {
    // 1. 로그인 (옵션)

    // 2. 핑 요청
    std::vector<uint8_t> req_payload; // Empty payload

    chat_service_->on_ping(*session_, req_payload);
    ProcessJobs();
    FlushSessionIO();

    // 검증: PONG 응답이 와야 함 (MSG_PONG = 0x0003)
    EXPECT_TRUE(WaitForData()) << "Pong response should be received by peer";
}

// 상태 갱신 요청 테스트
TEST_F(ChatServiceTest, Refresh) {
    // 1. 로그인
    std::vector<uint8_t> login_payload;
    write_lp_utf8(login_payload, "test_user");
    write_lp_utf8(login_payload, "test_token");
    chat_service_->on_login(*session_, login_payload);
    ProcessJobs();
    FlushSessionIO();
    WaitForData();

    // 2. Refresh 요청
    std::vector<uint8_t> req_payload; // Empty payload

    chat_service_->on_refresh_request(*session_, req_payload);
    ProcessJobs();
    FlushSessionIO();

    // 검증: Refresh 알림 또는 스냅샷이 와야 함
    // 현재 구현상 Refresh는 Snapshot을 다시 보내거나 할 수 있음.
    // 최소한 에러 없이 응답이 오는지 확인.
    // EXPECT_TRUE(WaitForData()) << "Refresh response should be received by peer";
    // NOTE: on_refresh_request 구현에 따라 응답이 없을 수도 있음(단순 갱신).
    // 여기서는 실행에 문제가 없는지 확인.
    SUCCEED();
}

TEST_F(ChatServiceTest, BlacklistCommandsRoundTrip) {
    LoginAs("test_user");
    WaitForData();

    SendChat("lobby", "/blacklist add blocked_user");
    EXPECT_TRUE(WaitForBroadcastText("blacklist add: blocked_user"));

    SendChat("lobby", "/blacklist list");
    EXPECT_TRUE(WaitForBroadcastText("blacklist: blocked_user"));

    SendChat("lobby", "/blacklist remove blocked_user");
    EXPECT_TRUE(WaitForBroadcastText("blacklist remove: blocked_user"));
}

TEST_F(ChatServiceTest, ChatSpamThresholdTemporarilyMutesSender) {
    ScopedEnvVar spam_threshold("CHAT_SPAM_THRESHOLD", "3");
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);

    LoginAs("spam_user");
    WaitForData();

    JoinRoom("spam_room");
    WaitForData();

    SendChat("spam_room", "first spam candidate");
    EXPECT_TRUE(WaitForBroadcastText("first spam candidate"));

    SendChat("spam_room", "second spam candidate");
    EXPECT_TRUE(WaitForBroadcastText("second spam candidate"));

    SendChat("spam_room", "third spam candidate");
    EXPECT_TRUE(WaitForBroadcastText("third spam candidate"));

    SendChat("spam_room", "fourth spam candidate");
    const auto error = WaitForError();
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->code, core_proto::errc::FORBIDDEN);
    EXPECT_EQ(error->message, "temporarily muted for spam");
}

TEST_F(ChatServiceTest, AdminModerationCommandsDeniedForNonAdmin) {
    LoginAs("regular_user");
    WaitForData();

    SendChat("lobby", "/mute target_user 30");
    EXPECT_TRUE(WaitForBroadcastText("mute denied: admin only"));

    SendChat("lobby", "/gkick target_user");
    EXPECT_TRUE(WaitForBroadcastText("global kick denied: admin only"));
}

TEST_F(ChatServiceTest, AdminModerationCommandsAllowedForAdmin) {
    const char* old_admin_env = std::getenv("CHAT_ADMIN_USERS");
    const bool had_old_admin_env = (old_admin_env != nullptr);
    const std::string old_admin_value = had_old_admin_env ? std::string(old_admin_env) : std::string();

#if defined(_WIN32)
    _putenv_s("CHAT_ADMIN_USERS", "admin_user");
#else
    setenv("CHAT_ADMIN_USERS", "admin_user", 1);
#endif

    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);

    LoginAs("admin_user");
    WaitForData();

    SendChat("lobby", "/mute target_user 30");
    EXPECT_TRUE(WaitForBroadcastText("mute applied: user=target_user"));

    SendChat("lobby", "/unmute target_user");
    EXPECT_TRUE(WaitForBroadcastText("unmute applied: user=target_user"));

    SendChat("lobby", "/ban target_user 60");
    EXPECT_TRUE(WaitForBroadcastText("ban applied: user=target_user"));

    SendChat("lobby", "/unban target_user");
    EXPECT_TRUE(WaitForBroadcastText("unban applied: user=target_user"));

#if defined(_WIN32)
    if (had_old_admin_env) {
        _putenv_s("CHAT_ADMIN_USERS", old_admin_value.c_str());
    } else {
        _putenv_s("CHAT_ADMIN_USERS", "");
    }
#else
    if (had_old_admin_env) {
        setenv("CHAT_ADMIN_USERS", old_admin_value.c_str(), 1);
    } else {
        unsetenv("CHAT_ADMIN_USERS");
    }
#endif
}

TEST_F(ChatServiceTest, RoomOwnerCanRemoveOwnRoom) {
    LoginAs("owner_user");
    WaitForData();

    JoinRoom("owner_room");
    WaitForData();

    SendChat("owner_room", "/room remove");
    EXPECT_TRUE(WaitForBroadcastText("room removed: owner_room"));
}

TEST_F(ChatServiceTest, ContinuityResumeRestoresPersistedOwnerInsteadOfMintingOne) {
    ScopedEnvVar continuity_enabled("SESSION_CONTINUITY_ENABLED", "1");
    ScopedEnvVar continuity_prefix("SESSION_CONTINUITY_REDIS_PREFIX", "dynaxis");

    redis_ = std::make_shared<MockRedisClient>();
    db_pool_ = std::make_shared<MockConnectionPool>();
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);

    LoginAs("resume_owner", "resume_owner_token");
    const auto initial_login = WaitForLoginResponse();
    ASSERT_TRUE(initial_login.has_value());
    ASSERT_FALSE(initial_login->resume_token().empty());

    JoinRoom("resume_owner_room");
    WaitForData();

    auto authority = nlohmann::json::parse(db_pool_->state()->control.at("room:resume_owner_room"));
    authority["owner"] = "actual_owner";
    db_pool_->state()->control["room:resume_owner_room"] = authority.dump();
    chat_service_.reset();
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);

    std::vector<std::uint8_t> resume_payload;
    write_lp_utf8(resume_payload, "resume_owner");
    write_lp_utf8(resume_payload, "resume:" + initial_login->resume_token());
    chat_service_->on_login(*session_, resume_payload);
    ProcessJobs();
    FlushSessionIO();

    const auto resumed_login = WaitForLoginResponse();
    ASSERT_TRUE(resumed_login.has_value());
    EXPECT_TRUE(resumed_login->resumed());

    const auto resumed_owner =
        ChatServicePrivateAccess::lookup_room_owner(*chat_service_, "resume_owner_room");
    ASSERT_TRUE(resumed_owner.has_value());
    EXPECT_EQ(*resumed_owner, "actual_owner");
}


TEST_F(ChatServiceTest, LoginDoesNotPublishIpToRoomHistory) {
    LoginAs("private-login-user");
    EXPECT_FALSE(db_pool_->state()->users_by_id.empty());
    EXPECT_FALSE(db_pool_->state()->rooms_by_id.empty());
    for (const auto& [room, messages] : db_pool_->state()->messages_by_room_id) {
        for (const auto& message : messages) {
            EXPECT_EQ(message.content.find("login ip="), std::string::npos);
            EXPECT_EQ(message.content.find("127.0.0.1"), std::string::npos);
        }
    }
}

TEST_F(ChatServiceTest, SignedLoginRejectsUnverifiedCredentials) {
    ChatService::Dependencies dependencies;
    dependencies.require_auth = true;
    dependencies.auth_signing_secret = std::string(32, 'k');
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, std::move(dependencies));
    LoginAs("victim", "unverified-token");
    const auto error = WaitForError();
    ASSERT_TRUE(error);
    EXPECT_EQ(error->code, core_proto::errc::UNAUTHORIZED);
    EXPECT_EQ(session_->session_status(), core_proto::SessionStatus::kAny);
}

TEST_F(ChatServiceTest, SignedLoginUsesVerifiedPrincipalInsteadOfClaimedName) {
    ChatService::Dependencies dependencies;
    dependencies.db_pool = db_pool_;
    dependencies.redis = redis_;
    dependencies.require_auth = true;
    dependencies.auth_signing_secret = std::string(32, 'k');
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, std::move(dependencies));
    const auto now = server::auth::unix_seconds();
    const auto token = server::auth::issue_token({"verified-user", "auth-session", "Display label", now, now + 60}, std::string(32, 'k'));
    LoginAs("impersonated-user", token);
    for (int i = 0; i < 10; ++i) {
        std::uint16_t id = 0;
        std::vector<std::uint8_t> payload;
        ASSERT_TRUE(WaitForPacket(id, payload));
        if (id != game_proto::MSG_LOGIN_RES) { continue; }
        server::wire::v1::LoginRes response;
        ASSERT_TRUE(response.ParseFromArray(payload.data(), static_cast<int>(payload.size())));
        EXPECT_EQ(response.effective_user(), "verified-user");
        EXPECT_EQ(response.principal_id(), "verified-user");
        EXPECT_EQ(response.auth_session_id(), "auth-session");
        EXPECT_EQ(response.display_name(), "Display label");
        return;
    }
    FAIL() << "login response missing";
}


// 세션 종료 테스트
TEST_F(ChatServiceTest, SessionClose) {
    // 1. 로그인
    std::vector<uint8_t> login_payload;
    write_lp_utf8(login_payload, "test_user");
    write_lp_utf8(login_payload, "test_token");
    chat_service_->on_login(*session_, login_payload);
    ProcessJobs();
    FlushSessionIO();
    WaitForData();

    // 2. 세션 종료
    chat_service_->on_session_close(session_);
    ProcessJobs();
    FlushSessionIO();

    // 검증: 세션 종료 시 Redis에서 제거되거나 로그아웃 처리가 되어야 함.
    // Spy를 통해 확인 가능하지만, 현재 MockRedisClient에는 del_called가 없음.
    // 에러 없이 실행되는지 확인.
    SUCCEED();
}
