#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace gateway {

/**
 * @brief endpoint별 UDP bind 반복 실패를 추적하고 임시 차단을 적용합니다.
 *
 * bind ticket 검증을 노린 반복 남용 시도를 줄이기 위해
 * 실패 윈도우와 차단 지속 시간을 함께 관리합니다.
 */
class UdpBindAbuseGuard {
public:
    /** @brief endpoint의 현재 차단 상태입니다. */
    struct BlockState {
        bool blocked{false};             ///< 현재 endpoint가 차단 상태인지 여부
        std::uint64_t retry_after_ms{0}; ///< 남은 차단 시간(ms)
    };

    UdpBindAbuseGuard() = default;

    /**
     * @brief 실패 윈도우와 차단 임계값을 설정합니다.
     * @param fail_window_ms 롤링 실패 윈도우 길이(ms)
     * @param fail_limit 차단 발동에 필요한 실패 횟수
     * @param block_ms 차단 지속 시간(ms)
     */
    void configure(std::uint32_t fail_window_ms,
                   std::uint32_t fail_limit,
                   std::uint32_t block_ms) {
        fail_window_ms_ = fail_window_ms;
        fail_limit_ = fail_limit;
        block_ms_ = block_ms;
    }

    /**
     * @brief endpoint의 현재 차단 상태를 반환합니다.
     * @param endpoint_key endpoint 식별 키(`ip:port`)
     * @param now_ms 현재 유닉스 시각(ms)
     * @return 차단 중이면 남은 재시도 대기 시간을 포함한 상태
     */
    BlockState block_state(std::string_view endpoint_key, std::uint64_t now_ms) {
        auto it = entries_.find(std::string(endpoint_key));
        if (it == entries_.end()) {
            return {};
        }

        auto& entry = it->second;
        if (entry.blocked_until_ms <= now_ms) {
            entry.blocked_until_ms = 0;
            return {};
        }

        return {true, entry.blocked_until_ms - now_ms};
    }

    /**
     * @brief bind 실패를 기록하고 차단 상태를 갱신합니다.
     * @param endpoint_key endpoint 식별 키(`ip:port`)
     * @param now_ms 현재 유닉스 시각(ms)
     * @return 이번 실패로 새 차단이 발동되면 `true`
     */
    bool record_failure(std::string_view endpoint_key, std::uint64_t now_ms) {
        auto& entry = entries_[std::string(endpoint_key)];
        if (entry.blocked_until_ms > now_ms) {
            return false;
        }

        if (entry.window_started_ms == 0 || now_ms < entry.window_started_ms ||
            (now_ms - entry.window_started_ms) > fail_window_ms_) {
            entry.window_started_ms = now_ms;
            entry.failures_in_window = 0;
        }

        ++entry.failures_in_window;
        if (entry.failures_in_window < fail_limit_) {
            return false;
        }

        entry.failures_in_window = 0;
        entry.window_started_ms = 0;
        entry.blocked_until_ms = now_ms + block_ms_;
        return true;
    }

    /**
     * @brief bind 성공 후 실패 카운터를 초기화합니다.
     * @param endpoint_key endpoint 식별 키(`ip:port`)
     */
    void record_success(std::string_view endpoint_key) {
        auto it = entries_.find(std::string(endpoint_key));
        if (it == entries_.end()) {
            return;
        }
        it->second.failures_in_window = 0;
        it->second.window_started_ms = 0;
    }

private:
    /** @brief Endpoint별 실패 윈도우/차단 시각을 보관하는 내부 상태입니다. */
    struct Entry {
        std::uint64_t window_started_ms{0};
        std::uint32_t failures_in_window{0};
        std::uint64_t blocked_until_ms{0};
    };

    std::uint32_t fail_window_ms_{10000};
    std::uint32_t fail_limit_{5};
    std::uint32_t block_ms_{60000};
    std::unordered_map<std::string, Entry> entries_;
};

} // namespace gateway
