#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace server::config {

/** @brief 영속 설정에서 지원하는 런타임 설정 키입니다. */
enum class RuntimeSettingKey {
    kPresenceTtlSec,
    kRecentHistoryLimit,
    kRoomRecentMaxlen,
};

/** @brief 런타임 설정 키의 검증 범위입니다. */
struct RuntimeSettingRule {
    RuntimeSettingKey key_id;     ///< 코드에서 사용하는 enum 식별자
    std::string_view key_name;    ///< 저장소에 기록되는 영속 키 이름
    std::uint32_t min_value;      ///< 허용 최소값(포함)
    std::uint32_t max_value;      ///< 허용 최대값(포함)
};

/** @brief 유효한 런타임 설정 규칙의 컴파일타임 레지스트리입니다. */
inline constexpr std::array<RuntimeSettingRule, 3> kRuntimeSettingRules{{
    {RuntimeSettingKey::kPresenceTtlSec, "presence_ttl_sec", 5, 3600},
    {RuntimeSettingKey::kRecentHistoryLimit, "recent_history_limit", 5, 2000},
    {RuntimeSettingKey::kRoomRecentMaxlen, "room_recent_maxlen", 5, 5000},
}};

/**
 * @brief 영속 키 이름으로 런타임 설정 규칙을 찾습니다.
 * @param key 런타임 설정 키 문자열
 * @return 매칭 규칙 포인터(없으면 `nullptr`)
 */
inline constexpr const RuntimeSettingRule* find_runtime_setting_rule(std::string_view key) {
    for (const auto& rule : kRuntimeSettingRules) {
        if (rule.key_name == key) {
            return &rule;
        }
    }
    return nullptr;
}

} // namespace server::config
