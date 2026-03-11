#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace server::core::config {

enum class RuntimeSettingKey {
    kPresenceTtlSec,
    kRecentHistoryLimit,
    kRoomRecentMaxlen,
    kChatSpamThreshold,
    kChatSpamWindowSec,
    kChatSpamMuteSec,
    kChatSpamBanSec,
    kChatSpamBanViolations,
};

struct RuntimeSettingRule {
    RuntimeSettingKey key_id;
    std::string_view key_name;
    std::uint32_t min_value;
    std::uint32_t max_value;
};

inline constexpr std::array<RuntimeSettingRule, 8> kRuntimeSettingRules{{
    {RuntimeSettingKey::kPresenceTtlSec, "presence_ttl_sec", 5, 3600},
    {RuntimeSettingKey::kRecentHistoryLimit, "recent_history_limit", 5, 2000},
    {RuntimeSettingKey::kRoomRecentMaxlen, "room_recent_maxlen", 5, 5000},
    {RuntimeSettingKey::kChatSpamThreshold, "chat_spam_threshold", 3, 100},
    {RuntimeSettingKey::kChatSpamWindowSec, "chat_spam_window_sec", 1, 120},
    {RuntimeSettingKey::kChatSpamMuteSec, "chat_spam_mute_sec", 5, 86400},
    {RuntimeSettingKey::kChatSpamBanSec, "chat_spam_ban_sec", 10, 604800},
    {RuntimeSettingKey::kChatSpamBanViolations, "chat_spam_ban_violations", 1, 20},
}};

inline constexpr const RuntimeSettingRule* find_runtime_setting_rule(std::string_view key) {
    for (const auto& rule : kRuntimeSettingRules) {
        if (rule.key_name == key) {
            return &rule;
        }
    }
    return nullptr;
}

} // namespace server::core::config
