#pragma once

#include <cstdint>
#include <string>

namespace server::core::trace {

bool enabled();
std::uint32_t sample_percent();
bool should_sample(std::uint64_t seed);
void reset_for_tests();

std::string make_trace_id();
std::string make_correlation_id(std::uint32_t session_id, std::uint16_t msg_id, std::uint32_t seq);

std::string current_trace_id();
std::string current_correlation_id();
bool current_sampled();

/** @brief 현재 스레드의 trace/correlation 컨텍스트를 RAII로 전환합니다. */
class ScopedContext {
public:
    ScopedContext(std::string trace_id, std::string correlation_id, bool sampled);
    ~ScopedContext();

    ScopedContext(const ScopedContext&) = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;

    bool active() const noexcept { return active_; }

private:
    bool active_{false};
    std::string prev_trace_id_;
    std::string prev_correlation_id_;
    bool prev_sampled_{false};
};

} // namespace server::core::trace
