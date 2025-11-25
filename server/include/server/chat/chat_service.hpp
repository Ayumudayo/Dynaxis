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

#include <boost/asio.hpp>

#include "server/core/net/session.hpp"
#include "server/core/protocol/opcodes.hpp"
#include "wire.pb.h"

namespace server::core { class JobQueue; }
namespace server::core::storage { class IConnectionPool; }
namespace server::storage::redis { class IRedisClient; }

namespace server::app::chat {

/**
 * @brief 채팅 서비스의 핵심 로직을 담당하는 클래스입니다.
 * 
 * 이 클래스는 다음과 같은 기능을 수행합니다:
 * - 사용자 로그인 및 세션 관리
 * - 채팅방 생성, 입장, 퇴장 및 메시지 전송
 * - 귓속말(1:1 채팅) 기능
 * - Redis Pub/Sub을 이용한 분산 환경에서의 메시지 브로드캐스팅
 * - DB 및 Redis 캐시를 이용한 메시지 히스토리 관리 (Write-behind 패턴 지원)
 * - 사용자 접속 상태(Presence) 관리
 */
class ChatService {
public:
    /**
     * @brief ChatService 생성자
     * @param io Boost.Asio IO Context
     * @param job_queue 작업 큐 (비동기 작업 처리용)
     * @param db_pool DB 연결 풀 (선택적)
     * @param redis Redis 클라이언트 (선택적)
     */
    ChatService(boost::asio::io_context& io,
                server::core::JobQueue& job_queue,
                std::shared_ptr<server::core::storage::IConnectionPool> db_pool = {},
                std::shared_ptr<server::storage::redis::IRedisClient> redis = {});

    // --- 핸들러 메서드 (패킷 처리) ---

    /**
     * @brief 로그인 요청을 처리합니다.
     * 닉네임 중복 검사 및 세션 인증 처리를 수행합니다.
     */
    void on_login(server::core::Session& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 방 입장 요청을 처리합니다.
     * 비밀번호 확인, 방 생성(필요 시), 입장 메시지 브로드캐스팅을 수행합니다.
     */
    void on_join(server::core::Session& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 방 퇴장 요청을 처리합니다.
     */
    void on_leave(server::core::Session& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 채팅 메시지 전송 요청을 처리합니다.
     * 메시지를 DB/Redis에 저장하고 방 내의 다른 사용자들에게 브로드캐스팅합니다.
     */
    void on_chat_send(server::core::Session& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 귓속말 요청을 처리합니다.
     */
    void on_whisper(server::core::Session& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 핑(Ping) 요청을 처리합니다. (Heartbeat)
     */
    void on_ping(server::core::Session& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 방 목록 요청을 처리합니다.
     */
    void on_rooms_request(server::core::Session& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 방 참여자 목록 요청을 처리합니다.
     */
    void on_room_users_request(server::core::Session& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 상태 갱신 요청을 처리합니다.
     */
    void on_refresh_request(server::core::Session& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 세션 종료 시 호출되어 정리 작업을 수행합니다.
     */
    void on_session_close(std::shared_ptr<server::core::Session> s);

    /**
     * @brief 특정 방에 메시지를 브로드캐스트합니다.
     * 외부(예: Redis Pub/Sub 핸들러)에서 호출하여 로컬 세션들에게 메시지를 전달할 때 사용합니다.
     * @param room 방 이름
     * @param body 전송할 메시지 본문
     * @param self 발신자 세션 (자신에게는 전송하지 않으려면 설정)
     */
    void broadcast_room(const std::string& room, const std::vector<std::uint8_t>& body, server::core::Session* self = nullptr);

private:
    using Session = server::core::Session;
    using WeakSession = std::weak_ptr<Session>;
    using WeakLess = std::owner_less<WeakSession>;
    using RoomSet = std::set<WeakSession, WeakLess>;

    using Exec = boost::asio::io_context::executor_type;
    using Strand = boost::asio::strand<Exec>;

    // Write-behind 설정 구조체
    struct WriteBehindConfig {
        bool enabled{false};
        std::string stream_key{"session_events"};
        std::optional<std::size_t> maxlen{};
        bool approximate{true};
    };

    // Presence 설정 구조체
    struct PresenceConfig {
        unsigned int ttl{30};
        std::string prefix;
    };

    // 서버 상태 구조체 (Mutex로 보호됨)
    struct State {
        std::mutex mu;
        std::unordered_map<std::string, RoomSet> rooms;          // 방 이름 -> 세션 목록
        std::unordered_map<Session*, std::string> user;          // 세션 -> 닉네임
        std::unordered_map<Session*, std::string> user_uuid;     // 세션 -> 사용자 UUID
        std::unordered_map<Session*, std::string> session_uuid;  // 세션 -> 세션 UUID (v4)
        std::unordered_map<Session*, std::string> cur_room;      // 세션 -> 현재 방 이름
        std::unordered_set<Session*> authed;                     // 인증된 세션 목록
        std::unordered_set<Session*> guest;                      // 게스트 세션 목록
        std::unordered_map<std::string, RoomSet> by_user;        // 닉네임 -> 세션 목록 (다중 접속 지원)
        std::unordered_map<std::string, std::string> room_ids;   // 방 이름 -> Room ID (UUID)
        std::unordered_map<std::string, std::string> room_passwords; // 방 이름 -> 비밀번호 해시
    } state_;

    boost::asio::io_context* io_{};
    server::core::JobQueue& job_queue_;
    std::shared_ptr<server::core::storage::IConnectionPool> db_pool_{};
    std::shared_ptr<server::storage::redis::IRedisClient> redis_{};
    std::string gateway_id_{"gw-default"};
    
    // 방별 Strand 관리 (동시성 제어)
    std::unordered_map<std::string, std::shared_ptr<Strand>> room_strands_;
    Strand& strand_for(const std::string& room);

    WriteBehindConfig write_behind_;
    PresenceConfig presence_{};
    
    // 히스토리 설정 구조체
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
    
    // Write-behind 이벤트 발행
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

    friend struct ChatServiceHistoryTester;

    static void collect_room_sessions(RoomSet& set, std::vector<std::shared_ptr<Session>>& out);
    unsigned int presence_ttl() const;
    std::string make_presence_key(std::string_view category, const std::string& id) const;
    void touch_user_presence(const std::string& uid);
};

} // namespace server::app::chat
