#pragma once

#include <charconv>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <unordered_set>

namespace server::core::fps {

namespace detail {

inline bool is_ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

inline std::string_view trim_ascii(std::string_view value) {
    while (!value.empty() && is_ascii_space(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_ascii_space(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

inline bool parse_u16_token(std::string_view token, std::uint16_t& out) {
    token = trim_ascii(token);
    if (token.empty()) {
        return false;
    }

    int base = 10;
    if (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X')) {
        base = 16;
        token.remove_prefix(2);
    }
    if (token.empty()) {
        return false;
    }

    unsigned int parsed = 0;
    const auto* first = token.data();
    const auto* last = token.data() + token.size();
    const auto [ptr, ec] = std::from_chars(first, last, parsed, base);
    if (ec != std::errc{} || ptr != last || parsed > 0xFFFFu) {
        return false;
    }

    out = static_cast<std::uint16_t>(parsed);
    return true;
}

inline std::uint64_t fnv1a64(std::string_view session_id, std::uint64_t nonce) {
    std::uint64_t hash = 1469598103934665603ull;
    constexpr std::uint64_t kPrime = 1099511628211ull;

    for (const char c : session_id) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= kPrime;
    }

    for (std::uint32_t shift = 0; shift < 64; shift += 8) {
        hash ^= static_cast<std::uint8_t>((nonce >> shift) & 0xFFu);
        hash *= kPrime;
    }

    return hash;
}

} // namespace detail

/** @brief CSV allowlist를 opcode 집합으로 파싱합니다. */
inline std::unordered_set<std::uint16_t> parse_direct_opcode_allowlist(std::string_view csv) {
    std::unordered_set<std::uint16_t> out;

    std::size_t begin = 0;
    while (begin <= csv.size()) {
        std::size_t end = csv.find(',', begin);
        if (end == std::string_view::npos) {
            end = csv.size();
        }

        const auto token = csv.substr(begin, end - begin);
        std::uint16_t opcode = 0;
        if (detail::parse_u16_token(token, opcode)) {
            out.insert(opcode);
        }

        if (end == csv.size()) {
            break;
        }
        begin = end + 1;
    }

    return out;
}

/** @brief direct UDP/RUDP gameplay transport rollout policy입니다. */
struct DirectTransportRolloutPolicy {
    bool enabled{false};
    std::uint32_t canary_percent{0};
    std::unordered_set<std::uint16_t> opcode_allowlist;

    bool session_selected(std::string_view session_id, std::uint64_t nonce) const noexcept {
        if (!enabled || canary_percent == 0) {
            return false;
        }
        if (canary_percent >= 100) {
            return true;
        }
        return static_cast<std::uint32_t>(detail::fnv1a64(session_id, nonce) % 100ull) < canary_percent;
    }

    bool opcode_allowed(std::uint16_t opcode) const noexcept {
        return !opcode_allowlist.empty() && opcode_allowlist.contains(opcode);
    }
};

/** @brief Direct bind ticket 발급 시 선택되는 attach 모드입니다. */
enum class DirectAttachMode : std::uint8_t {
    kUdpOnly = 0,
    kRudpCanary,
};

/** @brief Attach decision reason입니다. */
enum class DirectAttachReason : std::uint8_t {
    kRolloutDisabled = 0,
    kAllowlistEmpty,
    kCanaryMiss,
    kCanarySelected,
};

/** @brief Direct bind attach decision입니다. */
struct DirectAttachDecision {
    DirectAttachMode mode{DirectAttachMode::kUdpOnly};
    DirectAttachReason reason{DirectAttachReason::kRolloutDisabled};
};

inline DirectAttachDecision evaluate_direct_attach(const DirectTransportRolloutPolicy& policy,
                                                   std::string_view session_id,
                                                   std::uint64_t nonce) noexcept {
    if (!policy.enabled) {
        return DirectAttachDecision{
            .mode = DirectAttachMode::kUdpOnly,
            .reason = DirectAttachReason::kRolloutDisabled,
        };
    }
    if (policy.opcode_allowlist.empty()) {
        return DirectAttachDecision{
            .mode = DirectAttachMode::kUdpOnly,
            .reason = DirectAttachReason::kAllowlistEmpty,
        };
    }
    if (!policy.session_selected(session_id, nonce)) {
        return DirectAttachDecision{
            .mode = DirectAttachMode::kUdpOnly,
            .reason = DirectAttachReason::kCanaryMiss,
        };
    }
    return DirectAttachDecision{
        .mode = DirectAttachMode::kRudpCanary,
        .reason = DirectAttachReason::kCanarySelected,
    };
}

} // namespace server::core::fps
