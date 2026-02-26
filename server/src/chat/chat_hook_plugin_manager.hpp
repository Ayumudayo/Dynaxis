#pragma once

#include "server/chat/chat_hook_plugin_abi.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace server::app::chat {

/**
 * @brief 단일 chat hook 플러그인의 로드/호출/리로드를 관리합니다.
 */
class ChatHookPluginManager {
public:
    /** @brief 플러그인 매니저 구성값입니다. */
    struct Config {
        /** @brief 운영자가 실제 배포/교체하는 원본 플러그인 경로 */
        std::filesystem::path plugin_path;
        /** @brief 원본을 복사해 안전하게 로드할 캐시 디렉터리 */
        std::filesystem::path cache_dir;
        /** @brief lock 파일이 존재하면 reload를 보류할 sentinel 경로(옵션) */
        std::optional<std::filesystem::path> lock_path;
    };

    /** @brief on_chat_send 호출 결과입니다. */
    struct Result {
        ChatHookDecisionV1 decision{ChatHookDecisionV1::kPass};
        std::string notice;
        std::string replacement_text;
    };

    /** @brief 플러그인 로딩/리로드 상태 메트릭 스냅샷입니다. */
    struct MetricsSnapshot {
        std::filesystem::path plugin_path;
        bool loaded{false};
        std::string name;
        std::string version;
        std::uint64_t reload_attempt_total{0};
        std::uint64_t reload_success_total{0};
        std::uint64_t reload_failure_total{0};
    };

    /**
     * @brief 플러그인 매니저를 생성합니다.
     * @param cfg 플러그인 설정
     */
    explicit ChatHookPluginManager(Config cfg);

    /**
     * @brief plugin_path 변경 시간을 확인해 필요 시 hot-reload를 수행합니다.
     *
     * 호출자는 주기적으로 이 함수를 poll해 플러그인 갱신을 반영합니다.
     */
    void poll_reload();

    /**
     * @brief 현재 로드된 플러그인(있다면)을 호출합니다.
     * @param session_id 세션 ID
     * @param room 방 이름
     * @param user 사용자 이름
     * @param text 메시지 본문
     * @return 플러그인 결정 결과
     *
     * 플러그인 예외는 내부에서 흡수해 서버 기본 채팅 경로를 보호합니다.
     */
    Result on_chat_send(std::uint32_t session_id,
                        std::string_view room,
                        std::string_view user,
                        std::string_view text) const;

    /**
     * @brief 현재 로드 상태 메트릭 스냅샷을 반환합니다.
     * @return 플러그인 메트릭 스냅샷
     */
    MetricsSnapshot metrics_snapshot() const;

private:
    struct LoadedPlugin;

    Config cfg_;
    std::atomic<std::shared_ptr<LoadedPlugin>> current_{};
    std::optional<std::filesystem::file_time_type> last_attempt_mtime_;
    mutable std::mutex reload_mu_;
    static std::atomic<std::uint64_t> g_cache_seq_;

    std::atomic<std::uint64_t> reload_attempt_total_{0};
    std::atomic<std::uint64_t> reload_success_total_{0};
    std::atomic<std::uint64_t> reload_failure_total_{0};
};

} // namespace server::app::chat
