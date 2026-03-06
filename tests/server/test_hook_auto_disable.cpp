#include <gtest/gtest.h>

#include <server/chat/chat_service.hpp>
#include <server/core/concurrent/job_queue.hpp>
#include <server/core/config/options.hpp>
#include <server/core/net/connection_runtime_state.hpp>
#include <server/core/net/dispatcher.hpp>
#include <server/core/net/session.hpp>
#include <server/core/runtime_metrics.hpp>
#include <server/core/scripting/lua_runtime.hpp>
#include <server/core/storage/connection_pool.hpp>
#include <server/core/storage/unit_of_work.hpp>
#include <server/core/util/service_registry.hpp>
#include <server/storage/redis/client.hpp>

#include <boost/asio.hpp>

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

namespace {

namespace services = server::core::util::services;

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

class HookAutoDisableTest : public ::testing::Test {
protected:
    boost::asio::io_context io_;
    server::core::JobQueue job_queue_;
    std::shared_ptr<MockConnectionPool> db_pool_ = std::make_shared<MockConnectionPool>();
    std::shared_ptr<MockRedisClient> redis_ = std::make_shared<MockRedisClient>();
    std::shared_ptr<server::core::scripting::LuaRuntime> lua_runtime_ = std::make_shared<server::core::scripting::LuaRuntime>();
    std::unique_ptr<server::app::chat::ChatService> chat_service_;

    void SetUp() override {
        services::clear();
        services::set(lua_runtime_);
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
};

TEST_F(HookAutoDisableTest, LuaHookAutoDisablesAfterConsecutiveFailuresAndReenablesOnReload) {
#if !KNIGHTS_BUILD_LUA_SCRIPTING
    GTEST_SKIP() << "Lua scripting build flag is disabled";
#else
    ScopedTempDir script_temp("knights_hook_auto_disable");
    const auto bad_script_path = script_temp.path() / "policy_bad.lua";
    {
        std::ofstream out(bad_script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out << "return { hook = \"on_admin_command\", decision = \"denyy\", reason = \"invalid token\" }\n";
    }

    std::vector<server::core::scripting::LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(server::core::scripting::LuaRuntime::ScriptEntry{bad_script_path, "policy"});
    const auto reload_bad = lua_runtime_->reload_scripts(scripts);
    ASSERT_TRUE(reload_bad.error.empty());

    ScopedEnvVar threshold("LUA_AUTO_DISABLE_THRESHOLD", "3");
    chat_service_ = std::make_unique<server::app::chat::ChatService>(io_, job_queue_, db_pool_, redis_);

    const auto before_calls = lua_runtime_->metrics_snapshot().calls_total;

    chat_service_->admin_apply_runtime_setting("chat_spam_threshold", "7");
    ProcessJobs();
    chat_service_->admin_apply_runtime_setting("chat_spam_threshold", "8");
    ProcessJobs();
    chat_service_->admin_apply_runtime_setting("chat_spam_threshold", "9");
    ProcessJobs();
    chat_service_->admin_apply_runtime_setting("chat_spam_threshold", "10");
    ProcessJobs();

    const auto after_calls = lua_runtime_->metrics_snapshot().calls_total;
    EXPECT_EQ(after_calls, before_calls + 3u);

    const auto disabled_metrics = chat_service_->lua_hooks_metrics();
    bool found_hook = false;
    for (const auto& hook : disabled_metrics.hooks) {
        if (hook.hook_name == "on_admin_command") {
            found_hook = true;
            EXPECT_TRUE(hook.disabled);
            EXPECT_EQ(hook.auto_disable_total, 1u);
            EXPECT_GE(hook.consecutive_failures, 3u);
        }
    }
    EXPECT_TRUE(found_hook);

    const auto good_script_path = script_temp.path() / "policy_good.lua";
    {
        std::ofstream out(good_script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out << "return { hook = \"on_admin_command\", decision = \"pass\" }\n";
    }
    scripts.clear();
    scripts.push_back(server::core::scripting::LuaRuntime::ScriptEntry{good_script_path, "policy"});
    const auto reload_good = lua_runtime_->reload_scripts(scripts);
    ASSERT_TRUE(reload_good.error.empty());

    const auto calls_before_reenable = lua_runtime_->metrics_snapshot().calls_total;
    chat_service_->admin_apply_runtime_setting("chat_spam_threshold", "11");
    ProcessJobs();
    const auto calls_after_reenable = lua_runtime_->metrics_snapshot().calls_total;
    EXPECT_EQ(calls_after_reenable, calls_before_reenable + 1u);

    const auto reenabled_metrics = chat_service_->lua_hooks_metrics();
    for (const auto& hook : reenabled_metrics.hooks) {
        if (hook.hook_name == "on_admin_command") {
            EXPECT_FALSE(hook.disabled);
            EXPECT_EQ(hook.consecutive_failures, 0u);
        }
    }
#endif
}

TEST_F(HookAutoDisableTest, LuaInstructionLimitFailureDoesNotStopAdminSettingPath) {
#if !KNIGHTS_BUILD_LUA_SCRIPTING
    GTEST_SKIP() << "Lua scripting build flag is disabled";
#else
    ScopedTempDir script_temp("knights_hook_instruction_limit");
    const auto script_path = script_temp.path() / "policy_instruction.lua";
    {
        std::ofstream out(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out << "function on_admin_command(ctx)\n"
               "  while true do\n"
               "  end\n"
               "end\n";
    }

    std::vector<server::core::scripting::LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(server::core::scripting::LuaRuntime::ScriptEntry{script_path, "policy"});
    const auto reload = lua_runtime_->reload_scripts(scripts);
    ASSERT_TRUE(reload.error.empty());

    ScopedEnvVar threshold("LUA_AUTO_DISABLE_THRESHOLD", "100");
    chat_service_ = std::make_unique<server::app::chat::ChatService>(io_, job_queue_, db_pool_, redis_);

    const auto before_runtime = server::core::runtime_metrics::snapshot();
    const auto before_lua = lua_runtime_->metrics_snapshot();

    chat_service_->admin_apply_runtime_setting("chat_spam_threshold", "12");
    ProcessJobs();
    chat_service_->admin_apply_runtime_setting("chat_spam_threshold", "13");
    ProcessJobs();

    const auto after_runtime = server::core::runtime_metrics::snapshot();
    const auto after_lua = lua_runtime_->metrics_snapshot();

    EXPECT_EQ(after_lua.instruction_limit_hits, before_lua.instruction_limit_hits + 2u);
    EXPECT_EQ(after_lua.memory_limit_hits, before_lua.memory_limit_hits);
    EXPECT_EQ(after_runtime.runtime_setting_reload_attempt_total, before_runtime.runtime_setting_reload_attempt_total + 2u);
    EXPECT_EQ(after_runtime.runtime_setting_reload_success_total, before_runtime.runtime_setting_reload_success_total + 2u);

    bool found_hook = false;
    const auto hook_metrics = chat_service_->lua_hooks_metrics();
    for (const auto& hook : hook_metrics.hooks) {
        if (hook.hook_name == "on_admin_command") {
            found_hook = true;
            EXPECT_FALSE(hook.disabled);
            EXPECT_EQ(hook.instruction_limit_hits, 2u);
            EXPECT_EQ(hook.memory_limit_hits, 0u);
        }
    }
    EXPECT_TRUE(found_hook);
#endif
}

TEST_F(HookAutoDisableTest, LuaMemoryLimitFailureDoesNotStopAdminSettingPath) {
#if !KNIGHTS_BUILD_LUA_SCRIPTING
    GTEST_SKIP() << "Lua scripting build flag is disabled";
#else
    ScopedTempDir script_temp("knights_hook_memory_limit");
    const auto script_path = script_temp.path() / "policy_memory.lua";
    {
        std::ofstream out(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out << "function on_admin_command(ctx)\n"
               "  local values = {}\n"
               "  for i = 1, 4096 do\n"
               "    values[i] = tostring(i) .. string.rep('x', 1024)\n"
               "  end\n"
               "  return { decision = 'pass' }\n"
               "end\n";
    }

    std::vector<server::core::scripting::LuaRuntime::ScriptEntry> scripts;
    scripts.push_back(server::core::scripting::LuaRuntime::ScriptEntry{script_path, "policy"});
    const auto reload = lua_runtime_->reload_scripts(scripts);
    ASSERT_TRUE(reload.error.empty());

    ScopedEnvVar threshold("LUA_AUTO_DISABLE_THRESHOLD", "100");
    chat_service_ = std::make_unique<server::app::chat::ChatService>(io_, job_queue_, db_pool_, redis_);

    const auto before_runtime = server::core::runtime_metrics::snapshot();
    const auto before_lua = lua_runtime_->metrics_snapshot();

    chat_service_->admin_apply_runtime_setting("chat_spam_threshold", "14");
    ProcessJobs();
    chat_service_->admin_apply_runtime_setting("chat_spam_threshold", "15");
    ProcessJobs();

    const auto after_runtime = server::core::runtime_metrics::snapshot();
    const auto after_lua = lua_runtime_->metrics_snapshot();

    EXPECT_EQ(after_lua.memory_limit_hits, before_lua.memory_limit_hits + 2u);
    EXPECT_EQ(after_lua.instruction_limit_hits, before_lua.instruction_limit_hits);
    EXPECT_EQ(after_runtime.runtime_setting_reload_attempt_total, before_runtime.runtime_setting_reload_attempt_total + 2u);
    EXPECT_EQ(after_runtime.runtime_setting_reload_success_total, before_runtime.runtime_setting_reload_success_total + 2u);

    bool found_hook = false;
    const auto hook_metrics = chat_service_->lua_hooks_metrics();
    for (const auto& hook : hook_metrics.hooks) {
        if (hook.hook_name == "on_admin_command") {
            found_hook = true;
            EXPECT_FALSE(hook.disabled);
            EXPECT_EQ(hook.memory_limit_hits, 2u);
            EXPECT_EQ(hook.instruction_limit_hits, 0u);
        }
    }
    EXPECT_TRUE(found_hook);
#endif
}

} // namespace
