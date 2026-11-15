#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <functional>

#include <boost/asio.hpp>

#include "server/core/net/session.hpp"
#include "server/chat/chat_metrics.hpp"


namespace server::core { class JobQueue; }
namespace server::core::storage::redis { class IRedisClient; }
namespace server::storage { class IRepositoryConnectionPool; }

namespace server::app::chat {

struct ChatServicePrivateAccess;

/**
 * @brief `server_app`의 채팅 런타임을 한곳에서 조율하는 앱-로컬 서비스입니다.
 *
 * 이 타입은 단순히 opcode별 함수를 모아 둔 클래스가 아니라, 세션 상태, 방 상태,
 * Redis 분산 전파(fan-out), write-behind 적재, 채팅 정책을 한 요청 수명주기 안에서
 * 묶어 주는 조정층(orchestration layer)입니다.
 *
 * 왜 한곳에 모으는가:
 * - 로그인, 방 입장, 귓속말, 제재는 모두 "메모리 상태 + 외부 저장소 + 네트워크 응답"을 함께 다룹니다.
 *   이 규칙이 여러 객체에 흩어지면 동일 요청 안에서 상태 순서가 어긋나기 쉽습니다.
 * - 예를 들어 방 입장과 분산 전파(fan-out)가 분리되어 있으면 로컬 메모리는 이미 입장 처리됐는데,
 *   원격 서버에는 아직 반영되지 않은 반쯤 완료된 상태를 만들 수 있습니다.
 * - 반대로 모든 것을 `server_core`로 끌어올리면 채팅 전용 개념(방, 초대, 제재, 최근 메시지, 운영 명령)
 *   이 공용 엔진 표면을 오염시켜 다른 앱이 재사용하기 어려워집니다.
 *
 * 유지보수 관점에서 좋은 이유:
 * - 패킷 진입점은 좁게 유지하고, 실제 조정 규칙은 이 서비스 한곳에서 바꾸면 됩니다.
 * - 장애 분석 시 "이 요청이 어떤 상태를 언제 바꿨는가"를 한 타입 주변에서 추적할 수 있습니다.
 * - 추후 `RoomManager`, `ModerationManager`처럼 내부 하위 구성요소로 분해하더라도,
 *   외부 진입 계약은 유지한 채 내부 구조만 점진적으로 정리할 수 있습니다.
 *
 * 동시성 규칙:
 * - 명령과 close는 세션별 FIFO에서 실행하고 다른 세션은 worker pool에서 병렬 처리합니다.
 * - 로컬 컨테이너는 mutex, 공유 권한과 메시지 commit 순서는 DB 트랜잭션 잠금으로 보호합니다.
 * - 이 규칙을 깨면 중복 로그인 정리, 방 멤버십, 스팸 제재, refresh 스냅샷이 서로 다른 시점을 보게 됩니다.
 */
class ChatService {
public:
    using NetSession = server::core::net::Session;

    /** @brief 실제 앱은 명시적 의존성으로 구성하며 null을 전역 Registry로 대체하지 않습니다. */
    struct Dependencies {
        std::shared_ptr<server::storage::IRepositoryConnectionPool> db_pool;
        std::shared_ptr<server::core::storage::redis::IRedisClient> redis;
        bool require_auth{false};
        std::string auth_signing_secret;
    };

    /**
     * @brief 의존성을 명시해 다른 runtime의 전역 서비스와 격리합니다. 종료 전 worker를 drain해야 합니다.
     * @param io 앱 I/O 실행 문맥
     * @param job_queue 이 서비스의 작업 큐
     * @param dependencies 명시적 의존성과 인증 설정
     */
    ChatService(boost::asio::io_context& io, server::core::JobQueue& job_queue, Dependencies dependencies);

    /**
     * @brief 채팅 서비스가 의존하는 런타임 자원을 묶어 초기화합니다.
     *
     * 생성 시점에 IO, 순차 실행 큐, 저장소, Redis 연결을 주입해 두는 이유는
     * 요청 처리 중 전역 싱글턴을 뒤늦게 찾지 않게 만들기 위해서입니다.
     * 그렇게 해야 테스트에서 대체 구현을 넣기 쉽고, 장애가 났을 때도
     * 어떤 외부 의존성이 비어 있었는지를 부트스트랩 단계에서 곧바로 확인할 수 있습니다.
     *
     * @param io Boost.Asio IO 컨텍스트(비동기 후속 작업 예약용)
     * @param job_queue 앱 로직의 순서 보장을 맡는 작업 큐
     * @param db_pool 영구 저장소 접근용 저장소 연결 풀
     * @param redis 분산 전파(fan-out), 프레즌스, continuity, write-behind에 사용하는 Redis 클라이언트
     */
    ChatService(boost::asio::io_context& io,
                server::core::JobQueue& job_queue,
                std::shared_ptr<server::storage::IRepositoryConnectionPool> db_pool = {},
                std::shared_ptr<server::core::storage::redis::IRedisClient> redis = {});

    /** @brief 채팅 상태를 해제합니다. 호출 전에 worker와 구독을 종료해야 합니다. */
    ~ChatService();

    /** @brief 공유 권한 변경 알림 뒤 로컬 세션을 재검증합니다. 알림 유실 시에도 다음 명령에서 검증합니다. */
    void enforce_shared_state();

    // ======================================================================
    // 패킷 핸들러
    // Dispatcher는 "어느 opcode인가"까지만 해석하고,
    // 그 이후의 실제 상태 전이 규칙은 여기에서 수행합니다.
    // 이렇게 분리해야 transport/opcode 정책과 채팅 비즈니스 규칙이 서로 얽히지 않습니다.
    // ======================================================================

    /**
     * @brief 로그인 요청(`MSG_LOGIN_REQ`)의 인증/세션 정착을 처리합니다.
     *
     * 로그인은 단순 인증 확인이 아니라, 기존 세션 정리, 현재 세션의 사용자 식별,
     * presence 기록, continuity 복원 후보 계산까지 함께 묶이는 시작점입니다.
     * 이 과정을 여러 단계로 흩어 놓으면 "중복 접속은 끊겼는데 새 세션은 아직 준비되지 않은"
     * 중간 상태가 생겨 운영 중 유령 세션이나 잘못된 재접속 판정을 만들 수 있습니다.
     *
     * @param s 요청을 보낸 세션
     * @param payload 로그인 요청 본문 바이트
     */
    void on_login(NetSession& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 방 입장 요청(`MSG_JOIN_ROOM`)을 처리합니다.
     *
     * 방 입장은 방 존재/생성, 권한 확인, 멤버십 갱신, snapshot/fanout 준비가 한 묶음입니다.
     * 이 순서를 잘못 잡으면 아직 방 상태가 확정되지 않았는데 입장 브로드캐스트가 먼저 나가거나,
     * 반대로 로컬 상태만 바뀌고 다른 런타임에는 입장 사실이 퍼지지 않는 문제가 생깁니다.
     *
     * @param s 요청을 보낸 세션
     * @param payload 방 입장 요청 본문 바이트
     */
    void on_join(NetSession& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 방 퇴장 요청(`MSG_LEAVE_ROOM`)을 처리합니다.
     *
     * 퇴장은 단순 제거가 아니라 멤버십 정리, 빈 방 후처리, 상태 갱신 알림의 기준점입니다.
     * 이 처리가 입장 경로와 다른 규칙을 쓰면 refresh 결과와 실제 멤버 목록이 갈라질 수 있습니다.
     *
     * @param s 요청을 보낸 세션
     * @param payload 방 퇴장 요청 본문 바이트
     */
    void on_leave(NetSession& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 채팅 전송 요청(`MSG_CHAT_SEND`)을 처리합니다.
     *
     * 이 경로는 스팸 제어, write-behind 적재, 방 단위 순서 보장을 함께 다룹니다.
     * 메시지 저장과 브로드캐스트를 완전히 분리해 버리면 "방에는 보였지만 기록은 누락된 메시지" 또는
     * "저장은 됐지만 사용자에게는 순서가 뒤틀려 보이는 메시지"가 생기므로 한 흐름에서 조율해야 합니다.
     *
     * @param s 요청을 보낸 세션
     * @param payload 채팅 전송 요청 본문 바이트
     */
    void on_chat_send(NetSession& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 귓속말 요청(`MSG_WHISPER_REQ`)을 처리합니다.
     *
     * 귓속말은 로컬 세션 찾기만으로 끝나지 않고, 다른 서버에 붙어 있는 대상까지 고려해야 합니다.
     * 이 경로를 room fanout과 같은 방식으로 처리하면 잘못된 범위에 메시지가 노출될 수 있으므로,
     * 별도 전달 규칙과 결과 응답을 유지합니다.
     *
     * @param s 요청을 보낸 세션
     * @param payload 귓속말 요청 본문 바이트
     */
    void on_whisper(NetSession& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 핑(`MSG_PING`)을 처리합니다.
     *
     * 이 경로는 기능적으로 단순하지만, 연결 생존성과 지연 측정을 위한 가장 싼 확인 수단입니다.
     * 핑 처리까지 무거운 상태 락이나 저장소 접근을 끌어들이면 살아 있는 연결도 불필요하게 느려집니다.
     *
     * @param s 요청을 보낸 세션
     * @param payload 핑 요청 본문 바이트
     */
    void on_ping(NetSession& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 방 목록 요청(`MSG_ROOMS_REQ`)을 처리합니다.
     *
     * 목록 조회는 쓰기 경로보다 가벼워야 하지만, 동시에 스냅샷 일관성도 유지해야 합니다.
     * 부분적으로 잠긴 자료구조를 여기저기서 읽으면 동일 응답 안에서도 방 상태가 흔들릴 수 있습니다.
     *
     * @param s 요청을 보낸 세션
     * @param payload 방 목록 요청 본문 바이트
     */
    void on_rooms_request(NetSession& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 방 참여자 목록 요청(`MSG_ROOM_USERS_REQ`)을 처리합니다.
     *
     * 참여자 목록은 UI 편의 기능처럼 보이지만, 실제로는 중복 세션 정리와 refresh 검증에도 쓰입니다.
     * 그래서 입장/퇴장 경로와 다른 기준으로 계산하면 클라이언트가 잘못된 방 상태를 보게 됩니다.
     *
     * @param s 요청을 보낸 세션
     * @param payload 방 사용자 목록 요청 본문 바이트
     */
    void on_room_users_request(NetSession& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 상태 갱신 요청(`MSG_REFRESH_REQ`)을 처리합니다.
     *
     * refresh는 "현재 상태를 다시 받는다"는 의미이지만, 실제로는 최근 메시지 캐시와
     * 현재 멤버십, continuity 복원 결과를 같은 기준 시점으로 합쳐야 합니다.
     * 이 경로가 느슨하면 재접속 직후 방은 맞는데 최근 메시지가 비어 있거나,
     * 반대로 오래된 room residency가 되살아나는 문제가 생깁니다.
     *
     * @param s 요청을 보낸 세션
     * @param payload 상태 갱신 요청 본문 바이트
     */
    void on_refresh_request(NetSession& s, std::span<const std::uint8_t> payload);

    /**
     * @brief 세션 종료 시 후처리를 수행합니다.
     *
     * 연결 종료는 네트워크 계층 사건이지만, 여기서는 방 멤버십, presence, continuity lease,
     * 중복 로그인 역참조 같은 앱 상태 정리가 뒤따릅니다. 이 정리를 빼먹으면
     * "끊긴 사용자가 여전히 방에 남아 있는 것처럼 보이는" 유령 상태가 쌓입니다.
     *
     * @param s 종료된 세션
     */
    void on_session_close(std::shared_ptr<NetSession> s);

    /**
     * @brief 특정 방에 메시지를 전파합니다.
     *
     * 같은 API를 로컬 송신과 원격 fanout 수신 양쪽에서 함께 쓰는 이유는,
     * 최종 전달 규칙을 한곳으로 모아 중복 전송/자기 에코(self echo)/순서 불일치를 막기 위해서입니다.
     * 서로 다른 경로가 각자 브로드캐스트를 구현하면 운영 중 "로컬 발신만 한 번 더 보이는" 류의
     * 미묘한 버그가 생기기 쉽습니다.
     *
     * @param room 방 이름
     * @param body 전송할 메시지 본문 (직렬화된 Protobuf 등)
     * @param self 발신자 세션 (자신에게는 다시 보내지 않기 위해 사용, nullptr이면 모두에게 전송)
     */
    void broadcast_room(const std::string& room, const std::vector<std::uint8_t>& body, NetSession* self = nullptr);

    /**
     * @brief 방 단위 refresh 알림을 로컬과 원격 런타임에 함께 전파합니다.
     *
     * refresh 알림은 데이터 본문보다 "스냅샷을 다시 읽어야 한다"는 트리거 역할이 큽니다.
     * 로컬 전송과 원격 fanout을 따로 처리하면 어느 한쪽이 누락됐을 때
     * 일부 런타임만 오래된 화면을 유지하는 분할 상태가 생깁니다.
     *
     * @param room 상태 갱신을 전파할 방 이름
     */
    void broadcast_refresh(const std::string& room);

    /**
     * @brief 로컬 세션에만 refresh 알림을 보냅니다.
     *
     * 이 함수는 원격 전파와 로컬 전달을 분리하기 위한 내부 경계입니다.
     * Redis subscriber가 다시 Redis 전파까지 호출해 버리면 fanout loop가 생길 수 있으므로
     * 로컬 전용 경로를 명시적으로 유지합니다.
     *
     * @param room 상태 갱신을 전송할 방 이름
     */
    void broadcast_refresh_local(const std::string& room);

    /**
     * @brief 원격 런타임에서 넘어온 귓속말을 로컬 대상 세션으로 전달합니다.
     *
     * 원격 귓속말은 "다시 라우팅"이 아니라 이미 결정된 전달 결과를 소비하는 단계입니다.
     * 여기서 대상 탐색이나 재판정을 다시 해 버리면 중복 전달이나 잘못된 거부 응답이 생길 수 있습니다.
     *
     * @param body 직렬화된 귓속말 이벤트 본문 바이트
     */
    void deliver_remote_whisper(const std::vector<std::uint8_t>& body);

    ContinuityMetrics continuity_metrics() const;

private:
    // 세션의 명령과 close를 같은 FIFO에서 실행한다. closing은 정리 작업을 거절하지 않는다.
    bool enqueue_session(std::shared_ptr<NetSession> session, std::function<void()> job, bool closing = false);
    bool claim_shared_login(NetSession& session, const std::string& user, const std::string& resume_logical_id);
    bool renew_shared_login(NetSession& session, const std::string& logical_id = {});
    bool release_shared_login(NetSession& session);
    bool admit_shared_room(NetSession& session, std::string& room, const std::string& password, bool resume = false);
    bool handle_shared_room_command(std::shared_ptr<NetSession> session, const std::string& room, const std::string& text);
    void persist_shared_penalty(const std::string& user, bool ban, std::uint32_t seconds);
    void persist_shared_blacklist(const std::string& user);
    void deliver_verified(std::shared_ptr<NetSession> target, std::uint16_t opcode, std::vector<std::uint8_t> body,
                          std::string sender, std::string room = {});
    void publish_control_change();
    using Session = NetSession;

    /** @brief 채팅 런타임의 config/state/dispatch 세부 배치를 숨기는 단일 비공개 구현 상태입니다. */
    struct Impl;

    std::unique_ptr<Impl> impl_;

    boost::asio::io_context* io_{};
    server::core::JobQueue& job_queue_;

    // --- 내부 헬퍼 메서드 ---
    // public handler는 외부 계약을 유지하고, 실제 세부 단계는 아래 helper로 분리합니다.
    // 이렇게 해야 Doxygen 표면이 과도하게 커지지 않으면서도 구현 순서를 읽기 쉽게 유지할 수 있습니다.

    bool write_behind_enabled() const;
    bool pubsub_enabled();
    std::string generate_uuid_v4();
    std::string get_or_create_session_uuid(Session& s);

    // write-behind 적재를 동기 DB 쓰기와 분리하면 채팅 경로 tail latency를 줄일 수 있습니다.
    // 반대로 이 경로를 아무 조건 없이 호출하면 Redis 장애가 실시간 채팅까지 막을 수 있으므로
    // 별도 enabled/stream 정책을 함께 관리합니다.
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
    bool try_handle_slash_command(std::shared_ptr<Session> session,
                                  const std::string& current_room,
                                  const std::string& text);
    void dispatch_room_message(std::shared_ptr<Session> session,
                               const std::string& current_room,
                               const std::string& sender,
                               const std::string& text);

    /** @brief persisted logical session lease 한 건의 복원/발급 결과입니다. */
    struct ContinuityLease {
        std::string logical_session_id;
        std::string resume_token;
        std::string user_id;
        std::string effective_user;
        std::string room;
        std::uint64_t expires_unix_ms{0};
        bool resumed{false};
    };

    // Redis 키 생성 규칙을 흩어 놓으면 읽기/쓰기 경로가 다른 네이밍을 써서 복원이 깨지기 쉽습니다.
    // 키 구성은 helper로 고정해 continuity와 캐시 스키마를 한곳에서 관리합니다.
    std::string make_continuity_room_key(const std::string& logical_session_id) const;
    bool continuity_enabled() const;
    std::optional<std::string> extract_resume_token(std::string_view token) const;
    std::optional<std::string> load_continuity_room(const std::string& logical_session_id);
    void persist_continuity_room(const std::string& logical_session_id,
                                 const std::string& room,
                                 std::uint64_t expires_unix_ms);
    std::optional<ContinuityLease> try_resume_continuity_lease(std::string_view token);
    std::optional<ContinuityLease> issue_continuity_lease(const std::string& user_id,
                                                          const std::string& effective_user,
                                                          const std::string& room,
                                                          const std::optional<std::string>& client_ip);

    // refresh에서 저장소를 매번 다시 읽으면 재접속 폭주 시 Redis/DB가 먼저 흔들립니다.
    // 최근 메시지 캐시는 스냅샷 비용을 낮추되, TTL/길이를 제한해 메모리와 정합성 균형을 맞춥니다.
    void handle_refresh(std::shared_ptr<Session> session);

    friend struct ChatServicePrivateAccess;

    unsigned int presence_ttl() const;
    std::string make_presence_key(std::string_view category, const std::string& id) const;
    void touch_user_presence(const std::string& uid);
};

} // namespace server::app::chat
