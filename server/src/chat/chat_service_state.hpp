#pragma once

#include "server/chat/chat_service.hpp"

#include <chrono>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace server::app::chat {

struct ChatServiceHookPluginState;

using ChatWeakSession = std::weak_ptr<server::core::net::Session>;
using ChatWeakLess = std::owner_less<ChatWeakSession>;
using ChatRoomSet = std::set<ChatWeakSession, ChatWeakLess>;
using ChatExec = boost::asio::io_context::executor_type;
using ChatStrand = boost::asio::strand<ChatExec>;

/** @brief `ChatService`가 사용하는 비공개 세션/방/제재 메모리 상태입니다. */
struct ChatServiceState {
    /** @brief 만료 시각을 가진 제재 상태(뮤트/밴) 엔트리입니다. */
    struct TimedPenalty {
        std::chrono::steady_clock::time_point expires_at{};
        std::string reason;
    };

    std::mutex mu;

    // 방 관리
    std::unordered_map<std::string, ChatRoomSet> rooms;
    std::unordered_map<std::string, std::string> room_ids;
    std::unordered_map<std::string, std::string> room_passwords;
    std::unordered_map<std::string, std::string> room_owners;
    std::unordered_map<std::string, std::unordered_set<std::string>> room_invites;

    // 유저/세션 관리
    std::unordered_map<server::core::net::Session*, std::string> user;
    std::unordered_map<server::core::net::Session*, std::string> user_uuid;
    std::unordered_map<server::core::net::Session*, std::string> session_uuid;
    std::unordered_map<server::core::net::Session*, std::string> logical_session_id;
    std::unordered_map<server::core::net::Session*, std::uint64_t> logical_session_expires_unix_ms;
    std::unordered_map<server::core::net::Session*, std::string> cur_world;
    std::unordered_map<server::core::net::Session*, std::string> cur_room;
    std::unordered_map<server::core::net::Session*, std::string> session_ip;
    std::unordered_map<server::core::net::Session*, std::string> session_hwid_hash;
    std::unordered_map<std::string, std::string> user_last_ip;
    std::unordered_map<std::string, std::string> user_last_hwid_hash;

    // 세션 집합
    std::unordered_set<server::core::net::Session*> authed;
    std::unordered_set<server::core::net::Session*> guest;

    // 닉네임 역참조 (중복 로그인 방지 및 귓속말용)
    std::unordered_map<std::string, ChatRoomSet> by_user;
    std::unordered_map<std::uint32_t, ChatWeakSession> by_session_id;

    // 제재/스팸 관리
    std::unordered_map<std::string, TimedPenalty> muted_users;
    std::unordered_map<std::string, TimedPenalty> banned_users;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> banned_ips;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> banned_hwid_hashes;
    std::unordered_map<std::string, std::deque<std::chrono::steady_clock::time_point>> spam_events;
    std::unordered_map<std::string, std::uint32_t> spam_violations;
    std::unordered_map<std::string, std::unordered_set<std::string>> user_blacklists;
};

/** @brief Lua hook 실행기와 room-local strand 테이블을 묶는 비공개 dispatch 상태입니다. */
struct ChatServiceDispatchState {
    std::shared_ptr<ChatStrand> lua_execution_strand{};
    std::unordered_map<std::string, std::shared_ptr<ChatStrand>> room_strands;
};

/** @brief write-behind 파라미터 묶음입니다. */
struct ChatServiceWriteBehindConfig {
    bool enabled{false};
    std::string stream_key{"session_events"};
    std::optional<std::size_t> maxlen{};
    bool approximate{true};
};

/** @brief presence TTL/prefix 설정입니다. */
struct ChatServicePresenceConfig {
    unsigned int ttl{30};
    std::string prefix;
};

/** @brief reconnect/resume continuity 설정입니다. */
struct ChatServiceContinuityConfig {
    bool enabled{false};
    unsigned int lease_ttl_sec{15 * 60};
    std::string redis_prefix;
    std::string default_world_id;
    std::string current_owner_id;
    std::string topology_runtime_assignment_key;
};

/** @brief 최근 메시지 캐시 설정입니다. */
struct ChatServiceHistoryConfig {
    std::size_t recent_limit{20};
    std::size_t max_list_len{200};
    std::size_t fetch_factor{3};
    unsigned int cache_ttl_sec{6 * 60 * 60};
};

/** @brief 앱-로컬 서비스 의존성과 운영 중 조정 가능한 설정 묶음입니다. */
struct ChatServiceRuntimeState {
    std::shared_ptr<server::storage::IRepositoryConnectionPool> db_pool{};
    std::shared_ptr<server::core::storage::redis::IRedisClient> redis{};
    std::shared_ptr<server::core::scripting::LuaRuntime> lua_runtime{};
    std::string gateway_id{"gw-default"};
    bool redis_pubsub_enabled{false};
    std::unordered_set<std::string> admin_users{};
    std::size_t spam_message_threshold{6};
    std::uint32_t spam_window_sec{5};
    std::uint32_t spam_mute_sec{30};
    std::uint32_t spam_ban_sec{600};
    std::uint32_t spam_ban_violation_threshold{3};
    std::uint64_t lua_auto_disable_threshold{3};
    std::uint64_t lua_hook_warn_budget_us{0};
    std::unique_ptr<ChatServiceHookPluginState> hook_plugin{};
};

/** @brief continuity 관련 Prometheus 카운터를 한곳에 모은 비공개 상태입니다. */
struct ChatServiceContinuityMetricsState {
    std::atomic<std::uint64_t> lease_issue_total{0};
    std::atomic<std::uint64_t> lease_issue_fail_total{0};
    std::atomic<std::uint64_t> lease_resume_total{0};
    std::atomic<std::uint64_t> lease_resume_fail_total{0};
    std::atomic<std::uint64_t> state_write_total{0};
    std::atomic<std::uint64_t> state_write_fail_total{0};
    std::atomic<std::uint64_t> state_restore_total{0};
    std::atomic<std::uint64_t> state_restore_fallback_total{0};
    std::atomic<std::uint64_t> world_write_total{0};
    std::atomic<std::uint64_t> world_write_fail_total{0};
    std::atomic<std::uint64_t> world_restore_total{0};
    std::atomic<std::uint64_t> world_restore_fallback_total{0};
    std::atomic<std::uint64_t> world_restore_fallback_missing_world_total{0};
    std::atomic<std::uint64_t> world_restore_fallback_missing_owner_total{0};
    std::atomic<std::uint64_t> world_restore_fallback_owner_mismatch_total{0};
    std::atomic<std::uint64_t> world_restore_fallback_draining_replacement_unhonored_total{0};
    std::atomic<std::uint64_t> world_owner_write_total{0};
    std::atomic<std::uint64_t> world_owner_write_fail_total{0};
    std::atomic<std::uint64_t> world_owner_restore_total{0};
    std::atomic<std::uint64_t> world_owner_restore_fallback_total{0};
    std::atomic<std::uint64_t> world_migration_restore_total{0};
    std::atomic<std::uint64_t> world_migration_restore_fallback_total{0};
    std::atomic<std::uint64_t> world_migration_restore_fallback_target_world_missing_total{0};
    std::atomic<std::uint64_t> world_migration_restore_fallback_target_owner_missing_total{0};
    std::atomic<std::uint64_t> world_migration_restore_fallback_target_owner_not_ready_total{0};
    std::atomic<std::uint64_t> world_migration_restore_fallback_target_owner_mismatch_total{0};
    std::atomic<std::uint64_t> world_migration_restore_fallback_source_not_draining_total{0};
    std::atomic<std::uint64_t> world_migration_payload_room_handoff_total{0};
    std::atomic<std::uint64_t> world_migration_payload_room_handoff_fallback_total{0};
};

/** @brief Lua hook 계측과 auto-disable bookkeeping 상태입니다. */
struct ChatServiceLuaMetricsState {
    std::mutex mu;
    std::unordered_map<std::string, std::uint64_t> consecutive_failures;
    std::unordered_map<std::string, std::uint64_t> auto_disable_total;
    std::unordered_map<std::string, std::uint64_t> calls_total;
    std::unordered_map<std::string, std::uint64_t> errors_total;
    std::unordered_map<std::string, std::uint64_t> instruction_limit_hits;
    std::unordered_map<std::string, std::uint64_t> memory_limit_hits;
    std::unordered_map<std::string, std::unordered_map<std::string, std::uint64_t>> script_calls_total;
    std::unordered_map<std::string, std::unordered_map<std::string, std::uint64_t>> script_errors_total;
    std::unordered_set<std::string> disabled;
    std::uint64_t reload_epoch{0};
};

/** @brief `ChatService` 내부 config/state/dispatch 묶음을 숨기는 단일 구현 상태입니다. */
struct ChatService::Impl {
    ChatServiceRuntimeState runtime{};
    ChatServiceWriteBehindConfig write_behind{};
    ChatServicePresenceConfig presence{};
    ChatServiceContinuityConfig continuity{};
    ChatServiceHistoryConfig history{};
    ChatServiceContinuityMetricsState continuity_metrics{};
    ChatServiceLuaMetricsState lua_metrics{};
    ChatServiceDispatchState dispatch{};
    ChatServiceState state{};
};

inline void collect_room_sessions(
    ChatRoomSet& set,
    std::vector<std::shared_ptr<server::core::net::Session>>& out) {
    for (auto it = set.begin(); it != set.end();) {
        if (auto locked = it->lock()) {
            out.push_back(std::move(locked));
            ++it;
        } else {
            it = set.erase(it);
        }
    }
}

} // namespace server::app::chat
