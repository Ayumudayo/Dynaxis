#pragma once

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

#include <boost/asio.hpp>

#include "server/core/net/session.hpp"
#include "server/core/protocol/system_opcodes.hpp"
#include "server/protocol/game_opcodes.hpp"
#include "wire.pb.h"

namespace server::core { class JobQueue; }
namespace server::core::storage { class IConnectionPool; }
namespace server::storage::redis { class IRedisClient; }

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
class ChatService {
public:
    /**
     * @brief ChatService 생성자
     * @param io Boost.Asio IO Context (비동기 작업용)
     * @param job_queue 작업 큐 (순차적 로직 처리용)
     * @param db_pool DB 연결 풀 (영구 저장소)
     * @param redis Redis 클라이언트 (캐시 및 Pub/Sub)
     */
    ChatService(boost::asio::io_context& io,
                server::core::JobQueue& job_queue,
                std::shared_ptr<server::core::storage::IConnectionPool> db_pool = {},
                std::shared_ptr<server::storage::redis::IRedisClient> redis = {});

    ~ChatService();

    // ======================================================================
    // 패킷 핸들러 (Packet Handlers)
    // Dispatcher에 의해 호출되며, 각 Opcode에 대응하는 로직을 수행합니다.
    // ======================================================================

    /**
     * @brief 로그인 요청 (MSG_LOGIN_REQ) 처리
     * - 토큰 검증 (현재는 단순 닉네임 기반)
     * - 중복 접속 처리 (기존 세션 끊기)
     * - 유저 상태 등록
     */
    void on_login(server::core::Session& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 방 입장 요청 (MSG_JOIN_ROOM) 처리
     * - 방 존재 여부 확인 및 생성
     * - 비밀번호 검사 (비공개 방)
     * - 방 멤버십 등록 및 브로드캐스팅
     */
    void on_join(server::core::Session& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 방 퇴장 요청 (MSG_LEAVE_ROOM) 처리
     */
    void on_leave(server::core::Session& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 채팅 메시지 전송 (MSG_CHAT_SEND) 처리
     * - 메시지 DB 저장 (비동기/Write-behind)
     * - 같은 방의 모든 유저에게 브로드캐스팅
     */
    void on_chat_send(server::core::Session& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 귓속말 요청 (MSG_WHISPER_REQ) 처리
     * - 대상 유저 찾기 (로컬 또는 Redis를 통해 다른 서버 검색)
     * - 메시지 전송
     */
    void on_whisper(server::core::Session& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 핑(Ping) 요청 (MSG_PING) 처리
     * - 연결이 살아있는지 확인하는 Heartbeat
     * - PONG 응답 전송
     */
    void on_ping(server::core::Session& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 방 목록 요청 (MSG_ROOMS_REQ) 처리
     * - 현재 활성화된 방 목록 반환
     */
    void on_rooms_request(server::core::Session& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 방 참여자 목록 요청 (MSG_ROOM_USERS_REQ) 처리
     */
    void on_room_users_request(server::core::Session& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 상태 갱신 요청 (MSG_REFRESH_REQ) 처리
     * - 클라이언트가 재접속 후 놓친 메시지 등을 요청할 때 사용
     */
    void on_refresh_request(server::core::Session& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 세션 종료 시 호출 (연결 끊김)
     * - 유저 상태 정리 (방 나가기 처리 등)
     */
    void on_session_close(std::shared_ptr<server::core::Session> s);

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
    void broadcast_room(const std::string& room, const std::vector<std::uint8_t>& body, server::core::Session* self = nullptr);

    /**
     * @brief 해당 방의 모든 유저에게 상태 갱신 알림(MSG_REFRESH_NOTIFY)을 전송합니다.
     * 로컬 세션에게 전송하고, Redis를 통해 다른 서버에도 전파합니다.
     */
    void broadcast_refresh(const std::string& room);

    /**
     * @brief 해당 방의 로컬 세션에게만 상태 갱신 알림을 전송합니다.
     * Redis Subscriber에서 호출하거나, broadcast_refresh 내부에서 사용됩니다.
     */
    void broadcast_refresh_local(const std::string& room);

    // Redis Pub/Sub로 전달된 원격 귓속말 payload를 로컬 대상 세션에게 전달합니다.
    void deliver_remote_whisper(const std::vector<std::uint8_t>& body);

    struct ChatHookPluginMetric {
        std::string file;
        bool loaded{false};
        std::string name;
        std::string version;
        std::uint64_t reload_attempt_total{0};
        std::uint64_t reload_success_total{0};
        std::uint64_t reload_failure_total{0};
    };

    struct ChatHookPluginsMetrics {
        bool enabled{false};
        std::string mode; // none|dir|paths|single
        std::vector<ChatHookPluginMetric> plugins;
    };

    ChatHookPluginsMetrics chat_hook_plugins_metrics() const;

private:
    using Session = server::core::Session;
    using WeakSession = std::weak_ptr<Session>;
    using WeakLess = std::owner_less<WeakSession>;
    using RoomSet = std::set<WeakSession, WeakLess>; // 세션들의 집합 (약한 참조로 저장하여 순환 참조 방지)

    using Exec = boost::asio::io_context::executor_type;
    using Strand = boost::asio::strand<Exec>; // 핸들러 동기화를 위한 Strand

    struct HookPluginState;

    // Write-behind (지연 쓰기) 설정
    struct WriteBehindConfig {
        bool enabled{false};
        std::string stream_key{"session_events"};
        std::optional<std::size_t> maxlen{};
        bool approximate{true};
    };

    // Presence (접속 현황) 설정
    struct PresenceConfig {
        unsigned int ttl{30}; // 초 단위
        std::string prefix;
    };

    // 서버의 전체 상태를 관리하는 구조체
    // 멀티스레드 환경에서 안전하게 접근하기 위해 mutex로 보호됩니다.
    struct State {
        std::mutex mu;
        
        // 방 관리
        std::unordered_map<std::string, RoomSet> rooms;          // 방 이름 -> 참여 중인 세션 목록
        std::unordered_map<std::string, std::string> room_ids;   // 방 이름 -> 방 UUID
        std::unordered_map<std::string, std::string> room_passwords; // 방 이름 -> 비밀번호 해시

        // 유저/세션 관리
        std::unordered_map<Session*, std::string> user;          // 세션 -> 닉네임
        std::unordered_map<Session*, std::string> user_uuid;     // 세션 -> 유저 UUID
        std::unordered_map<Session*, std::string> session_uuid;  // 세션 -> 세션 UUID
        std::unordered_map<Session*, std::string> cur_room;      // 세션 -> 현재 참여 중인 방 이름
        
        // 세션 집합
        std::unordered_set<Session*> authed;                     // 로그인한 세션 목록
        std::unordered_set<Session*> guest;                      // 로그인하지 않은 게스트 세션
        
        // 닉네임 역참조 (중복 로그인 방지 및 귓속말용)
        std::unordered_map<std::string, RoomSet> by_user;        // 닉네임 -> 세션 목록 (다중 접속 허용 시 여러 개일 수 있음)
    } state_;

    boost::asio::io_context* io_{};
    server::core::JobQueue& job_queue_;
    std::shared_ptr<server::core::storage::IConnectionPool> db_pool_{};
    std::shared_ptr<server::storage::redis::IRedisClient> redis_{};
    std::string gateway_id_{"gw-default"};

    std::unique_ptr<HookPluginState> hook_plugin_{};
    
    // 방별 Strand 관리 (방 단위로 메시지 순서를 보장하기 위함)
    std::unordered_map<std::string, std::shared_ptr<Strand>> room_strands_;
    Strand& strand_for(const std::string& room);

    WriteBehindConfig write_behind_;
    PresenceConfig presence_{};
    
    // 메시지 히스토리 캐싱 설정
    struct HistoryConfig {
        std::size_t recent_limit{20};
        std::size_t max_list_len{200};
        std::size_t fetch_factor{3};
        unsigned int cache_ttl_sec{6 * 60 * 60};
    } history_;

    // --- 내부 헬퍼 메서드 ---

    bool write_behind_enabled() const;
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
    void send_whisper_result(Session& s, bool ok, const std::string& reason);
    std::string ensure_room_id_ci(const std::string& room_name);
    
    // Redis 키 생성 헬퍼
    std::string make_recent_list_key(const std::string& room_id) const;
    std::string make_recent_message_key(std::uint64_t message_id) const;
    
    // 캐시 관리
    bool cache_recent_message(const std::string& room_id,
                              const server::wire::v1::StateSnapshot::SnapshotMessage& message);
    bool load_recent_messages_from_cache(const std::string& room_id,
                                         std::vector<server::wire::v1::StateSnapshot::SnapshotMessage>& out);
    void handle_refresh(std::shared_ptr<Session> session);

    // Returns true if plugin handled/blocked the message (i.e., default logic should stop).
    // May mutate text (replace) and/or send system notices.
    bool maybe_handle_chat_hook_plugin(Session& s,
                                       const std::string& room,
                                       const std::string& sender,
                                       std::string& text);

    friend struct ChatServiceHistoryTester;

    static void collect_room_sessions(RoomSet& set, std::vector<std::shared_ptr<Session>>& out);
    unsigned int presence_ttl() const;
    std::string make_presence_key(std::string_view category, const std::string& id) const;
    void touch_user_presence(const std::string& uid);
};

} // namespace server::app::chat
