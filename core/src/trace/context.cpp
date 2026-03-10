#include "server/core/trace/context.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <mutex>
#include <random>
#include <string_view>

namespace server::core::trace {

namespace {

struct TraceContextState {
    std::string trace_id;
    std::string correlation_id;
    bool sampled{false};
};

thread_local TraceContextState g_trace_context;

struct TraceConfig {
    bool enabled{false};
    std::uint32_t sample_percent{100};
};

TraceConfig g_trace_config{};
bool g_trace_config_loaded{false};
std::mutex g_trace_config_mu;

bool parse_env_bool(const char* key, bool fallback) {
    if (const char* value = std::getenv(key); value && *value) {
        const auto text = std::string_view(value);
        if (text == "1" || text == "true" || text == "TRUE" || text == "on" || text == "ON") {
            return true;
        }
        if (text == "0" || text == "false" || text == "FALSE" || text == "off" || text == "OFF") {
            return false;
        }
    }
    return fallback;
}

std::uint32_t parse_env_u32(const char* key, std::uint32_t fallback, std::uint32_t min_value, std::uint32_t max_value) {
    if (const char* value = std::getenv(key); value && *value) {
        try {
            const auto parsed = std::stoul(value);
            if (parsed >= min_value && parsed <= max_value) {
                return static_cast<std::uint32_t>(parsed);
            }
        } catch (...) {
        }
    }
    return fallback;
}

const TraceConfig& config() {
    std::lock_guard<std::mutex> lock(g_trace_config_mu);
    if (!g_trace_config_loaded) {
        g_trace_config.enabled = parse_env_bool("RUNTIME_TRACING_ENABLED", false);
        g_trace_config.sample_percent = parse_env_u32("RUNTIME_TRACING_SAMPLE_PERCENT", 100, 0, 100);
        g_trace_config_loaded = true;
    }
    return g_trace_config;
}

std::string random_hex_16bytes() {
    static constexpr char kHex[] = "0123456789abcdef";

    std::array<std::uint8_t, 16> bytes{};
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    for (std::size_t i = 0; i < bytes.size(); i += sizeof(std::uint64_t)) {
        const auto value = rng();
        for (std::size_t j = 0; j < sizeof(std::uint64_t) && (i + j) < bytes.size(); ++j) {
            bytes[i + j] = static_cast<std::uint8_t>((value >> (j * 8)) & 0xFFu);
        }
    }

    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        out.push_back(kHex[(byte >> 4) & 0x0F]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

} // namespace

bool enabled() {
    return config().enabled;
}

std::uint32_t sample_percent() {
    return config().sample_percent;
}

bool should_sample(std::uint64_t seed) {
    if (!enabled()) {
        return false;
    }

    const auto percent = sample_percent();
    if (percent == 0) {
        return false;
    }
    if (percent >= 100) {
        return true;
    }

    return (seed % 100ull) < percent;
}

void reset_for_tests() {
    std::lock_guard<std::mutex> lock(g_trace_config_mu);
    g_trace_config = TraceConfig{};
    g_trace_config_loaded = false;
    g_trace_context = TraceContextState{};
}

std::string make_trace_id() {
    return random_hex_16bytes();
}

std::string make_correlation_id(std::uint32_t session_id, std::uint16_t msg_id, std::uint32_t seq) {
    return "s" + std::to_string(session_id)
         + "-m" + std::to_string(msg_id)
         + "-q" + std::to_string(seq);
}

std::string current_trace_id() {
    return g_trace_context.sampled ? g_trace_context.trace_id : std::string{};
}

std::string current_correlation_id() {
    return g_trace_context.sampled ? g_trace_context.correlation_id : std::string{};
}

bool current_sampled() {
    return g_trace_context.sampled;
}

ScopedContext::ScopedContext(std::string trace_id, std::string correlation_id, bool sampled) {
    if (!enabled() || !sampled || trace_id.empty()) {
        return;
    }

    prev_trace_id_ = g_trace_context.trace_id;
    prev_correlation_id_ = g_trace_context.correlation_id;
    prev_sampled_ = g_trace_context.sampled;

    g_trace_context.trace_id = std::move(trace_id);
    g_trace_context.correlation_id = std::move(correlation_id);
    g_trace_context.sampled = true;
    active_ = true;
}

ScopedContext::~ScopedContext() {
    if (!active_) {
        return;
    }

    g_trace_context.trace_id = std::move(prev_trace_id_);
    g_trace_context.correlation_id = std::move(prev_correlation_id_);
    g_trace_context.sampled = prev_sampled_;
}

} // namespace server::core::trace
