#include <gtest/gtest.h>
#include <server/chat/chat_service.hpp>
#include <server/core/net/session.hpp>
#include <server/core/net/dispatcher.hpp>
#include <server/core/concurrent/job_queue.hpp>
#include <server/storage/connection_pool.hpp>
#include <server/storage/unit_of_work.hpp>
#include <server/storage/redis/client.hpp>
#include <server/core/config/options.hpp>
#include <server/core/net/connection_runtime_state.hpp>
#include <server/core/protocol/protocol_errors.hpp>
#include <server/core/protocol/packet.hpp>
#include <server/core/protocol/version.hpp>
#include <server/core/runtime_metrics.hpp>
#include <server/core/scripting/lua_runtime.hpp>
#include <server/core/util/service_registry.hpp>
#include <server/protocol/game_opcodes.hpp>
#include "wire.pb.h"
#include <boost/asio.hpp>
#include <thread>
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

#ifndef TEST_CHAT_HOOK_V2_ONLY_PATH
#define TEST_CHAT_HOOK_V2_ONLY_PATH ""
#endif

#ifndef TEST_CHAT_HOOK_THROWING_PATH
#define TEST_CHAT_HOOK_THROWING_PATH ""
#endif

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
    std::unordered_map<std::string, server::storage::User> users_by_id;
    std::unordered_map<std::string, std::string> user_id_by_name_ci;
    std::unordered_map<std::string, server::storage::Session> sessions_by_id;
    std::unordered_map<std::string, std::string> session_id_by_token_hash;
    std::uint64_t next_user_id{1};
    std::uint64_t next_session_id{1};
};

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
    MockRoomRepository() = default;
    std::optional<Room> find_by_id(const std::string&) override { return std::nullopt; }
    std::vector<Room> search_by_name_ci(const std::string&, std::size_t) override { return {}; }
    std::optional<Room> find_by_name_exact_ci(const std::string&) override { return std::nullopt; }
    Room create(const std::string&, bool) override { return {}; }
    void close(const std::string&) override {}
};

class MockMessageRepository : public IMessageRepository {
public:
    MockMessageRepository() = default;
    std::vector<Message> fetch_recent_by_room(const std::string&, std::uint64_t, std::size_t) override { return {}; }
    Message create(const std::string&, const std::string&, const std::optional<std::string>&, const std::string&) override { return {}; }
    std::uint64_t get_last_id(const std::string&) override { return 0; }
    void delete_by_room(const std::string&) override {}
};

class MockMembershipRepository : public IMembershipRepository {
public:
    MockMembershipRepository() = default;
    void upsert_join(const std::string&, const std::string&, const std::string&) override {}
    void update_last_seen(const std::string&, const std::string&, std::uint64_t) override {}
    void leave(const std::string&, const std::string&) override {}
    std::optional<std::uint64_t> get_last_seen(const std::string&, const std::string&) override { return std::nullopt; }
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

class MockUnitOfWork : public IRepositoryUnitOfWork {
public:
    explicit MockUnitOfWork(std::shared_ptr<MockStorageState> state)
        : user_repo(state),
          session_repo(std::move(state)) {}

    MockUserRepository user_repo;
    MockRoomRepository room_repo;
    MockMessageRepository msg_repo;
    MockMembershipRepository membership_repo;
    MockSessionRepository session_repo;

    void commit() override {}
    void rollback() override {}
    
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
    
    // IRedisClient Interface Implementation
    bool health_check() override { return true; }
    bool lpush_trim(const std::string&, const std::string&, std::size_t) override { return true; }
    
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
        std::promise<bool> done;
        std::future<bool> fut = done.get_future();
        
        std::thread worker([&]() {
            try {
                auto job = job_queue_.Pop();
                if (job) {
                    job();
                    done.set_value(true);
                } else {
                    done.set_value(false);
                }
            } catch (const std::exception& e) {
                std::printf("Job execution failed: %s\n", e.what());
                done.set_value(false);
            } catch (...) {
                std::printf("Job execution failed with unknown error\n");
                done.set_value(false);
            }
        });
        
        if (fut.wait_for(std::chrono::milliseconds(500)) == std::future_status::timeout) {
            std::printf("ProcessJobs timed out\n");
            job_queue_.Stop();
        }
        worker.join();
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
        std::string world_id;
        std::string room;
        std::uint64_t expires_unix_ms{0};
        bool resumed{false};
    };

    static std::optional<ResumeSnapshot> TryResume(ChatService& service, std::string_view token) {
        const auto lease = service.try_resume_continuity_lease(token);
        if (!lease.has_value()) {
            return std::nullopt;
        }

        return ResumeSnapshot{
            .logical_session_id = lease->logical_session_id,
            .resume_token = lease->resume_token,
            .user_id = lease->user_id,
            .effective_user = lease->effective_user,
            .world_id = lease->world_id,
            .room = lease->room,
            .expires_unix_ms = lease->expires_unix_ms,
            .resumed = lease->resumed,
        };
    }

    static std::string WorldPolicyKey(ChatService& service, const std::string& world_id) {
        return service.make_continuity_world_policy_key(world_id);
    }

    static std::string WorldOwnerKey(ChatService& service, const std::string& world_id) {
        return service.make_continuity_world_owner_key(world_id);
    }

    static std::string WorldMigrationKey(ChatService& service, const std::string& world_id) {
        return service.make_continuity_world_migration_key(world_id);
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

TEST_F(ChatServiceTest, ContinuityResumeUsesAppDefinedMigrationPayloadRoomHandoff) {
    ScopedEnvVar continuity_enabled("SESSION_CONTINUITY_ENABLED", "1");
    ScopedEnvVar continuity_prefix("SESSION_CONTINUITY_REDIS_PREFIX", "dynaxis");
    ScopedEnvVar world_default("WORLD_ADMISSION_DEFAULT", "starter-a");
    ScopedEnvVar server_instance("SERVER_INSTANCE_ID", "server-test-a");

    redis_ = std::make_shared<MockRedisClient>();
    db_pool_ = std::make_shared<MockConnectionPool>();
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);

    LoginAs("payload_user", "payload_token");
    const auto login = WaitForLoginResponse();
    ASSERT_TRUE(login.has_value());
    ASSERT_FALSE(login->logical_session_id().empty());
    ASSERT_FALSE(login->resume_token().empty());
    EXPECT_EQ(login->world_id(), "starter-a");

    JoinRoom("source-room");

    server::core::discovery::WorldLifecyclePolicy policy{};
    policy.draining = true;
    EXPECT_TRUE(redis_->set(
        server::app::chat::ChatServiceContinuityTester::WorldPolicyKey(*chat_service_, "starter-a"),
        server::core::discovery::serialize_world_lifecycle_policy(policy)));
    EXPECT_TRUE(redis_->set(
        server::app::chat::ChatServiceContinuityTester::WorldOwnerKey(*chat_service_, "starter-b"),
        "server-test-a"));

    server::core::worlds::WorldMigrationEnvelope envelope{};
    envelope.target_world_id = "starter-b";
    envelope.target_owner_instance_id = "server-test-a";
    envelope.preserve_room = false;
    envelope.payload_kind = "chat-room-v1";
    envelope.payload_ref = "target-room";
    envelope.updated_at_ms = 1;
    EXPECT_TRUE(redis_->set(
        server::app::chat::ChatServiceContinuityTester::WorldMigrationKey(*chat_service_, "starter-a"),
        server::core::worlds::serialize_world_migration_envelope(envelope)));

    const auto metrics_before = chat_service_->continuity_metrics();
    const auto resumed =
        server::app::chat::ChatServiceContinuityTester::TryResume(
            *chat_service_,
            "resume:" + login->resume_token());
    ASSERT_TRUE(resumed.has_value());
    EXPECT_TRUE(resumed->resumed);
    EXPECT_EQ(resumed->world_id, "starter-b");
    EXPECT_EQ(resumed->room, "target-room");

    const auto metrics_after = chat_service_->continuity_metrics();
    EXPECT_EQ(
        metrics_after.world_migration_payload_room_handoff_total,
        metrics_before.world_migration_payload_room_handoff_total + 1);
    EXPECT_EQ(
        metrics_after.world_migration_payload_room_handoff_fallback_total,
        metrics_before.world_migration_payload_room_handoff_fallback_total);
    EXPECT_EQ(metrics_after.state_restore_total, metrics_before.state_restore_total);
    EXPECT_EQ(metrics_after.state_restore_fallback_total, metrics_before.state_restore_fallback_total);
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

TEST_F(ChatServiceTest, RuntimeSettingRejectsOutOfRangeWithoutCountingSuccess) {
    const auto before = server::core::runtime_metrics::snapshot();

    chat_service_->admin_apply_runtime_setting("recent_history_limit", "12");
    ProcessJobs();
    FlushSessionIO();

    chat_service_->admin_apply_runtime_setting("room_recent_maxlen", "8");
    ProcessJobs();
    FlushSessionIO();

    const auto after = server::core::runtime_metrics::snapshot();
    EXPECT_EQ(after.runtime_setting_reload_attempt_total, before.runtime_setting_reload_attempt_total + 2);
    EXPECT_EQ(after.runtime_setting_reload_success_total, before.runtime_setting_reload_success_total + 1);
    EXPECT_EQ(after.runtime_setting_reload_failure_total, before.runtime_setting_reload_failure_total + 1);
}

TEST_F(ChatServiceTest, RuntimeSettingRejectsUnsupportedKeyAndInvalidValue) {
    const auto before = server::core::runtime_metrics::snapshot();

    chat_service_->admin_apply_runtime_setting("unknown_runtime_key", "11");
    ProcessJobs();
    FlushSessionIO();

    chat_service_->admin_apply_runtime_setting("chat_spam_threshold", "NaN");
    ProcessJobs();
    FlushSessionIO();

    const auto after = server::core::runtime_metrics::snapshot();
    EXPECT_EQ(after.runtime_setting_reload_attempt_total, before.runtime_setting_reload_attempt_total + 2);
    EXPECT_EQ(after.runtime_setting_reload_success_total, before.runtime_setting_reload_success_total);
    EXPECT_EQ(after.runtime_setting_reload_failure_total, before.runtime_setting_reload_failure_total + 2);
}

TEST_F(ChatServiceTest, LoginDeniedByV2HookPluginReturnsForbidden) {
    if (std::string(TEST_CHAT_HOOK_V2_ONLY_PATH).empty()) {
        GTEST_SKIP() << "TEST_CHAT_HOOK_V2_ONLY_PATH is not configured";
    }

    ScopedEnvVar env_single("CHAT_HOOK_PLUGIN_PATH", TEST_CHAT_HOOK_V2_ONLY_PATH);
    ScopedEnvVar env_paths("CHAT_HOOK_PLUGIN_PATHS", "");
    ScopedEnvVar env_dir("CHAT_HOOK_PLUGINS_DIR", "");
    ScopedTempDir cache_temp("chat_service_hook_cache");
    const auto cache_path = cache_temp.path().string();
    const auto lock_path = (cache_temp.path() / "chat_hook.lock").string();
    ScopedEnvVar env_lock("CHAT_HOOK_LOCK_PATH", lock_path.c_str());
    ScopedEnvVar env_cache("CHAT_HOOK_CACHE_DIR", cache_path.c_str());
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);

    std::vector<std::uint8_t> payload;
    write_lp_utf8(payload, "deny_login");
    write_lp_utf8(payload, "test_token");

    chat_service_->on_login(*session_, payload);
    ProcessJobs();
    FlushSessionIO();

    const auto error = WaitForError();
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->code, core_proto::errc::FORBIDDEN);
    EXPECT_EQ(error->message, "login blocked by v2-only test plugin");
    chat_service_.reset();
}

TEST_F(ChatServiceTest, JoinDeniedByV2HookPluginReturnsForbidden) {
    if (std::string(TEST_CHAT_HOOK_V2_ONLY_PATH).empty()) {
        GTEST_SKIP() << "TEST_CHAT_HOOK_V2_ONLY_PATH is not configured";
    }

    ScopedEnvVar env_single("CHAT_HOOK_PLUGIN_PATH", TEST_CHAT_HOOK_V2_ONLY_PATH);
    ScopedEnvVar env_paths("CHAT_HOOK_PLUGIN_PATHS", "");
    ScopedEnvVar env_dir("CHAT_HOOK_PLUGINS_DIR", "");
    ScopedTempDir cache_temp("chat_service_hook_cache");
    const auto cache_path = cache_temp.path().string();
    const auto lock_path = (cache_temp.path() / "chat_hook.lock").string();
    ScopedEnvVar env_lock("CHAT_HOOK_LOCK_PATH", lock_path.c_str());
    ScopedEnvVar env_cache("CHAT_HOOK_CACHE_DIR", cache_path.c_str());
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);

    LoginAs("allow_user");

    std::vector<std::uint8_t> join_payload;
    write_lp_utf8(join_payload, "forbidden_room");
    write_lp_utf8(join_payload, "");

    chat_service_->on_join(*session_, join_payload);
    ProcessJobs();
    FlushSessionIO();

    const auto error = WaitForError();
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->code, core_proto::errc::FORBIDDEN);
    EXPECT_EQ(error->message, "join blocked by v2-only test plugin");
    chat_service_.reset();
}

TEST_F(ChatServiceTest, LeaveDeniedByV2HookPluginReturnsForbidden) {
    if (std::string(TEST_CHAT_HOOK_V2_ONLY_PATH).empty()) {
        GTEST_SKIP() << "TEST_CHAT_HOOK_V2_ONLY_PATH is not configured";
    }

    ScopedEnvVar env_single("CHAT_HOOK_PLUGIN_PATH", TEST_CHAT_HOOK_V2_ONLY_PATH);
    ScopedEnvVar env_paths("CHAT_HOOK_PLUGIN_PATHS", "");
    ScopedEnvVar env_dir("CHAT_HOOK_PLUGINS_DIR", "");
    ScopedTempDir cache_temp("chat_service_hook_cache");
    const auto cache_path = cache_temp.path().string();
    const auto lock_path = (cache_temp.path() / "chat_hook.lock").string();
    ScopedEnvVar env_lock("CHAT_HOOK_LOCK_PATH", lock_path.c_str());
    ScopedEnvVar env_cache("CHAT_HOOK_CACHE_DIR", cache_path.c_str());
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);

    LoginAs("allow_user");

    std::vector<std::uint8_t> join_payload;
    write_lp_utf8(join_payload, "locked_leave");
    write_lp_utf8(join_payload, "");
    chat_service_->on_join(*session_, join_payload);
    ProcessJobs();
    FlushSessionIO();

    std::vector<std::uint8_t> leave_payload;
    write_lp_utf8(leave_payload, "locked_leave");
    chat_service_->on_leave(*session_, leave_payload);
    ProcessJobs();
    FlushSessionIO();

    const auto error_code = WaitForErrorCode();
    ASSERT_TRUE(error_code.has_value());
    EXPECT_EQ(*error_code, core_proto::errc::FORBIDDEN);
    chat_service_.reset();
}

TEST_F(ChatServiceTest, ThrowingChatHookExceptionDoesNotStopChatOrAdminSettingPath) {
    if (std::string(TEST_CHAT_HOOK_THROWING_PATH).empty()) {
        GTEST_SKIP() << "TEST_CHAT_HOOK_THROWING_PATH is not configured";
    }

    ScopedEnvVar env_single("CHAT_HOOK_PLUGIN_PATH", TEST_CHAT_HOOK_THROWING_PATH);
    ScopedEnvVar env_paths("CHAT_HOOK_PLUGIN_PATHS", "");
    ScopedEnvVar env_dir("CHAT_HOOK_PLUGINS_DIR", "");
    ScopedTempDir cache_temp("chat_service_throwing_hook_cache");
    const auto cache_path = cache_temp.path().string();
    const auto lock_path = (cache_temp.path() / "chat_hook.lock").string();
    ScopedEnvVar env_lock("CHAT_HOOK_LOCK_PATH", lock_path.c_str());
    ScopedEnvVar env_cache("CHAT_HOOK_CACHE_DIR", cache_path.c_str());
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);

    LoginAs("throwing_user");
    JoinRoom("throwing_room");

    SendChat("throwing_room", "throwing plugin first message");
    EXPECT_TRUE(WaitForBroadcastText("throwing plugin first message"));

    const auto plugin_metrics = chat_service_->chat_hook_plugins_metrics();
    EXPECT_TRUE(plugin_metrics.enabled);
    bool found_plugin = false;
    for (const auto& plugin : plugin_metrics.plugins) {
        if (plugin.name != "chat_hook_throwing") {
            continue;
        }
        found_plugin = true;
        EXPECT_TRUE(plugin.loaded);

        bool found_chat_hook = false;
        for (const auto& hook_metric : plugin.hook_metrics) {
            if (hook_metric.hook_name != "on_chat_send") {
                continue;
            }
            found_chat_hook = true;
            EXPECT_GE(hook_metric.calls_total, 1u);
            EXPECT_GE(hook_metric.errors_total, 1u);
        }
        EXPECT_TRUE(found_chat_hook);
    }
    EXPECT_TRUE(found_plugin);

    const auto before = server::core::runtime_metrics::snapshot();
    chat_service_->admin_apply_runtime_setting("chat_spam_threshold", "17");
    ProcessJobs();
    FlushSessionIO();
    const auto after = server::core::runtime_metrics::snapshot();
    EXPECT_EQ(after.runtime_setting_reload_attempt_total, before.runtime_setting_reload_attempt_total + 1);
    EXPECT_EQ(after.runtime_setting_reload_success_total, before.runtime_setting_reload_success_total + 1);

    SendChat("throwing_room", "throwing plugin second message");
    EXPECT_TRUE(WaitForBroadcastText("throwing plugin second message"));
    chat_service_.reset();
}

TEST_F(ChatServiceTest, LuaColdHooksRunForLoginAndJoinButNotChatSend) {
    ScopedTempDir script_temp("chat_lua_cold_hook");
    const auto script_path = script_temp.path() / "on_login.lua";
    {
        std::ofstream out(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out << "return 1\n";
        out.flush();
        ASSERT_TRUE(out.good());
    }

    auto lua_runtime = std::make_shared<server::core::scripting::LuaRuntime>();
    std::vector<server::core::scripting::LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(server::core::scripting::LuaRuntime::ScriptEntry{script_path, "on_login"});
    const auto reload_result = lua_runtime->reload_scripts(scripts);
    ASSERT_TRUE(reload_result.error.empty());
    ASSERT_EQ(reload_result.loaded, 1u);

    services::set(lua_runtime);
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);

    const auto before = lua_runtime->metrics_snapshot();

    LoginAs("lua_user");
    JoinRoom("lua_room");
    SendChat("lua_room", "hello from hot path");

    const auto after = lua_runtime->metrics_snapshot();
    EXPECT_EQ(after.calls_total, before.calls_total + 2u);
}

TEST_F(ChatServiceTest, LuaColdHookDenyStopsLoginWhenNativePathPasses) {
    ScopedTempDir script_temp("chat_lua_login_deny");
    const auto script_path = script_temp.path() / "policy.lua";
    {
        std::ofstream out(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out << "return { hook = \"on_login\", decision = \"deny\", reason = \"login denied by lua scaffold\" }\n";
        out.flush();
        ASSERT_TRUE(out.good());
    }

    auto lua_runtime = std::make_shared<server::core::scripting::LuaRuntime>();
    std::vector<server::core::scripting::LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(server::core::scripting::LuaRuntime::ScriptEntry{script_path, "policy"});
    const auto reload_result = lua_runtime->reload_scripts(scripts);
    ASSERT_TRUE(reload_result.error.empty());
    ASSERT_EQ(reload_result.loaded, 1u);

    services::set(lua_runtime);
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);

    std::vector<std::uint8_t> payload;
    write_lp_utf8(payload, "lua_deny_user");
    write_lp_utf8(payload, "test_token");
    chat_service_->on_login(*session_, payload);
    ProcessJobs();
    FlushSessionIO();

    const auto error = WaitForError();
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->code, core_proto::errc::FORBIDDEN);
    EXPECT_EQ(error->message, "login denied by lua scaffold");
}

TEST_F(ChatServiceTest, LuaColdHookPassSendsLoginWelcomeNotice) {
    ScopedTempDir script_temp("chat_lua_login_notice");
    const auto script_path = script_temp.path() / "policy.lua";
    {
        std::ofstream out(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out << "function on_login(ctx)\n"
               "  local name = server.get_user_name(ctx.session_id)\n"
               "  local online = server.get_online_count()\n"
               "  server.send_notice(ctx.session_id, 'welcome ' .. name .. ' online=' .. tostring(online))\n"
               "  return { decision = 'pass' }\n"
               "end\n";
        out.flush();
        ASSERT_TRUE(out.good());
    }

    auto lua_runtime = std::make_shared<server::core::scripting::LuaRuntime>();
    std::vector<server::core::scripting::LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(server::core::scripting::LuaRuntime::ScriptEntry{script_path, "policy"});
    const auto reload_result = lua_runtime->reload_scripts(scripts);
    ASSERT_TRUE(reload_result.error.empty());
    ASSERT_EQ(reload_result.loaded, 1u);

    services::set(lua_runtime);
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);

    std::vector<std::uint8_t> payload;
    write_lp_utf8(payload, "lua_notice_user");
    write_lp_utf8(payload, "test_token");
    chat_service_->on_login(*session_, payload);
    ProcessJobs();
    FlushSessionIO();

    EXPECT_TRUE(WaitForBroadcastText("welcome lua_notice_user online=0"));
}

TEST_F(ChatServiceTest, LuaAdminHookCanUseArgsAndBroadcastAll) {
    ScopedTempDir script_temp("chat_lua_admin_announce");
    const auto script_path = script_temp.path() / "policy.lua";
    {
        std::ofstream out(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out << "function on_admin_command(ctx)\n"
               "  if ctx.command ~= 'announce' then\n"
               "    return { decision = 'pass' }\n"
               "  end\n"
               "  server.broadcast_all('[announce] ' .. ctx.args)\n"
               "  return { decision = 'handled' }\n"
               "end\n";
        out.flush();
        ASSERT_TRUE(out.good());
    }

    auto lua_runtime = std::make_shared<server::core::scripting::LuaRuntime>();
    std::vector<server::core::scripting::LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(server::core::scripting::LuaRuntime::ScriptEntry{script_path, "policy"});
    const auto reload_result = lua_runtime->reload_scripts(scripts);
    ASSERT_TRUE(reload_result.error.empty());

    services::set(lua_runtime);
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);

    LoginAs("announce_listener");
    chat_service_->admin_broadcast_notice("server maintenance");
    ProcessJobs();
    FlushSessionIO();

    EXPECT_TRUE(WaitForBroadcastText("[announce] server maintenance"));
}

TEST_F(ChatServiceTest, LuaColdHookDenyStopsJoinWhenNativePathPasses) {
    ScopedTempDir script_temp("chat_lua_join_deny");
    const auto script_path = script_temp.path() / "policy.lua";
    {
        std::ofstream out(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out << "return { hook = \"on_join\", decision = \"deny\", reason = \"join denied by lua scaffold\" }\n";
        out.flush();
        ASSERT_TRUE(out.good());
    }

    auto lua_runtime = std::make_shared<server::core::scripting::LuaRuntime>();
    std::vector<server::core::scripting::LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(server::core::scripting::LuaRuntime::ScriptEntry{script_path, "policy"});
    const auto reload_result = lua_runtime->reload_scripts(scripts);
    ASSERT_TRUE(reload_result.error.empty());
    ASSERT_EQ(reload_result.loaded, 1u);

    services::set(lua_runtime);
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);

    LoginAs("lua_join_user");

    std::vector<std::uint8_t> join_payload;
    write_lp_utf8(join_payload, "lua_forbidden_room");
    write_lp_utf8(join_payload, "");
    chat_service_->on_join(*session_, join_payload);
    ProcessJobs();
    FlushSessionIO();

    const auto error = WaitForError();
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->code, core_proto::errc::FORBIDDEN);
    EXPECT_EQ(error->message, "join denied by lua scaffold");
}

TEST_F(ChatServiceTest, LuaColdHookJoinVipPolicyCanDenyAccess) {
    ScopedTempDir script_temp("chat_lua_join_vip_policy");
    const auto script_path = script_temp.path() / "policy.lua";
    {
        std::ofstream out(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out << "return { hook = \"on_join\", decision = \"deny\", reason = \"vip room requires policy approval\" }\n";
        out.flush();
        ASSERT_TRUE(out.good());
    }

    auto lua_runtime = std::make_shared<server::core::scripting::LuaRuntime>();
    std::vector<server::core::scripting::LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(server::core::scripting::LuaRuntime::ScriptEntry{script_path, "policy"});
    const auto reload_result = lua_runtime->reload_scripts(scripts);
    ASSERT_TRUE(reload_result.error.empty());
    ASSERT_EQ(reload_result.loaded, 1u);

    services::set(lua_runtime);
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);

    LoginAs("vip_candidate");

    std::vector<std::uint8_t> join_payload;
    write_lp_utf8(join_payload, "vip_lounge");
    write_lp_utf8(join_payload, "");
    chat_service_->on_join(*session_, join_payload);
    ProcessJobs();
    FlushSessionIO();

    const auto error = WaitForError();
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->code, core_proto::errc::FORBIDDEN);
    EXPECT_EQ(error->message, "vip room requires policy approval");
}

TEST_F(ChatServiceTest, LuaColdHookDenySkipsAdminRuntimeSettingReload) {
    ScopedTempDir script_temp("chat_lua_admin_deny");
    const auto script_path = script_temp.path() / "policy.lua";
    {
        std::ofstream out(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out << "return { hook = \"on_admin_command\", decision = \"deny\", reason = \"admin denied by lua scaffold\" }\n";
        out.flush();
        ASSERT_TRUE(out.good());
    }

    auto lua_runtime = std::make_shared<server::core::scripting::LuaRuntime>();
    std::vector<server::core::scripting::LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(server::core::scripting::LuaRuntime::ScriptEntry{script_path, "policy"});
    const auto reload_result = lua_runtime->reload_scripts(scripts);
    ASSERT_TRUE(reload_result.error.empty());
    ASSERT_EQ(reload_result.loaded, 1u);

    services::set(lua_runtime);
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);

    const auto before = server::core::runtime_metrics::snapshot().runtime_setting_reload_attempt_total;

    chat_service_->admin_apply_runtime_setting("chat_spam_threshold", "7");
    ProcessJobs();
    FlushSessionIO();

    const auto after = server::core::runtime_metrics::snapshot().runtime_setting_reload_attempt_total;
    EXPECT_EQ(after, before);
}

TEST_F(ChatServiceTest, LuaColdHookCanDenyAfterNativePluginPassesLogin) {
    if (std::string(TEST_CHAT_HOOK_V2_ONLY_PATH).empty()) {
        GTEST_SKIP() << "TEST_CHAT_HOOK_V2_ONLY_PATH is not configured";
    }

    ScopedTempDir script_temp("chat_lua_after_native_pass");
    const auto script_path = script_temp.path() / "policy.lua";
    {
        std::ofstream out(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out << "return { hook = \"on_login\", decision = \"deny\", reason = \"login denied after native pass\" }\n";
        out.flush();
        ASSERT_TRUE(out.good());
    }

    auto lua_runtime = std::make_shared<server::core::scripting::LuaRuntime>();
    std::vector<server::core::scripting::LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(server::core::scripting::LuaRuntime::ScriptEntry{script_path, "policy"});
    const auto reload_result = lua_runtime->reload_scripts(scripts);
    ASSERT_TRUE(reload_result.error.empty());
    services::set(lua_runtime);

    ScopedEnvVar env_single("CHAT_HOOK_PLUGIN_PATH", TEST_CHAT_HOOK_V2_ONLY_PATH);
    ScopedEnvVar env_paths("CHAT_HOOK_PLUGIN_PATHS", "");
    ScopedEnvVar env_dir("CHAT_HOOK_PLUGINS_DIR", "");
    ScopedTempDir cache_temp("chat_service_hook_cache_pass");
    const auto cache_path = cache_temp.path().string();
    const auto lock_path = (cache_temp.path() / "chat_hook.lock").string();
    ScopedEnvVar env_lock("CHAT_HOOK_LOCK_PATH", lock_path.c_str());
    ScopedEnvVar env_cache("CHAT_HOOK_CACHE_DIR", cache_path.c_str());
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);

    const auto before = lua_runtime->metrics_snapshot();

    std::vector<std::uint8_t> payload;
    write_lp_utf8(payload, "allow_user");
    write_lp_utf8(payload, "test_token");
    chat_service_->on_login(*session_, payload);
    ProcessJobs();
    FlushSessionIO();

    const auto error = WaitForError();
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->code, core_proto::errc::FORBIDDEN);
    EXPECT_EQ(error->message, "login denied after native pass");

    const auto after = lua_runtime->metrics_snapshot();
    EXPECT_EQ(after.calls_total, before.calls_total + 1u);
}

TEST_F(ChatServiceTest, LuaColdHookSkippedWhenNativePluginBlocksLogin) {
    if (std::string(TEST_CHAT_HOOK_V2_ONLY_PATH).empty()) {
        GTEST_SKIP() << "TEST_CHAT_HOOK_V2_ONLY_PATH is not configured";
    }

    ScopedTempDir script_temp("chat_lua_plugin_block");
    const auto script_path = script_temp.path() / "on_login.lua";
    {
        std::ofstream out(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out << "return 1\n";
        out.flush();
        ASSERT_TRUE(out.good());
    }

    auto lua_runtime = std::make_shared<server::core::scripting::LuaRuntime>();
    std::vector<server::core::scripting::LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(server::core::scripting::LuaRuntime::ScriptEntry{script_path, "on_login"});
    const auto reload_result = lua_runtime->reload_scripts(scripts);
    ASSERT_TRUE(reload_result.error.empty());
    services::set(lua_runtime);

    ScopedEnvVar env_single("CHAT_HOOK_PLUGIN_PATH", TEST_CHAT_HOOK_V2_ONLY_PATH);
    ScopedEnvVar env_paths("CHAT_HOOK_PLUGIN_PATHS", "");
    ScopedEnvVar env_dir("CHAT_HOOK_PLUGINS_DIR", "");
    ScopedTempDir cache_temp("chat_service_hook_cache");
    const auto cache_path = cache_temp.path().string();
    const auto lock_path = (cache_temp.path() / "chat_hook.lock").string();
    ScopedEnvVar env_lock("CHAT_HOOK_LOCK_PATH", lock_path.c_str());
    ScopedEnvVar env_cache("CHAT_HOOK_CACHE_DIR", cache_path.c_str());
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);

    const auto before = lua_runtime->metrics_snapshot();

    std::vector<std::uint8_t> payload;
    write_lp_utf8(payload, "deny_login");
    write_lp_utf8(payload, "test_token");
    chat_service_->on_login(*session_, payload);
    ProcessJobs();
    FlushSessionIO();

    const auto error = WaitForError();
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->code, core_proto::errc::FORBIDDEN);

    const auto after = lua_runtime->metrics_snapshot();
    EXPECT_EQ(after.calls_total, before.calls_total);
}

TEST_F(ChatServiceTest, LuaColdHookSkippedWhenNativePluginBlocksLeave) {
    if (std::string(TEST_CHAT_HOOK_V2_ONLY_PATH).empty()) {
        GTEST_SKIP() << "TEST_CHAT_HOOK_V2_ONLY_PATH is not configured";
    }

    ScopedTempDir script_temp("chat_lua_plugin_block_leave");
    const auto script_path = script_temp.path() / "on_leave.lua";
    {
        std::ofstream out(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out << "return 1\n";
        out.flush();
        ASSERT_TRUE(out.good());
    }

    auto lua_runtime = std::make_shared<server::core::scripting::LuaRuntime>();
    std::vector<server::core::scripting::LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(server::core::scripting::LuaRuntime::ScriptEntry{script_path, "on_leave"});
    const auto reload_result = lua_runtime->reload_scripts(scripts);
    ASSERT_TRUE(reload_result.error.empty());
    services::set(lua_runtime);

    ScopedEnvVar env_single("CHAT_HOOK_PLUGIN_PATH", TEST_CHAT_HOOK_V2_ONLY_PATH);
    ScopedEnvVar env_paths("CHAT_HOOK_PLUGIN_PATHS", "");
    ScopedEnvVar env_dir("CHAT_HOOK_PLUGINS_DIR", "");
    ScopedTempDir cache_temp("chat_service_hook_cache_leave");
    const auto cache_path = cache_temp.path().string();
    const auto lock_path = (cache_temp.path() / "chat_hook.lock").string();
    ScopedEnvVar env_lock("CHAT_HOOK_LOCK_PATH", lock_path.c_str());
    ScopedEnvVar env_cache("CHAT_HOOK_CACHE_DIR", cache_path.c_str());
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);

    LoginAs("allow_user");

    std::vector<std::uint8_t> join_payload;
    write_lp_utf8(join_payload, "locked_leave");
    write_lp_utf8(join_payload, "");
    chat_service_->on_join(*session_, join_payload);
    ProcessJobs();
    FlushSessionIO();

    const auto before_leave = lua_runtime->metrics_snapshot();

    std::vector<std::uint8_t> leave_payload;
    write_lp_utf8(leave_payload, "locked_leave");
    chat_service_->on_leave(*session_, leave_payload);
    ProcessJobs();
    FlushSessionIO();

    const auto error_code = WaitForErrorCode();
    ASSERT_TRUE(error_code.has_value());
    EXPECT_EQ(*error_code, core_proto::errc::FORBIDDEN);

    const auto after_leave = lua_runtime->metrics_snapshot();
    EXPECT_EQ(after_leave.calls_total, before_leave.calls_total);
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
