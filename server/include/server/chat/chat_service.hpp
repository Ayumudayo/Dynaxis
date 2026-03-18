#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdint>
#include <array>
#include <chrono>
#include <deque>

#include <boost/asio.hpp>

#include "server/core/net/session.hpp"
#include "server/core/mmorpg/migration.hpp"
#include "server/core/mmorpg/topology.hpp"
#include "server/core/protocol/system_opcodes.hpp"
#include "server/core/state/world_lifecycle_policy.hpp"
#include "server/core/storage/redis/client.hpp"
#include "server/protocol/game_opcodes.hpp"
#include "server/chat/chat_hook_plugin_abi.hpp"
#include "server/scripting/chat_lua_bindings.hpp"
#include "wire.pb.h"

namespace server::core { class JobQueue; }
namespace server::core::scripting {
class LuaRuntime;
struct LuaHookContext;
}
namespace server::storage { class IRepositoryConnectionPool; }

namespace server::app::chat {

/**
 * @brief 채팅 서비스의 핵심 로직을 담당하는 클래스입니다.
 * 
 * 이 클래스는 서버의 "두뇌" 역할을 하며, 모든 비즈니스 로직을 처리합니다.
 * (Facade 패턴과 Mediator 패턴의 혼합 형태)
 * 
 * 주요 기능:
 * 1. 세션 및 유저 관리: 로그인, 로그아웃, 중복 접속 방지
 * 2. 채팅방 관리: 방 생성, 입장, 퇴장, 목록 조회
 * 3. 메시지 처리: 일반 채팅, 귓속말, 시스템 메시지
 * 4. 데이터 동기화: DB(PostgreSQL) 저장 및 Redis Pub/Sub을 통한 서버 간 통신
 * 
 * 설계 노트:
 * - 현재는 단일 클래스에서 모든 로직을 처리하지만, 규모가 커지면 `RoomManager`, `UserManager` 등으로 분리할 수 있습니다.
 * - 모든 상태 변경은 `job_queue_`를 통해 단일 스레드(또는 Strand)에서 순차적으로 처리되거나,
 *   내부 `mutex`를 통해 보호받아야 합니다.
 */
class ChatService : public server::app::scripting::ChatLuaHost {
public:
    using NetSession = server::core::net::Session;

    /**
     * @brief 채팅 서비스 생성자
     * @param io Boost.Asio IO 컨텍스트(비동기 작업용)
     * @param job_queue 작업 큐 (순차적 로직 처리용)
     * @param db_pool DB 연결 풀 (영구 저장소)
     * @param redis Redis 클라이언트 (캐시 및 Pub/Sub)
     */
    ChatService(boost::asio::io_context& io,
                server::core::JobQueue& job_queue,
                std::shared_ptr<server::storage::IRepositoryConnectionPool> db_pool = {},
                std::shared_ptr<server::core::storage::redis::IRedisClient> redis = {});

    /** @brief ChatService 리소스를 정리하고 플러그인/구독을 종료합니다. */
    ~ChatService();

    // ======================================================================
    // 패킷 핸들러
    // 디스패처가 호출하며, 각 opcode에 대응하는 로직을 수행합니다.
    // ======================================================================

    /**
     * @brief 로그인 요청 (MSG_LOGIN_REQ) 처리
     * - 토큰 검증 (현재는 단순 닉네임 기반)
     * - 중복 접속 처리 (기존 세션 끊기)
     * - 유저 상태 등록
     * @param s 요청을 보낸 세션
     * @param payload 로그인 요청 본문 바이트
     */
    void on_login(NetSession& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 방 입장 요청 (MSG_JOIN_ROOM) 처리
     * - 방 존재 여부 확인 및 생성
     * - 비밀번호 검사 (비공개 방)
     * - 방 멤버십 등록 및 브로드캐스팅
     * @param s 요청을 보낸 세션
     * @param payload 방 입장 요청 본문 바이트
     */
    void on_join(NetSession& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 방 퇴장 요청 (MSG_LEAVE_ROOM) 처리
     * @param s 요청을 보낸 세션
     * @param payload 방 퇴장 요청 본문 바이트
     */
    void on_leave(NetSession& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 채팅 메시지 전송 (MSG_CHAT_SEND) 처리
     * - 메시지 DB 저장 (비동기/Write-behind)
     * - 같은 방의 모든 유저에게 브로드캐스팅
     * @param s 요청을 보낸 세션
     * @param payload 채팅 전송 요청 본문 바이트
     */
    void on_chat_send(NetSession& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 귓속말 요청 (MSG_WHISPER_REQ) 처리
     * - 대상 유저 찾기 (로컬 또는 Redis를 통해 다른 서버 검색)
     * - 메시지 전송
     * @param s 요청을 보낸 세션
     * @param payload 귓속말 요청 본문 바이트
     */
    void on_whisper(NetSession& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 핑(Ping) 요청 (MSG_PING) 처리
     * - 연결 생존 여부를 확인하는 하트비트
     * - PONG 응답 전송
     * @param s 요청을 보낸 세션
     * @param payload 핑 요청 본문 바이트
     */
    void on_ping(NetSession& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 방 목록 요청 (MSG_ROOMS_REQ) 처리
     * - 현재 활성화된 방 목록 반환
     * @param s 요청을 보낸 세션
     * @param payload 방 목록 요청 본문 바이트
     */
    void on_rooms_request(NetSession& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 방 참여자 목록 요청 (MSG_ROOM_USERS_REQ) 처리
     * @param s 요청을 보낸 세션
     * @param payload 방 사용자 목록 요청 본문 바이트
     */
    void on_room_users_request(NetSession& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 상태 갱신 요청 (MSG_REFRESH_REQ) 처리
     * - 클라이언트가 재접속 후 놓친 메시지 등을 요청할 때 사용
     * @param s 요청을 보낸 세션
     * @param payload 상태 갱신 요청 본문 바이트
     */
    void on_refresh_request(NetSession& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 세션 종료 시 호출 (연결 끊김)
     * - 유저 상태 정리 (방 나가기 처리 등)
     * @param s 종료된 세션
     */
    void on_session_close(std::shared_ptr<NetSession> s);

    /**
     * @brief 특정 방에 메시지를 브로드캐스트합니다.
     * 
     * 로컬 세션뿐만 아니라, Redis Pub/Sub을 통해 수신된 메시지를
     * 로컬에 있는 해당 방 유저들에게 전달할 때도 사용됩니다.
     * 
     * @param room 방 이름
     * @param body 전송할 메시지 본문 (직렬화된 Protobuf 등)
     * @param self 발신자 세션 (자신에게는 다시 보내지 않기 위해 사용, nullptr이면 모두에게 전송)
     */
    void broadcast_room(const std::string& room, const std::vector<std::uint8_t>& body, NetSession* self = nullptr);

    /**
     * @brief 해당 방의 모든 유저에게 상태 갱신 알림(MSG_REFRESH_NOTIFY)을 전송합니다.
     * 로컬 세션에게 전송하고, Redis를 통해 다른 서버에도 전파합니다.
     * @param room 상태 갱신을 전파할 방 이름
     */
    void broadcast_refresh(const std::string& room);

    /**
     * @brief 해당 방의 로컬 세션에게만 상태 갱신 알림을 전송합니다.
     * Redis Subscriber에서 호출하거나, broadcast_refresh 내부에서 사용됩니다.
     * @param room 상태 갱신을 전송할 방 이름
     */
    void broadcast_refresh_local(const std::string& room);

    /**
     * @brief Redis Pub/Sub로 수신한 원격 귓속말 본문을 로컬 대상 세션에 전달합니다.
     * @param body 직렬화된 귓속말 이벤트 본문 바이트
     */
    void deliver_remote_whisper(const std::vector<std::uint8_t>& body);

    /**
     * @brief 관리자 제어면에서 지정한 사용자 세션들을 강제 종료합니다.
     *
     * @param users 종료 대상 사용자 식별자 목록
     * @param reason 종료 사유(클라이언트 공지용, 선택)
     */
    void admin_disconnect_users(const std::vector<std::string>& users, const std::string& reason);

    /**
     * @brief 관리자 공지를 현재 서버의 모든 로컬 사용자 세션에 전송합니다.
     *
     * @param text 공지 본문
     */
    void admin_broadcast_notice(const std::string& text);

    /**
     * @brief 런타임 변경 가능한 채팅 설정을 적용합니다.
     *
     * 지원 키:
     * - `presence_ttl_sec`
     * - `recent_history_limit`
     * - `room_recent_maxlen`
      * - `chat_spam_threshold`
      * - `chat_spam_window_sec`
      * - `chat_spam_mute_sec`
      * - `chat_spam_ban_sec`
      * - `chat_spam_ban_violations`
     * @param key 설정 키
     * @param value 설정 값(문자열)
     */
    void admin_apply_runtime_setting(const std::string& key, const std::string& value);

    /**
     * @brief 관리자 제재 명령(mute/unmute/ban/unban/kick)을 적용합니다.
     *
     * @param op 제재 연산자
     * @param users 대상 사용자 목록
     * @param duration_sec 기간(초, mute/ban에서만 사용)
     * @param reason 사유(선택)
     */
    void admin_apply_user_moderation(const std::string& op,
                                     const std::vector<std::string>& users,
                                     std::uint32_t duration_sec,
                                     const std::string& reason);

    std::optional<std::string> lua_get_user_name(std::uint32_t session_id) override;
    std::optional<std::string> lua_get_user_room(std::uint32_t session_id) override;
    std::vector<std::string> lua_get_room_users(std::string_view room_name) override;
    std::vector<std::string> lua_get_room_list() override;
    std::optional<std::string> lua_get_room_owner(std::string_view room_name) override;
    bool lua_is_user_muted(std::string_view nickname) override;
    bool lua_is_user_banned(std::string_view nickname) override;
    std::size_t lua_get_online_count() override;
    std::size_t lua_get_room_count() override;
    bool lua_send_notice(std::uint32_t session_id, std::string_view text) override;
    bool lua_broadcast_room(std::string_view room_name, std::string_view text) override;
    bool lua_broadcast_all(std::string_view text) override;
    bool lua_kick_user(std::uint32_t session_id, std::string_view reason) override;
    bool lua_mute_user(std::string_view nickname,
                       std::uint32_t duration_sec,
                       std::string_view reason) override;
    bool lua_ban_user(std::string_view nickname,
                      std::uint32_t duration_sec,
                      std::string_view reason) override;

    /** @brief 단일 채팅 훅 플러그인의 런타임 메트릭 스냅샷입니다. */
    struct ChatHookPluginMetric {
        /** @brief Hook 단위 호출/에러/지연 집계 메트릭입니다. */
        struct HookMetric {
            std::string hook_name;
            std::uint64_t calls_total{0};
            std::uint64_t errors_total{0};
            std::uint64_t duration_count{0};
            std::uint64_t duration_sum_ns{0};
            std::array<std::uint64_t, 12> duration_bucket_counts{};
        };

        std::string file;                        ///< 플러그인 파일 경로
        bool loaded{false};                      ///< 현재 로드 성공 여부
        std::string name;                        ///< 플러그인 이름
        std::string version;                     ///< 플러그인 버전 문자열
        std::uint64_t reload_attempt_total{0};  ///< reload 시도 누적 횟수
        std::uint64_t reload_success_total{0};  ///< reload 성공 누적 횟수
        std::uint64_t reload_failure_total{0};  ///< reload 실패 누적 횟수
        std::vector<HookMetric> hook_metrics;    ///< hook별 호출/에러 누적 횟수
    };

    /** @brief 채팅 훅 플러그인 체인의 집계 메트릭 스냅샷입니다. */
    struct ChatHookPluginsMetrics {
        bool enabled{false};                          ///< 플러그인 기능 활성화 여부
        std::string mode;                             ///< 로드 모드(`none|dir|paths|single`)
        std::vector<ChatHookPluginMetric> plugins;    ///< 플러그인별 메트릭 목록
    };

    /**
     * @brief 현재 플러그인 로더 상태를 운영 메트릭 스냅샷으로 반환합니다.
     * @return 플러그인 활성화/모드/개별 reload 메트릭 집계
     */
    ChatHookPluginsMetrics chat_hook_plugins_metrics() const;

    /** @brief Lua hook 단위 상태/에러 누적 메트릭입니다. */
    struct LuaHookMetric {
        std::string hook_name;
        bool disabled{false};
        std::uint64_t consecutive_failures{0};
        std::uint64_t auto_disable_total{0};
        std::uint64_t calls_total{0};
        std::uint64_t errors_total{0};
        std::uint64_t instruction_limit_hits{0};
        std::uint64_t memory_limit_hits{0};
    };

    /** @brief Lua hook+script 조합 호출 누적 메트릭입니다. */
    struct LuaScriptCallMetric {
        std::string hook_name;
        std::string script_name;
        std::uint64_t calls_total{0};
        std::uint64_t errors_total{0};
    };

    /** @brief Lua 런타임/훅 전체 집계 메트릭 스냅샷입니다. */
    struct LuaHooksMetrics {
        bool enabled{false};
        std::uint64_t auto_disable_threshold{0};
        std::uint64_t reload_epoch{0};
        std::size_t loaded_scripts{0};
        std::size_t memory_used_bytes{0};
        std::uint64_t calls_total{0};
        std::uint64_t errors_total{0};
        std::uint64_t instruction_limit_hits{0};
        std::uint64_t memory_limit_hits{0};
        std::vector<LuaHookMetric> hooks;
        std::vector<LuaScriptCallMetric> script_calls;
    };

    LuaHooksMetrics lua_hooks_metrics() const;

    /** @brief continuity lease/state handoff 집계 메트릭입니다. */
    struct ContinuityMetrics {
        std::uint64_t lease_issue_total{0};
        std::uint64_t lease_issue_fail_total{0};
        std::uint64_t lease_resume_total{0};
        std::uint64_t lease_resume_fail_total{0};
        std::uint64_t state_write_total{0};
        std::uint64_t state_write_fail_total{0};
        std::uint64_t state_restore_total{0};
        std::uint64_t state_restore_fallback_total{0};
        std::uint64_t world_write_total{0};
        std::uint64_t world_write_fail_total{0};
        std::uint64_t world_restore_total{0};
        std::uint64_t world_restore_fallback_total{0};
        std::uint64_t world_restore_fallback_missing_world_total{0};
        std::uint64_t world_restore_fallback_missing_owner_total{0};
        std::uint64_t world_restore_fallback_owner_mismatch_total{0};
        std::uint64_t world_restore_fallback_draining_replacement_unhonored_total{0};
        std::uint64_t world_owner_write_total{0};
        std::uint64_t world_owner_write_fail_total{0};
        std::uint64_t world_owner_restore_total{0};
        std::uint64_t world_owner_restore_fallback_total{0};
        std::uint64_t world_migration_restore_total{0};
        std::uint64_t world_migration_restore_fallback_total{0};
        std::uint64_t world_migration_restore_fallback_target_world_missing_total{0};
        std::uint64_t world_migration_restore_fallback_target_owner_missing_total{0};
        std::uint64_t world_migration_restore_fallback_target_owner_not_ready_total{0};
        std::uint64_t world_migration_restore_fallback_target_owner_mismatch_total{0};
        std::uint64_t world_migration_restore_fallback_source_not_draining_total{0};
        std::uint64_t world_migration_payload_room_handoff_total{0};
        std::uint64_t world_migration_payload_room_handoff_fallback_total{0};
    };

    ContinuityMetrics continuity_metrics() const;

private:
    using Session = NetSession;
    using WeakSession = std::weak_ptr<Session>;
    using WeakLess = std::owner_less<WeakSession>;
    using RoomSet = std::set<WeakSession, WeakLess>; // 세션들의 집합 (약한 참조로 저장하여 순환 참조 방지)

    using Exec = boost::asio::io_context::executor_type;
    using Strand = boost::asio::strand<Exec>; // 핸들러 동기화를 위한 스트랜드

    struct HookPluginState;

    // Write-behind(지연 쓰기) 설정
    /** @brief write-behind 동작 파라미터 집합입니다. */
    struct WriteBehindConfig {
        bool enabled{false};
        std::string stream_key{"session_events"};
        std::optional<std::size_t> maxlen{};
        bool approximate{true};
    };

    // Presence(접속 현황) 설정
    /** @brief presence 키 TTL/prefix 설정입니다. */
    struct PresenceConfig {
        unsigned int ttl{30}; // 초 단위
        std::string prefix;
    };

    /** @brief reconnect/resume용 logical session continuity 설정입니다. */
    struct ContinuityConfig {
        bool enabled{false};
        unsigned int lease_ttl_sec{15 * 60};
        std::string redis_prefix;
        std::string default_world_id;
        std::string current_owner_id;
        std::string topology_runtime_assignment_key;
    };

    // 서버의 전체 상태를 관리하는 구조체
    // 멀티스레드 환경에서 안전하게 접근하기 위해 mutex로 보호됩니다.
    /** @brief 세션/방/제재 상태를 보관하는 서버 메모리 상태 컨테이너입니다. */
    struct State {
        /** @brief 만료 시각을 가진 제재 상태(뮤트/밴) 엔트리입니다. */
        struct TimedPenalty {
            std::chrono::steady_clock::time_point expires_at{};
            std::string reason;
        };

        std::mutex mu;
        
        // 방 관리
        std::unordered_map<std::string, RoomSet> rooms;          // 방 이름 -> 참여 중인 세션 목록
        std::unordered_map<std::string, std::string> room_ids;   // 방 이름 -> 방 UUID
        std::unordered_map<std::string, std::string> room_passwords; // 방 이름 -> 비밀번호 해시
        std::unordered_map<std::string, std::string> room_owners; // 방 이름 -> 소유자 닉네임
        std::unordered_map<std::string, std::unordered_set<std::string>> room_invites; // 방 이름 -> 초대된 닉네임

        // 유저/세션 관리
        std::unordered_map<Session*, std::string> user;          // 세션 -> 닉네임
        std::unordered_map<Session*, std::string> user_uuid;     // 세션 -> 유저 UUID
        std::unordered_map<Session*, std::string> session_uuid;  // 세션 -> 세션 UUID
        std::unordered_map<Session*, std::string> logical_session_id; // 세션 -> logical continuity session ID
        std::unordered_map<Session*, std::uint64_t> logical_session_expires_unix_ms; // 세션 -> continuity lease 만료 시각
        std::unordered_map<Session*, std::string> cur_world;     // 세션 -> 현재 소속 world residency ID
        std::unordered_map<Session*, std::string> cur_room;      // 세션 -> 현재 참여 중인 방 이름
        std::unordered_map<Session*, std::string> session_ip;     // 세션 -> 최근 로그인 IP
        std::unordered_map<Session*, std::string> session_hwid_hash; // 세션 -> HWID 해시(로그인 토큰 기반)
        std::unordered_map<std::string, std::string> user_last_ip; // 유저 -> 최근 로그인 IP
        std::unordered_map<std::string, std::string> user_last_hwid_hash; // 유저 -> 최근 HWID 해시

        // 세션 집합
        std::unordered_set<Session*> authed;                     // 로그인한 세션 목록
        std::unordered_set<Session*> guest;                      // 로그인하지 않은 게스트 세션

        // 닉네임 역참조 (중복 로그인 방지 및 귓속말용)
        std::unordered_map<std::string, RoomSet> by_user;        // 닉네임 -> 세션 목록 (다중 접속 허용 시 여러 개일 수 있음)
        std::unordered_map<std::uint32_t, WeakSession> by_session_id; // 세션 ID -> 세션 weak 참조

        // 제재/스팸 관리
        std::unordered_map<std::string, TimedPenalty> muted_users; // 유저 -> 뮤트 만료/사유
        std::unordered_map<std::string, TimedPenalty> banned_users; // 유저 -> 밴 만료/사유
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> banned_ips; // IP -> 밴 만료
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> banned_hwid_hashes; // HWID 해시 -> 밴 만료
        std::unordered_map<std::string, std::deque<std::chrono::steady_clock::time_point>> spam_events; // 유저 -> 최근 메시지 시각
        std::unordered_map<std::string, std::uint32_t> spam_violations; // 유저 -> 누적 위반 횟수
        std::unordered_map<std::string, std::unordered_set<std::string>> user_blacklists; // 유저 -> 차단 대상 유저 집합
    } state_;

    boost::asio::io_context* io_{};
    server::core::JobQueue& job_queue_;
    std::shared_ptr<server::storage::IRepositoryConnectionPool> db_pool_{};
    std::shared_ptr<server::core::storage::redis::IRedisClient> redis_{};
    std::shared_ptr<server::core::scripting::LuaRuntime> lua_runtime_{};
    std::shared_ptr<Strand> lua_execution_strand_{};
    std::string gateway_id_{"gw-default"};
    bool redis_pubsub_enabled_{false};
    std::unordered_set<std::string> admin_users_{};
    std::size_t spam_message_threshold_{6};
    std::uint32_t spam_window_sec_{5};
    std::uint32_t spam_mute_sec_{30};
    std::uint32_t spam_ban_sec_{600};
    std::uint32_t spam_ban_violation_threshold_{3};
    std::uint64_t lua_auto_disable_threshold_{3};
    std::uint64_t lua_hook_warn_budget_us_{0};
    std::atomic<std::uint64_t> continuity_lease_issue_total_{0};
    std::atomic<std::uint64_t> continuity_lease_issue_fail_total_{0};
    std::atomic<std::uint64_t> continuity_lease_resume_total_{0};
    std::atomic<std::uint64_t> continuity_lease_resume_fail_total_{0};
    std::atomic<std::uint64_t> continuity_state_write_total_{0};
    std::atomic<std::uint64_t> continuity_state_write_fail_total_{0};
    std::atomic<std::uint64_t> continuity_state_restore_total_{0};
    std::atomic<std::uint64_t> continuity_state_restore_fallback_total_{0};
    std::atomic<std::uint64_t> continuity_world_write_total_{0};
    std::atomic<std::uint64_t> continuity_world_write_fail_total_{0};
    std::atomic<std::uint64_t> continuity_world_restore_total_{0};
    std::atomic<std::uint64_t> continuity_world_restore_fallback_total_{0};
    std::atomic<std::uint64_t> continuity_world_restore_fallback_missing_world_total_{0};
    std::atomic<std::uint64_t> continuity_world_restore_fallback_missing_owner_total_{0};
    std::atomic<std::uint64_t> continuity_world_restore_fallback_owner_mismatch_total_{0};
    std::atomic<std::uint64_t> continuity_world_restore_fallback_draining_replacement_unhonored_total_{0};
    std::atomic<std::uint64_t> continuity_world_owner_write_total_{0};
    std::atomic<std::uint64_t> continuity_world_owner_write_fail_total_{0};
    std::atomic<std::uint64_t> continuity_world_owner_restore_total_{0};
    std::atomic<std::uint64_t> continuity_world_owner_restore_fallback_total_{0};
    std::atomic<std::uint64_t> continuity_world_migration_restore_total_{0};
    std::atomic<std::uint64_t> continuity_world_migration_restore_fallback_total_{0};
    std::atomic<std::uint64_t> continuity_world_migration_restore_fallback_target_world_missing_total_{0};
    std::atomic<std::uint64_t> continuity_world_migration_restore_fallback_target_owner_missing_total_{0};
    std::atomic<std::uint64_t> continuity_world_migration_restore_fallback_target_owner_not_ready_total_{0};
    std::atomic<std::uint64_t> continuity_world_migration_restore_fallback_target_owner_mismatch_total_{0};
    std::atomic<std::uint64_t> continuity_world_migration_restore_fallback_source_not_draining_total_{0};
    std::atomic<std::uint64_t> continuity_world_migration_payload_room_handoff_total_{0};
    std::atomic<std::uint64_t> continuity_world_migration_payload_room_handoff_fallback_total_{0};
    mutable std::mutex lua_hook_metrics_mu_;
    std::unordered_map<std::string, std::uint64_t> lua_hook_consecutive_failures_;
    std::unordered_map<std::string, std::uint64_t> lua_hook_auto_disable_total_;
    std::unordered_map<std::string, std::uint64_t> lua_hook_calls_total_;
    std::unordered_map<std::string, std::uint64_t> lua_hook_errors_total_;
    std::unordered_map<std::string, std::uint64_t> lua_hook_instruction_limit_hits_;
    std::unordered_map<std::string, std::uint64_t> lua_hook_memory_limit_hits_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::uint64_t>> lua_hook_script_calls_total_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::uint64_t>> lua_hook_script_errors_total_;
    std::unordered_set<std::string> lua_hook_disabled_;
    std::uint64_t lua_reload_epoch_{0};

    std::unique_ptr<HookPluginState> hook_plugin_{};
    
    // 방별 스트랜드 관리(방 단위 메시지 순서 보장)
    std::unordered_map<std::string, std::shared_ptr<Strand>> room_strands_;
    Strand& strand_for(const std::string& room);

    WriteBehindConfig write_behind_;
    PresenceConfig presence_{};
    ContinuityConfig continuity_{};
    
    // 메시지 히스토리 캐싱 설정
    /** @brief 스냅샷/refresh 경로의 최근 메시지 캐시 파라미터입니다. */
    struct HistoryConfig {
        std::size_t recent_limit{20};
        std::size_t max_list_len{200};
        std::size_t fetch_factor{3};
        unsigned int cache_ttl_sec{6 * 60 * 60};
    } history_;

    // --- 내부 헬퍼 메서드 ---

    bool write_behind_enabled() const;
    bool pubsub_enabled();
    std::string generate_uuid_v4();
    std::string get_or_create_session_uuid(Session& s);
    
    // Write-behind 이벤트 발행 (Redis Stream에 추가)
    void emit_write_behind_event(const std::string& type,
                                 const std::string& session_id,
                                 const std::optional<std::string>& user_id,
                                 const std::optional<std::string>& room_id,
                                 std::vector<std::pair<std::string, std::string>> extra_fields = {});

    std::string ensure_unique_or_error(Session& s, const std::string& desired);
    std::string gen_temp_name_uuid8();
    void send_room_users(Session& s, const std::string& room);
    void send_rooms_list(Session& s);
    void send_snapshot(Session& s, const std::string& current);
    void dispatch_whisper(std::shared_ptr<Session> sender, const std::string& target_user, const std::string& text);
    void send_system_notice(Session& s, const std::string& text);
    std::string hash_room_password(const std::string& password);
    bool verify_room_password(const std::string& password, const std::string& stored_hash);
    bool is_modern_room_password_hash(const std::string& stored_hash) const;
    std::string hash_hwid_token(std::string_view token) const;
    void send_whisper_result(Session& s, bool ok, const std::string& reason);
    std::string ensure_room_id_ci(const std::string& room_name);

    /** @brief persisted logical session lease 한 건의 복원/발급 결과입니다. */
    struct ContinuityLease {
        std::string logical_session_id;
        std::string resume_token;
        std::string user_id;
        std::string effective_user;
        std::string world_id;
        std::string room;
        std::uint64_t expires_unix_ms{0};
        bool resumed{false};
    };

    /** @brief app-local migration payload에서 복원한 target room handoff 결과입니다. */
    struct AppMigrationRoomHandoff {
        bool recognized{false};
        std::string room;
    };
    
    // Redis 키 생성 헬퍼
    std::string make_recent_list_key(const std::string& room_id) const;
    std::string make_recent_message_key(std::uint64_t message_id) const;
    std::string make_continuity_room_key(const std::string& logical_session_id) const;
    std::string make_continuity_world_key(const std::string& logical_session_id) const;
    std::string make_continuity_world_owner_key(const std::string& world_id) const;
    std::string make_continuity_world_policy_key(const std::string& world_id) const;
    std::string make_continuity_world_migration_key(const std::string& world_id) const;
    bool continuity_enabled() const;
    std::optional<std::string> extract_resume_token(std::string_view token) const;
    std::optional<std::string> load_continuity_room(const std::string& logical_session_id);
    std::optional<std::string> load_continuity_world(const std::string& logical_session_id);
    std::optional<std::string> load_continuity_world_owner(const std::string& world_id);
    std::optional<server::core::state::WorldLifecyclePolicy> load_continuity_world_policy(const std::string& world_id);
    std::optional<server::core::mmorpg::WorldMigrationEnvelope> load_continuity_world_migration(const std::string& world_id);
    std::optional<server::core::mmorpg::TopologyActuationRuntimeAssignmentItem> load_topology_runtime_assignment() const;
    std::string current_runtime_default_world_id() const;
    AppMigrationRoomHandoff resolve_app_world_migration_room_handoff(
        const server::core::mmorpg::WorldMigrationEnvelope& migration) const;
    void persist_continuity_room(const std::string& logical_session_id,
                                 const std::string& room,
                                 std::uint64_t expires_unix_ms);
    void persist_continuity_world(const std::string& logical_session_id,
                                  const std::string& world_id,
                                  std::uint64_t expires_unix_ms);
    void persist_continuity_world_owner(const std::string& world_id,
                                        const std::string& owner_id,
                                        std::uint64_t expires_unix_ms);
    std::optional<ContinuityLease> try_resume_continuity_lease(std::string_view token);
    std::optional<ContinuityLease> issue_continuity_lease(const std::string& user_id,
                                                          const std::string& effective_user,
                                                          const std::string& world_id,
                                                          const std::string& room,
                                                          const std::optional<std::string>& client_ip);
    
    // 캐시 관리
    bool cache_recent_message(const std::string& room_id,
                              const server::wire::v1::StateSnapshot::SnapshotMessage& message);
    bool load_recent_messages_from_cache(const std::string& room_id,
                                         std::vector<server::wire::v1::StateSnapshot::SnapshotMessage>& out);
    void handle_refresh(std::shared_ptr<Session> session);

    /** @brief Lua cold-hook 호출 결과를 기본 경로 제어용으로 정규화한 값입니다. */
    struct LuaColdHookOutcome {
        bool stop_default{false};
        std::string deny_reason;
        std::vector<std::string> notices;
    };

    LuaColdHookOutcome invoke_lua_cold_hook(
        std::string_view hook_name,
        const server::core::scripting::LuaHookContext& context);

    // 플러그인이 메시지를 처리/차단했으면 true를 반환합니다(기본 로직은 중단).
    // 필요 시 텍스트를 변경(replace)하거나 시스템 공지를 전송할 수 있습니다.
    bool maybe_handle_chat_hook_plugin(Session& s,
                                       const std::string& room,
                                       const std::string& sender,
                                       std::string& text);

    bool maybe_handle_login_hook(Session& s, const std::string& user);
    bool maybe_handle_join_hook(Session& s, const std::string& user, const std::string& room);
    bool maybe_handle_leave_hook(Session& s, const std::string& user, const std::string& room);
    void notify_session_event_hook(std::uint32_t session_id,
                                   SessionEventKindV2 kind,
                                   const std::string& user,
                                   const std::string& reason);
    bool maybe_handle_admin_command_hook(std::string_view command,
                                         std::string_view issuer,
                                         std::string_view payload_json,
                                         std::string_view args,
                                         std::string& deny_reason);

    friend struct ChatServiceHistoryTester;
    friend struct ChatServiceContinuityTester;

    static void collect_room_sessions(RoomSet& set, std::vector<std::shared_ptr<Session>>& out);
    std::shared_ptr<Session> find_session_by_id_locked(std::uint32_t session_id);
    std::vector<std::uint8_t> make_system_chat_body(std::string_view room, std::string_view text) const;
    bool broadcast_notice_to_all_sessions(std::string notice);
    bool apply_user_moderation_without_hook(const std::string& op,
                                            const std::vector<std::string>& users,
                                            std::uint32_t duration_sec,
                                            const std::string& reason);
    unsigned int presence_ttl() const;
    std::string make_presence_key(std::string_view category, const std::string& id) const;
    void touch_user_presence(const std::string& uid);
};

} // namespace server::app::chat
