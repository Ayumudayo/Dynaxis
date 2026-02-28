#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>

namespace gateway {

/** @brief 고정 burst와 refill rate를 갖는 thread-safe 토큰 버킷입니다. */
class TokenBucket {
public:
    TokenBucket() = default;

    void configure(double tokens_per_sec, double burst_tokens) {
        std::lock_guard<std::mutex> lock(mutex_);

        refill_per_sec_ = std::max(0.0, tokens_per_sec);
        burst_tokens_ = std::max(0.0, burst_tokens);
        tokens_ = burst_tokens_;
        last_refill_ms_ = 0;
    }

    bool consume(std::uint64_t now_ms, double amount = 1.0) {
        std::lock_guard<std::mutex> lock(mutex_);
        refill_locked(now_ms);

        const double required = std::max(0.0, amount);
        if (tokens_ < required) {
            return false;
        }

        tokens_ -= required;
        return true;
    }

    double available(std::uint64_t now_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        refill_locked(now_ms);
        return tokens_;
    }

private:
    void refill_locked(std::uint64_t now_ms) {
        if (last_refill_ms_ == 0) {
            last_refill_ms_ = now_ms;
            return;
        }
        if (now_ms <= last_refill_ms_) {
            return;
        }

        const auto elapsed_ms = now_ms - last_refill_ms_;
        const double refill = refill_per_sec_ * (static_cast<double>(elapsed_ms) / 1000.0);
        tokens_ = std::min(burst_tokens_, tokens_ + refill);
        last_refill_ms_ = now_ms;
    }

    std::mutex mutex_;
    double refill_per_sec_{0.0};
    double burst_tokens_{0.0};
    double tokens_{0.0};
    std::uint64_t last_refill_ms_{0};
};

/** @brief 고정 시간 창 단위 재시도 예산을 제어합니다. */
class RetryBudget {
public:
    RetryBudget() = default;

    void configure(std::uint32_t budget_per_window, std::uint64_t window_ms = 60000) {
        std::lock_guard<std::mutex> lock(mutex_);

        budget_per_window_ = budget_per_window;
        window_ms_ = std::max<std::uint64_t>(1, window_ms);
        window_start_ms_ = 0;
        used_in_window_ = 0;
    }

    bool consume(std::uint64_t now_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        roll_window_locked(now_ms);

        if (used_in_window_ >= budget_per_window_) {
            return false;
        }

        ++used_in_window_;
        return true;
    }

    std::uint32_t remaining(std::uint64_t now_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        roll_window_locked(now_ms);

        if (used_in_window_ >= budget_per_window_) {
            return 0;
        }
        return budget_per_window_ - used_in_window_;
    }

private:
    void roll_window_locked(std::uint64_t now_ms) {
        if (window_start_ms_ == 0) {
            window_start_ms_ = now_ms;
            return;
        }
        if (now_ms < window_start_ms_) {
            window_start_ms_ = now_ms;
            used_in_window_ = 0;
            return;
        }

        const auto elapsed = now_ms - window_start_ms_;
        if (elapsed >= window_ms_) {
            window_start_ms_ = now_ms;
            used_in_window_ = 0;
        }
    }

    std::mutex mutex_;
    std::uint32_t budget_per_window_{0};
    std::uint64_t window_ms_{60000};
    std::uint64_t window_start_ms_{0};
    std::uint32_t used_in_window_{0};
};

/** @brief 연속 실패 임계치 기반의 단순 circuit breaker입니다. */
class CircuitBreaker {
public:
    CircuitBreaker() = default;

    void configure(bool enabled, std::uint32_t failure_threshold, std::uint64_t open_window_ms) {
        std::lock_guard<std::mutex> lock(mutex_);

        enabled_ = enabled;
        failure_threshold_ = std::max<std::uint32_t>(1, failure_threshold);
        open_window_ms_ = std::max<std::uint64_t>(1, open_window_ms);
        consecutive_failures_ = 0;
        open_until_ms_ = 0;
    }

    bool allow(std::uint64_t now_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_) {
            return true;
        }
        if (open_until_ms_ == 0) {
            return true;
        }
        if (now_ms >= open_until_ms_) {
            open_until_ms_ = 0;
            consecutive_failures_ = 0;
            return true;
        }
        return false;
    }

    bool record_failure(std::uint64_t now_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_) {
            return false;
        }
        if (open_until_ms_ != 0 && now_ms < open_until_ms_) {
            return false;
        }
        if (open_until_ms_ != 0 && now_ms >= open_until_ms_) {
            open_until_ms_ = 0;
            consecutive_failures_ = 0;
        }

        ++consecutive_failures_;
        if (consecutive_failures_ >= failure_threshold_) {
            open_until_ms_ = now_ms + open_window_ms_;
            consecutive_failures_ = 0;
            return true;
        }
        return false;
    }

    void record_success() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_) {
            return;
        }
        open_until_ms_ = 0;
        consecutive_failures_ = 0;
    }

    bool is_open(std::uint64_t now_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_) {
            return false;
        }
        if (open_until_ms_ == 0) {
            return false;
        }
        if (now_ms >= open_until_ms_) {
            open_until_ms_ = 0;
            consecutive_failures_ = 0;
            return false;
        }
        return true;
    }

private:
    std::mutex mutex_;
    bool enabled_{true};
    std::uint32_t failure_threshold_{5};
    std::uint64_t open_window_ms_{10000};
    std::uint32_t consecutive_failures_{0};
    std::uint64_t open_until_ms_{0};
};

} // namespace gateway
