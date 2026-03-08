#include <gtest/gtest.h>

#include <server/chat/chat_service.hpp>
#include <server/core/concurrent/job_queue.hpp>
#include <server/core/config/options.hpp>
#include <server/core/net/connection_runtime_state.hpp>
#include <server/core/net/dispatcher.hpp>
#include <server/core/net/session.hpp>
#include <server/core/protocol/packet.hpp>
#include <server/core/protocol/protocol_errors.hpp>
#include <server/core/scripting/lua_runtime.hpp>
#include <server/core/storage/connection_pool.hpp>
#include <server/core/storage/unit_of_work.hpp>
#include <server/core/util/service_registry.hpp>
#include <server/protocol/game_opcodes.hpp>
#include <server/storage/redis/client.hpp>

#include <boost/asio.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef TEST_CHAT_HOOK_V2_ONLY_PATH
#define TEST_CHAT_HOOK_V2_ONLY_PATH ""
#endif

namespace {

namespace core_proto = server::core::protocol;
namespace services = server::core::util::services;

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

class MockUserRepository : public server::core::storage::IUserRepository {
public:
    std::optional<server::core::storage::User> find_by_id(const std::string&) override { return std::nullopt; }
    std::vector<server::core::storage::User> find_by_name_ci(const std::string&, std::size_t) override { return {}; }
    server::core::storage::User create_guest(const std::string&) override { return {}; }
    void update_last_login(const std::string&, const std::string&) override {}
};

class MockRoomRepository : public server::core::storage::IRoomRepository {
public:
    std::optional<server::core::storage::Room> find_by_id(const std::string&) override { return std::nullopt; }
    std::vector<server::core::storage::Room> search_by_name_ci(const std::string&, std::size_t) override { return {}; }
    std::optional<server::core::storage::Room> find_by_name_exact_ci(const std::string&) override { return std::nullopt; }
    server::core::storage::Room create(const std::string&, bool) override { return {}; }
    void close(const std::string&) override {}
};

class MockMessageRepository : public server::core::storage::IMessageRepository {
public:
    std::vector<server::core::storage::Message> fetch_recent_by_room(const std::string&, std::uint64_t, std::size_t) override {
        return {};
    }
    server::core::storage::Message create(
        const std::string&, const std::string&, const std::optional<std::string>&, const std::string&) override {
        return {};
    }
    std::uint64_t get_last_id(const std::string&) override { return 0; }
    void delete_by_room(const std::string&) override {}
};

class MockMembershipRepository : public server::core::storage::IMembershipRepository {
public:
    void upsert_join(const std::string&, const std::string&, const std::string&) override {}
    void update_last_seen(const std::string&, const std::string&, std::uint64_t) override {}
    void leave(const std::string&, const std::string&) override {}
    std::optional<std::uint64_t> get_last_seen(const std::string&, const std::string&) override { return std::nullopt; }
};

class MockSessionRepository : public server::core::storage::ISessionRepository {
public:
    std::optional<server::core::storage::Session> find_by_token_hash(const std::string&) override { return std::nullopt; }
    server::core::storage::Session create(const std::string&, const std::chrono::system_clock::time_point&, const std::optional<std::string>&, const std::optional<std::string>&, const std::string&) override { return {}; }
    void revoke(const std::string&) override {}
};

class MockUnitOfWork : public server::core::storage::IUnitOfWork {
public:
    MockUserRepository user_repo;
    MockRoomRepository room_repo;
    MockMessageRepository message_repo;
    MockMembershipRepository membership_repo;
    MockSessionRepository session_repo;

    void commit() override {}
    void rollback() override {}
    server::core::storage::IUserRepository& users() override { return user_repo; }
    server::core::storage::IRoomRepository& rooms() override { return room_repo; }
    server::core::storage::IMessageRepository& messages() override { return message_repo; }
    server::core::storage::ISessionRepository& sessions() override { return session_repo; }
    server::core::storage::IMembershipRepository& memberships() override { return membership_repo; }
};

class MockConnectionPool : public server::core::storage::IConnectionPool {
public:
    std::unique_ptr<server::core::storage::IUnitOfWork> make_unit_of_work() override {
        return std::make_unique<MockUnitOfWork>();
    }
    bool health_check() override { return true; }
};

class MockRedisClient : public server::storage::redis::IRedisClient {
public:
    bool health_check() override { return true; }
    bool lpush_trim(const std::string&, const std::string&, std::size_t) override { return true; }
    bool sadd(const std::string&, const std::string&) override { return true; }
    bool srem(const std::string&, const std::string&) override { return true; }
    bool smembers(const std::string&, std::vector<std::string>&) override { return true; }
    bool scard(const std::string&, std::size_t& out) override { out = 0; return true; }
    bool scard_many(const std::vector<std::string>& keys, std::vector<std::size_t>& out) override {
        out.assign(keys.size(), 0);
        return true;
    }
    bool del(const std::string&) override { return true; }
    std::optional<std::string> get(const std::string&) override { return std::nullopt; }
    bool mget(const std::vector<std::string>& keys, std::vector<std::optional<std::string>>& out) override {
        out.assign(keys.size(), std::nullopt);
        return true;
    }
    bool set_if_not_exists(const std::string&, const std::string&, unsigned int) override { return true; }
    bool set_if_equals(const std::string&, const std::string&, const std::string&, unsigned int) override { return true; }
    bool del_if_equals(const std::string&, const std::string&) override { return true; }
    bool scan_keys(const std::string&, std::vector<std::string>&) override { return true; }
    bool lrange(const std::string&, long long, long long, std::vector<std::string>&) override { return true; }
    bool scan_del(const std::string&) override { return true; }
    bool setex(const std::string&, const std::string&, unsigned int) override { return true; }
    bool publish(const std::string&, const std::string&) override { return true; }
    bool start_psubscribe(const std::string&, std::function<void(const std::string&, const std::string&)>) override { return true; }
    void stop_psubscribe() override {}
    bool xgroup_create_mkstream(const std::string&, const std::string&) override { return true; }
    bool xadd(const std::string&, const std::vector<std::pair<std::string, std::string>>&, std::string*, std::optional<std::size_t>, bool) override { return true; }
    bool xreadgroup(const std::string&, const std::string&, const std::string&, long long, std::size_t, std::vector<StreamEntry>&) override { return true; }
    bool xack(const std::string&, const std::string&, const std::string&) override { return true; }
    bool xpending(const std::string&, const std::string&, long long&) override { return true; }
    bool xautoclaim(
        const std::string&, const std::string&, const std::string&, long long, const std::string& start,
        std::size_t, StreamAutoClaimResult& out) override {
        out.next_start_id = start;
        out.entries.clear();
        out.deleted_ids.clear();
        return true;
    }
};

class LuaHookIntegrationTest : public ::testing::Test {
protected:
    using ChatService = server::app::chat::ChatService;

    boost::asio::io_context io_;
    server::core::JobQueue job_queue_;
    std::shared_ptr<MockConnectionPool> db_pool_ = std::make_shared<MockConnectionPool>();
    std::shared_ptr<MockRedisClient> redis_ = std::make_shared<MockRedisClient>();
    std::unique_ptr<ChatService> chat_service_;
    std::shared_ptr<server::core::scripting::LuaRuntime> lua_runtime_;

    server::core::Dispatcher dispatcher_;
    server::core::BufferManager buffer_manager_{1024, 10};
    std::shared_ptr<server::core::SessionOptions> session_options_ = std::make_shared<server::core::SessionOptions>();
    std::shared_ptr<server::core::net::ConnectionRuntimeState> shared_state_ = std::make_shared<server::core::net::ConnectionRuntimeState>();
    std::shared_ptr<server::core::net::Session> session_;
    boost::asio::ip::tcp::acceptor acceptor_{io_};
    boost::asio::ip::tcp::socket peer_socket_{io_};

    void SetUp() override {
        services::clear();
        chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);

        boost::asio::ip::tcp::endpoint ep(boost::asio::ip::make_address("127.0.0.1"), 0);
        acceptor_.open(ep.protocol());
        acceptor_.bind(ep);
        acceptor_.listen();

        boost::asio::ip::tcp::socket socket(io_);
        socket.connect(acceptor_.local_endpoint());
        acceptor_.accept(peer_socket_);

        session_ = std::make_shared<server::core::net::Session>(
            std::move(socket), dispatcher_, buffer_manager_, session_options_, shared_state_);
        session_->start();
    }

    void TearDown() override {
        chat_service_.reset();
        services::clear();
    }

    void ProcessJobs() {
        std::promise<void> done;
        std::future<void> fut = done.get_future();
        std::thread worker([&]() {
            auto job = job_queue_.Pop();
            if (job) {
                job();
            }
            done.set_value();
        });
        (void)fut.wait_for(std::chrono::milliseconds(500));
        worker.join();
    }

    void FlushSessionIO() {
        if (io_.stopped()) {
            io_.restart();
        }
        while (io_.poll() > 0) {}
        io_.restart();
    }

    std::optional<std::string> WaitForErrorMessage(int timeout_ms = 400) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        boost::system::error_code ec;

        while (std::chrono::steady_clock::now() < deadline) {
            FlushSessionIO();
            const std::size_t available = peer_socket_.available(ec);
            if (ec || available < core_proto::k_header_bytes) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            std::array<std::uint8_t, core_proto::k_header_bytes> header_buf{};
            boost::asio::read(peer_socket_, boost::asio::buffer(header_buf), ec);
            if (ec) {
                return std::nullopt;
            }

            core_proto::PacketHeader header{};
            core_proto::decode_header(header_buf.data(), header);

            std::vector<std::uint8_t> payload(header.length, 0);
            if (header.length > 0) {
                boost::asio::read(peer_socket_, boost::asio::buffer(payload), ec);
                if (ec) {
                    return std::nullopt;
                }
            }

            if (header.msg_id != core_proto::MSG_ERR || payload.size() < 4) {
                continue;
            }

            const auto msg_len = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(payload[2]) << 8) | payload[3]);
            if (payload.size() < 4u + static_cast<std::size_t>(msg_len)) {
                return std::nullopt;
            }
            return std::string(reinterpret_cast<const char*>(payload.data() + 4), msg_len);
        }

        return std::nullopt;
    }

    void RecreateChatServiceWithLuaAndOptionalPlugin(const char* plugin_path) {
        chat_service_.reset();
        lua_runtime_ = std::make_shared<server::core::scripting::LuaRuntime>();
        services::set(lua_runtime_);

        ScopedEnvVar env_single("CHAT_HOOK_PLUGIN_PATH", plugin_path ? plugin_path : "");
        ScopedEnvVar env_paths("CHAT_HOOK_PLUGIN_PATHS", "");
        ScopedEnvVar env_dir("CHAT_HOOK_PLUGINS_DIR", "");
        ScopedTempDir cache_temp("lua_hook_integration_cache");
        const auto cache_path = cache_temp.path().string();
        const auto lock_path = (cache_temp.path() / "chat_hook.lock").string();
        ScopedEnvVar env_lock("CHAT_HOOK_LOCK_PATH", lock_path.c_str());
        ScopedEnvVar env_cache("CHAT_HOOK_CACHE_DIR", cache_path.c_str());

        chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);
    }
};

TEST_F(LuaHookIntegrationTest, NativePassThenLuaDenyOnLogin) {
    if (std::string(TEST_CHAT_HOOK_V2_ONLY_PATH).empty()) {
        GTEST_SKIP() << "TEST_CHAT_HOOK_V2_ONLY_PATH is not configured";
    }

    ScopedTempDir script_temp("lua_hook_integration_pass_then_deny");
    const auto script_path = script_temp.path() / "policy.lua";
    {
        std::ofstream out(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out << "return { hook = \"on_login\", decision = \"deny\", reason = \"login denied after native pass\" }\n";
    }

    lua_runtime_ = std::make_shared<server::core::scripting::LuaRuntime>();
    std::vector<server::core::scripting::LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(server::core::scripting::LuaRuntime::ScriptEntry{script_path, "policy"});
    const auto reload = lua_runtime_->reload_scripts(scripts);
    ASSERT_TRUE(reload.error.empty());

    services::set(lua_runtime_);

    ScopedEnvVar env_single("CHAT_HOOK_PLUGIN_PATH", TEST_CHAT_HOOK_V2_ONLY_PATH);
    ScopedEnvVar env_paths("CHAT_HOOK_PLUGIN_PATHS", "");
    ScopedEnvVar env_dir("CHAT_HOOK_PLUGINS_DIR", "");
    ScopedTempDir cache_temp("lua_hook_integration_cache_pass");
    const auto cache_path = cache_temp.path().string();
    const auto lock_path = (cache_temp.path() / "chat_hook.lock").string();
    ScopedEnvVar env_lock("CHAT_HOOK_LOCK_PATH", lock_path.c_str());
    ScopedEnvVar env_cache("CHAT_HOOK_CACHE_DIR", cache_path.c_str());
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);

    const auto before = lua_runtime_->metrics_snapshot();
    std::vector<std::uint8_t> payload;
    write_lp_utf8(payload, "allow_user");
    write_lp_utf8(payload, "token");
    chat_service_->on_login(*session_, payload);
    ProcessJobs();
    FlushSessionIO();

    const auto error_message = WaitForErrorMessage();
    ASSERT_TRUE(error_message.has_value());
    EXPECT_EQ(*error_message, "login denied after native pass");
    const auto after = lua_runtime_->metrics_snapshot();
    EXPECT_EQ(after.calls_total, before.calls_total + 1u);
}

TEST_F(LuaHookIntegrationTest, NativeBlockSkipsLuaOnLogin) {
    if (std::string(TEST_CHAT_HOOK_V2_ONLY_PATH).empty()) {
        GTEST_SKIP() << "TEST_CHAT_HOOK_V2_ONLY_PATH is not configured";
    }

    ScopedTempDir script_temp("lua_hook_integration_native_block");
    const auto script_path = script_temp.path() / "policy.lua";
    {
        std::ofstream out(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out << "return 1\n";
    }

    lua_runtime_ = std::make_shared<server::core::scripting::LuaRuntime>();
    std::vector<server::core::scripting::LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(server::core::scripting::LuaRuntime::ScriptEntry{script_path, "on_login"});
    const auto reload = lua_runtime_->reload_scripts(scripts);
    ASSERT_TRUE(reload.error.empty());
    services::set(lua_runtime_);

    ScopedEnvVar env_single("CHAT_HOOK_PLUGIN_PATH", TEST_CHAT_HOOK_V2_ONLY_PATH);
    ScopedEnvVar env_paths("CHAT_HOOK_PLUGIN_PATHS", "");
    ScopedEnvVar env_dir("CHAT_HOOK_PLUGINS_DIR", "");
    ScopedTempDir cache_temp("lua_hook_integration_cache_block");
    const auto cache_path = cache_temp.path().string();
    const auto lock_path = (cache_temp.path() / "chat_hook.lock").string();
    ScopedEnvVar env_lock("CHAT_HOOK_LOCK_PATH", lock_path.c_str());
    ScopedEnvVar env_cache("CHAT_HOOK_CACHE_DIR", cache_path.c_str());
    chat_service_ = std::make_unique<ChatService>(io_, job_queue_, db_pool_, redis_);

    const auto before = lua_runtime_->metrics_snapshot();
    std::vector<std::uint8_t> payload;
    write_lp_utf8(payload, "deny_login");
    write_lp_utf8(payload, "token");
    chat_service_->on_login(*session_, payload);
    ProcessJobs();
    FlushSessionIO();

    const auto error_message = WaitForErrorMessage();
    ASSERT_TRUE(error_message.has_value());
    EXPECT_EQ(*error_message, "login blocked by v2-only test plugin");
    const auto after = lua_runtime_->metrics_snapshot();
    EXPECT_EQ(after.calls_total, before.calls_total);
}

} // namespace
