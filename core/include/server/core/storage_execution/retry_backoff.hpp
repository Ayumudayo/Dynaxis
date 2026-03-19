#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>

namespace server::core::storage_execution {

/** @brief generic storage execution retry 지연 계산 방식입니다. */
enum class RetryBackoffMode : std::uint8_t {
    kLinear = 0,
    kExponentialFullJitter = 1,
};

/** @brief storage execution retry/backoff 계산에 필요한 공용 정책입니다. */
struct RetryBackoffPolicy {
    RetryBackoffMode mode{RetryBackoffMode::kLinear};
    std::uint64_t base_delay_ms{100};
    std::uint64_t max_delay_ms{1'000};
};

/**
 * @brief 재시도 횟수에 대한 backoff 상한(ms)을 계산합니다.
 *
 * `kLinear`는 exact delay를 반환하고, `kExponentialFullJitter`는 random sampling의 upper bound를 반환합니다.
 */
inline constexpr std::uint64_t retry_backoff_upper_bound_ms(
    const RetryBackoffPolicy& policy,
    std::uint32_t attempt) noexcept {
    const std::uint64_t base_delay_ms = policy.base_delay_ms == 0 ? 1 : policy.base_delay_ms;
    const std::uint64_t max_delay_ms = std::max(base_delay_ms, policy.max_delay_ms);

    if (policy.mode == RetryBackoffMode::kLinear) {
        if (attempt == 0) {
            return 0;
        }

        const auto attempt_u64 = static_cast<std::uint64_t>(attempt);
        if (attempt_u64 > std::numeric_limits<std::uint64_t>::max() / base_delay_ms) {
            return max_delay_ms;
        }

        return std::min(max_delay_ms, base_delay_ms * attempt_u64);
    }

    constexpr std::uint32_t k_max_shift = 16;
    const std::uint32_t capped_attempt = std::min(attempt, k_max_shift);
    const std::uint64_t multiplier = 1ull << capped_attempt;
    if (multiplier > std::numeric_limits<std::uint64_t>::max() / base_delay_ms) {
        return max_delay_ms;
    }

    return std::min(max_delay_ms, base_delay_ms * multiplier);
}

/**
 * @brief 정책과 시드 RNG를 바탕으로 실제 retry delay(ms)를 샘플링합니다.
 *
 * `kLinear`는 deterministic delay를 그대로 반환하고, `kExponentialFullJitter`는 `[0, upper_bound]` 범위에서 샘플링합니다.
 */
template <typename UniformRandomBitGenerator>
std::uint64_t sample_retry_backoff_delay_ms(
    const RetryBackoffPolicy& policy,
    std::uint32_t attempt,
    UniformRandomBitGenerator& rng) {
    const std::uint64_t upper_bound_ms = retry_backoff_upper_bound_ms(policy, attempt);
    if (upper_bound_ms == 0 || policy.mode == RetryBackoffMode::kLinear) {
        return upper_bound_ms;
    }

    std::uniform_int_distribution<std::uint64_t> distribution(0, upper_bound_ms);
    return distribution(rng);
}

} // namespace server::core::storage_execution
