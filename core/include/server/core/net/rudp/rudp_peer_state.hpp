#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>

namespace server::core::net::rudp {

/** @brief RUDP 피어 수명주기 상태입니다. */
enum class LifecycleState : std::uint8_t {
    kIdle = 0,
    kHelloSent,
    kHelloReceived,
    kEstablished,
    kDraining,
    kClosed,
};

/** @brief RUDP 엔진 설정값입니다. */
struct RudpConfig {
    std::uint32_t handshake_timeout_ms{1500};
    std::uint32_t idle_timeout_ms{10000};
    std::uint32_t ack_delay_ms{10};
    std::uint32_t rto_min_ms{50};
    std::uint32_t rto_max_ms{2000};
    std::size_t max_inflight_packets{256};
    std::size_t max_inflight_bytes{256 * 1024};
    std::size_t mtu_payload_bytes{1200};
};

/** @brief RTT/RTO 추정 상태입니다. */
struct RttEstimator {
    bool initialized{false};
    std::uint32_t srtt_ms{0};
    std::uint32_t rttvar_ms{0};
    std::uint32_t rto_ms{200};

    /** @brief 새 RTT 샘플을 반영해 RTO를 갱신합니다. */
    void update(std::uint32_t sample_ms, std::uint32_t rto_min_ms, std::uint32_t rto_max_ms) {
        if (sample_ms == 0) {
            return;
        }

        if (!initialized) {
            initialized = true;
            srtt_ms = sample_ms;
            rttvar_ms = std::max<std::uint32_t>(1, sample_ms / 2);
        } else {
            const auto err = static_cast<std::uint32_t>(std::abs(static_cast<int>(srtt_ms) - static_cast<int>(sample_ms)));
            rttvar_ms = static_cast<std::uint32_t>((3ULL * rttvar_ms + err) / 4ULL);
            srtt_ms = static_cast<std::uint32_t>((7ULL * srtt_ms + sample_ms) / 8ULL);
        }

        const auto candidate = srtt_ms + std::max<std::uint32_t>(1, 4U * rttvar_ms);
        rto_ms = std::clamp(candidate, rto_min_ms, rto_max_ms);
    }
};

/** @brief 단일 피어의 RUDP 런타임 상태입니다. */
struct RudpPeerState {
    LifecycleState lifecycle{LifecycleState::kIdle};
    std::uint32_t connection_id{0};
    std::uint32_t next_packet_number{1};
    std::uint64_t last_recv_unix_ms{0};
    std::uint64_t last_send_unix_ms{0};
    std::uint64_t last_ack_emit_unix_ms{0};
    std::uint32_t ack_largest{0};
    std::uint64_t ack_mask{0};
    bool fallback_required{false};
    std::string fallback_reason;
    RttEstimator rtt;

    /** @brief fallback 상태를 기록합니다. */
    void mark_fallback(std::string reason) {
        fallback_required = true;
        fallback_reason = std::move(reason);
    }

    /** @brief 상태를 완전히 초기화합니다. */
    void reset() {
        *this = RudpPeerState{};
    }
};

} // namespace server::core::net::rudp
