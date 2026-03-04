#pragma once

#include "server/chat/chat_hook_plugin_abi.hpp"
#include "server/core/plugin/plugin_chain_host.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace server::app::chat {

/**
 * @brief 여러 chat hook 플러그인을 체인으로 구성해 순차 적용합니다.
 */
class ChatHookPluginChain {
public:
    /** @brief 플러그인 체인 구성값입니다. */
    struct Config {
        /** @brief 명시 플러그인 경로 목록(입력 순서 유지, 정렬 없음). */
        std::vector<std::filesystem::path> plugin_paths;

        /** @brief plugin_paths가 비어 있을 때 사용할 디렉터리 모드 경로입니다. */
        std::optional<std::filesystem::path> plugins_dir;

        /** @brief plugins_dir가 비어 있거나 로드 가능한 모듈이 없을 때 사용할 fallback 디렉터리입니다. */
        std::optional<std::filesystem::path> fallback_plugins_dir;

        /** @brief 모든 플러그인이 공유하는 cache-copy 디렉터리입니다. */
        std::filesystem::path cache_dir;

        /** @brief 단일 플러그인 모드 lock/sentinel 경로(옵션)입니다. */
        std::optional<std::filesystem::path> single_lock_path;
    };

    /** @brief on_chat_send 체인 실행 결과입니다. */
    struct Outcome {
        bool stop_default{false};
        std::vector<std::string> notices;
    };

    /** @brief 로그인/입장/퇴장/세션 이벤트 게이트 훅 실행 결과입니다. */
    struct GateOutcome {
        bool stop_default{false};
        std::vector<std::string> notices;
        std::string deny_reason;
    };

    /** @brief 관리자 명령 훅 실행 결과입니다. */
    struct AdminOutcome {
        bool stop_default{false};
        std::vector<std::string> notices;
        std::string response_json;
        std::string deny_reason;
    };

    /** @brief 단일 플러그인 메트릭 스냅샷입니다. */
    struct PluginMetricsSnapshot {
        std::filesystem::path plugin_path;
        bool loaded{false};
        std::string name;
        std::string version;
        std::uint64_t reload_attempt_total{0};
        std::uint64_t reload_success_total{0};
        std::uint64_t reload_failure_total{0};
    };

    /** @brief 체인 상태 메트릭 스냅샷입니다. */
    struct MetricsSnapshot {
        bool configured{false};
        std::string mode; // none|dir|paths|single
        std::vector<PluginMetricsSnapshot> plugins;
    };

    /**
     * @brief 플러그인 체인을 생성합니다.
     * @param cfg 체인 구성값
     */
    explicit ChatHookPluginChain(Config cfg);

    /**
     * @brief 구성에 맞는 플러그인 목록을 재스캔하고 변경 모듈을 hot-reload합니다.
     */
    void poll_reload();

    /**
     * @brief 체인 순서대로 `on_chat_send`를 적용합니다.
     * @param session_id 세션 ID
     * @param room 방 이름
     * @param user 사용자 이름
     * @param text 입출력 메시지 본문(`kReplaceText` 시 변경됨)
     * @return 기본 경로 중단 여부와 notice 목록
     */
    Outcome on_chat_send(std::uint32_t session_id,
                         std::string_view room,
                         std::string_view user,
                         std::string& text) const;

    /**
     * @brief 체인 순서대로 `on_login`을 적용합니다.
     * @param session_id 세션 ID
     * @param user 사용자 이름
     * @return 기본 경로 중단 여부와 notice/deny 정보
     */
    GateOutcome on_login(std::uint32_t session_id, std::string_view user) const;

    /**
     * @brief 체인 순서대로 `on_join`을 적용합니다.
     * @param session_id 세션 ID
     * @param user 사용자 이름
     * @param room 대상 방 이름
     * @return 기본 경로 중단 여부와 notice/deny 정보
     */
    GateOutcome on_join(std::uint32_t session_id, std::string_view user, std::string_view room) const;

    /**
     * @brief 체인 순서대로 `on_leave`를 적용합니다.
     * @param session_id 세션 ID
     * @param user 사용자 이름
     * @param room 현재 방 이름
     * @return 기본 경로 중단 여부와 notice/deny 정보
     */
    GateOutcome on_leave(std::uint32_t session_id, std::string_view user, std::string_view room) const;

    /**
     * @brief 체인 순서대로 `on_session_event`를 적용합니다.
     * @param session_id 세션 ID
     * @param kind 세션 이벤트 종류(open/close)
     * @param user 사용자 이름
     * @param reason 이벤트 사유
     * @return 기본 경로 중단 여부와 notice/deny 정보
     */
    GateOutcome on_session_event(std::uint32_t session_id,
                                 SessionEventKindV2 kind,
                                 std::string_view user,
                                 std::string_view reason) const;

    /**
     * @brief 체인 순서대로 `on_admin_command`를 적용합니다.
     * @param command 관리자 명령 이름
     * @param issuer 명령 발행자
     * @param payload_json 명령 페이로드(JSON 문자열)
     * @return 기본 경로 중단 여부와 notice/response/deny 정보
     */
    AdminOutcome on_admin_command(std::string_view command,
                                  std::string_view issuer,
                                  std::string_view payload_json) const;

    /**
     * @brief 현재 체인 상태 스냅샷을 반환합니다.
     * @return 체인 메트릭 스냅샷
     */
    MetricsSnapshot metrics_snapshot() const;

private:
    using Host = server::core::plugin::PluginHost<ChatHookApiV2>;
    using HostChain = server::core::plugin::PluginChainHost<ChatHookApiV2>;

    HostChain host_;
};

} // namespace server::app::chat
