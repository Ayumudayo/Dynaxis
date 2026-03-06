#pragma once

#include <charconv>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <unordered_set>

namespace gateway {

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

/** @brief RUDP opcode allowlist(CSV)를 파싱해 opcode 집합으로 반환합니다. */
inline std::unordered_set<std::uint16_t> parse_rudp_opcode_allowlist(std::string_view csv) {
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

/** @brief UDP ingress opcode allowlist(CSV)를 파싱해 opcode 집합으로 반환합니다. */
inline std::unordered_set<std::uint16_t> parse_udp_opcode_allowlist(std::string_view csv) {
    return parse_rudp_opcode_allowlist(csv);
}

/** @brief 세션 canary 및 opcode allowlist 기반 RUDP rollout 정책입니다. */
struct RudpRolloutPolicy {
    /** @brief gateway RUDP 경로 활성화 여부입니다. */
    bool enabled{false};
    /** @brief 세션 단위 canary 비율(0~100)입니다. */
    std::uint32_t canary_percent{0};
    /** @brief RUDP를 허용할 opcode 집합입니다. */
    std::unordered_set<std::uint16_t> opcode_allowlist;

    /** @brief 주어진 세션이 canary 대상인지 결정합니다. */
    bool session_selected(std::string_view session_id, std::uint64_t nonce) const noexcept {
        if (!enabled || canary_percent == 0) {
            return false;
        }
        if (canary_percent >= 100) {
            return true;
        }
        return static_cast<std::uint32_t>(detail::fnv1a64(session_id, nonce) % 100ull) < canary_percent;
    }

    /** @brief opcode가 allowlist에 포함됐는지 반환합니다. */
    bool opcode_allowed(std::uint16_t opcode) const noexcept {
        return !opcode_allowlist.empty() && opcode_allowlist.contains(opcode);
    }
};

} // namespace gateway
