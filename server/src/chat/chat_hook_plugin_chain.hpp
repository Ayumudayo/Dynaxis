#pragma once

#include "chat_hook_plugin_manager.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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

    /** @brief 체인 상태 메트릭 스냅샷입니다. */
    struct MetricsSnapshot {
        bool configured{false};
        std::string mode; // none|dir|paths|single
        std::vector<ChatHookPluginManager::MetricsSnapshot> plugins;
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
     * @brief 현재 체인 상태 스냅샷을 반환합니다.
     * @return 체인 메트릭 스냅샷
     */
    MetricsSnapshot metrics_snapshot() const;

private:
    using PluginList = std::vector<std::shared_ptr<ChatHookPluginManager>>;

    static std::string normalize_key(const std::filesystem::path& p);
    static std::string module_extension();
    bool get_desired_paths(std::vector<std::filesystem::path>& out) const;

    Config cfg_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<ChatHookPluginManager>> by_key_;
    std::atomic<std::shared_ptr<const PluginList>> ordered_{};
};

} // namespace server::app::chat
