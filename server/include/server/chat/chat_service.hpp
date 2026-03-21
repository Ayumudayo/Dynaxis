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
#include "server/core/worlds/migration.hpp"
#include "server/core/worlds/topology.hpp"
#include "server/core/protocol/system_opcodes.hpp"
#include "server/core/discovery/world_lifecycle_policy.hpp"
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
 * @brief `server_app`의 채팅 런타임을 한곳에서 조율하는 앱-로컬 서비스입니다.
 *
 * 이 타입은 단순히 opcode별 함수를 모아 둔 클래스가 아니라, 세션 상태, 방 상태,
 * Redis 분산 전파(fan-out), write-behind 적재, 관리자 제어, Lua/플러그인 훅을 한 요청 수명주기 안에서
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
 * - 상태 변경은 `job_queue_` 또는 room별 `Strand`를 통해 순서를 보장하거나,
 *   공용 상태 컨테이너의 `mutex`로 보호해야 합니다.
 * - 이 규칙을 깨면 중복 로그인 정리, 방 멤버십, 스팸 제재, refresh 스냅샷이 서로 다른 시점을 보게 됩니다.
 */
class ChatService : public server::app::scripting::ChatLuaHost {
public:
    using NetSession = server::core::net::Session;

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

    /** @brief 구독, 플러그인, 보조 런타임을 정리해 종료 시 잔여 비동기 작업이 남지 않게 합니다. */
    ~ChatService();

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
     * 이 경로는 스팸 제어, 훅 체인, write-behind 적재, 방 단위 순서 보장을 함께 다룹니다.
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

    /**
     * @brief 관리자 제어면이 지정한 사용자 세션을 강제 종료합니다.
     *
     * 제어면 경로를 일반 채팅 명령과 분리해 두어야 권한 모델과 감사 로그 관점을 명확히 유지할 수 있습니다.
     * 그렇지 않으면 운영 명령이 일반 사용자 흐름과 같은 검증 경로를 타면서 우회 또는 누락이 생길 수 있습니다.
     *
     * @param users 종료 대상 사용자 식별자 목록
     * @param reason 종료 사유(클라이언트 공지용, 선택)
     */
    void admin_disconnect_users(const std::vector<std::string>& users, const std::string& reason);

    /**
     * @brief 관리자 공지를 현재 런타임의 로컬 세션들에 보냅니다.
     *
     * 공지는 일반 채팅 메시지와 다르게 room membership 없이 전송되어야 하므로
     * 별도 진입점을 유지합니다. 이를 일반 브로드캐스트로 섞으면 차단/방 범위 규칙과 충돌합니다.
     *
     * @param text 공지 본문
     */
    void admin_broadcast_notice(const std::string& text);

    /**
     * @brief 런타임 변경 가능한 채팅 설정을 적용합니다.
     *
     * 이 진입점은 운영 중 값을 빠르게 조정하되, 코드 재배포 없이 스팸/캐시/presence 거동을
     * 다듬을 수 있게 하기 위한 제어점입니다. 설정 변경 경로를 여기저기 흩어 놓으면
     * 어떤 값이 즉시 반영되고 어떤 값이 재시작 후 반영되는지 구분하기 어려워집니다.
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
     * @brief 관리자 제재 명령(`mute/unmute/ban/unban/kick`)을 적용합니다.
     *
     * 제재는 메모리 상태, 현재 연결, 후속 알림이 함께 묶이는 행위입니다.
     * 단순 플래그 저장으로 끝내면 이미 붙어 있는 세션에는 적용되지 않거나,
     * 반대로 연결만 끊기고 제재 상태는 남지 않는 반쪽 처리로 끝날 수 있습니다.
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

    /** @brief 단일 채팅 훅(hook) 플러그인 한 개의 상태와 재로드 결과를 담는 운영 스냅샷입니다. */
    struct ChatHookPluginMetric {
        /** @brief 훅(hook)별 호출 수, 오류 수, 지연 분포를 담는 세부 집계입니다. */
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

    /** @brief 플러그인 체인 전체를 한 번에 관찰하기 위한 집계 스냅샷입니다. */
    struct ChatHookPluginsMetrics {
        bool enabled{false};                          ///< 플러그인 기능 활성화 여부
        std::string mode;                             ///< 로드 모드(`none|dir|paths|single`)
        std::vector<ChatHookPluginMetric> plugins;    ///< 플러그인별 메트릭 목록
    };

    /**
     * @brief 현재 플러그인 로더 상태를 읽기 전용 스냅샷으로 반환합니다.
     * @return 플러그인 활성화 여부, 로드 모드, 개별 재로드 집계가 담긴 값 객체
     */
    ChatHookPluginsMetrics chat_hook_plugins_metrics() const;

    /** @brief Lua 훅(hook) 하나가 얼마나 자주 실패하거나 자동 비활성화됐는지 보여 주는 집계입니다. */
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

    /** @brief 특정 훅(hook)과 특정 스크립트 조합의 누적 호출 결과를 담는 집계입니다. */
    struct LuaScriptCallMetric {
        std::string hook_name;
        std::string script_name;
        std::uint64_t calls_total{0};
        std::uint64_t errors_total{0};
    };

    /** @brief Lua 런타임 전체 상태를 제어면과 메트릭 경로에서 읽기 쉽게 펼친 스냅샷입니다. */
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

    /** @brief continuity 임대(lease), world 복구, migration handoff의 성공/실패 누계를 묶은 집계입니다. */
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
    // 방은 세션을 소유하지 않아야 합니다.
    // 강한 참조를 잡으면 연결 종료 후에도 방 엔트리가 세션 수명을 붙들 수 있으므로 weak_ptr 집합을 씁니다.
    using RoomSet = std::set<WeakSession, WeakLess>;

    using Exec = boost::asio::io_context::executor_type;
    // 방 단위 순서를 보장하려고 글로벌 락 하나로 모든 방을 막으면 hot room 하나가 전체를 지연시킵니다.
    // 그래서 room별 strand를 두어 같은 방 안의 순서만 보장하고, 다른 방은 병렬성을 유지합니다.
    using Strand = boost::asio::strand<Exec>;

    struct HookPluginState;

    /** @brief write-behind 동작 파라미터 집합입니다. */
    struct WriteBehindConfig {
        bool enabled{false};
        std::string stream_key{"session_events"};
        std::optional<std::size_t> maxlen{};
        bool approximate{true};
    };

    /** @brief presence 키 TTL/prefix 설정입니다. */
    struct PresenceConfig {
        unsigned int ttl{30}; // 초 단위. 너무 길면 유령 online 상태가 오래 남고, 너무 짧으면 재갱신 비용이 커집니다.
        std::string prefix;
    };

    /**
     * @brief reconnect/resume용 logical session continuity 설정입니다.
     *
     * transport 세션이 끊겨도 사용자 맥락을 잠시 이어 붙일 수 있도록
     * lease TTL, Redis prefix, world ownership 키를 한군데로 묶습니다.
     */
    struct ContinuityConfig {
        bool enabled{false};
        unsigned int lease_ttl_sec{15 * 60};
        std::string redis_prefix;
        std::string default_world_id;
        std::string current_owner_id;
        std::string topology_runtime_assignment_key;
    };

    /**
     * @brief 세션/방/제재 상태를 보관하는 서버 메모리 상태 컨테이너입니다.
     *
     * 상태를 한 구조체로 모으는 이유는 "어떤 맵을 어떤 락으로 보호하는가"를 분명히 하기 위해서입니다.
     * 자료구조가 클래스 전역에 흩어지면 로그인/퇴장/제재가 서로 다른 락 규칙을 쓰기 쉬워집니다.
     */
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
    
    // 방별 메시지 순서를 보장하되, 전체 서버를 직렬화하지 않기 위한 room-local 실행기 테이블입니다.
    std::unordered_map<std::string, std::shared_ptr<Strand>> room_strands_;
    Strand& strand_for(const std::string& room);

    WriteBehindConfig write_behind_;
    PresenceConfig presence_{};
    ContinuityConfig continuity_{};
    
    /** @brief 스냅샷/refresh 경로의 최근 메시지 캐시 파라미터입니다. */
    struct HistoryConfig {
        std::size_t recent_limit{20};
        std::size_t max_list_len{200};
        std::size_t fetch_factor{3};
        unsigned int cache_ttl_sec{6 * 60 * 60};
    } history_;

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

    // Redis 키 생성 규칙을 흩어 놓으면 읽기/쓰기 경로가 다른 네이밍을 써서 복원이 깨지기 쉽습니다.
    // 키 구성은 helper로 고정해 continuity와 캐시 스키마를 한곳에서 관리합니다.
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
    std::optional<server::core::discovery::WorldLifecyclePolicy> load_continuity_world_policy(const std::string& world_id);
    std::optional<server::core::worlds::WorldMigrationEnvelope> load_continuity_world_migration(const std::string& world_id);
    std::optional<server::core::worlds::TopologyActuationRuntimeAssignmentItem> load_topology_runtime_assignment() const;
    std::string current_runtime_default_world_id() const;
    AppMigrationRoomHandoff resolve_app_world_migration_room_handoff(
        const server::core::worlds::WorldMigrationEnvelope& migration) const;
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

    // refresh에서 저장소를 매번 다시 읽으면 재접속 폭주 시 Redis/DB가 먼저 흔들립니다.
    // 최근 메시지 캐시는 스냅샷 비용을 낮추되, TTL/길이를 제한해 메모리와 정합성 균형을 맞춥니다.
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

    // 플러그인이 메시지를 처리/차단했으면 true를 반환합니다.
    // 이 규칙을 명확히 두지 않으면 "플러그인도 처리했고 기본 경로도 다시 처리한" 중복 전송이 생깁니다.
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
