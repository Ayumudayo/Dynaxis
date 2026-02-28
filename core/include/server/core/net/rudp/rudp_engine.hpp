#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "server/core/net/rudp/ack_window.hpp"
#include "server/core/net/rudp/retransmission_queue.hpp"
#include "server/core/net/rudp/rudp_packet.hpp"
#include "server/core/net/rudp/rudp_peer_state.hpp"

namespace server::core::net::rudp {

/** @brief datagram 처리 결과입니다. */
struct ProcessResult {
    bool parsed{false};
    bool consumed{false};
    bool handshake_established{false};
    bool fallback_required{false};
    std::string fallback_reason;
    std::vector<std::vector<std::uint8_t>> egress_datagrams;
    std::vector<std::vector<std::uint8_t>> inner_frames;
    std::uint64_t retransmit_count{0};
};

/** @brief Core 재사용 가능한 RUDP 엔진입니다. */
class RudpEngine {
public:
    explicit RudpEngine(RudpConfig config = {});

    /** @brief 엔진/피어 상태를 초기화합니다. */
    void reset();

    /** @brief 외부에서 datagram을 입력해 상태를 진전시킵니다. */
    ProcessResult process_datagram(std::span<const std::uint8_t> datagram, std::uint64_t now_unix_ms);

    /** @brief 재전송/타임아웃 poll 결과를 반환합니다. */
    ProcessResult poll(std::uint64_t now_unix_ms);

    /** @brief 로컬에서 HELLO 패킷을 생성합니다. */
    std::vector<std::uint8_t> make_hello(std::uint32_t connection_id, std::uint64_t now_unix_ms);

    /** @brief 로컬에서 CLOSE 패킷을 생성합니다. */
    std::vector<std::uint8_t> make_close(std::uint64_t now_unix_ms);

    /**
     * @brief inner frame을 RUDP DATA로 감싸 큐/송신합니다.
     * @param out_datagram 즉시 송신할 datagram 출력
     * @return 큐/상태 제약에 걸리지 않고 패킷 생성에 성공하면 `true`
     */
    bool queue_reliable_payload(std::span<const std::uint8_t> inner_frame,
                                std::uint8_t channel,
                                std::uint64_t now_unix_ms,
                                std::vector<std::uint8_t>& out_datagram);

    /** @brief 현재 피어 상태를 반환합니다. */
    const RudpPeerState& state() const noexcept { return state_; }

    /** @brief 현재 설정을 반환합니다. */
    const RudpConfig& config() const noexcept { return config_; }

private:
    std::vector<std::uint8_t> make_packet(PacketType type,
                                          std::uint8_t flags,
                                          std::uint8_t channel,
                                          std::uint32_t packet_number,
                                          std::uint64_t now_unix_ms,
                                          std::span<const std::uint8_t> payload) const;

    void set_fallback(ProcessResult& result, std::string reason);

    RudpConfig config_;
    RudpPeerState state_;
    AckWindow ack_window_;
    RetransmissionQueue retransmission_queue_;
};

} // namespace server::core::net::rudp
