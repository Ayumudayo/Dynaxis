#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace server::core::net::rudp {

/** @brief 재전송 큐 엔트리입니다. */
struct PendingPacket {
    std::uint32_t packet_number{0};
    std::uint64_t first_sent_unix_ms{0};
    std::uint64_t last_sent_unix_ms{0};
    std::uint32_t retransmit_count{0};
    std::vector<std::uint8_t> encoded_frame;
};

/** @brief ACK 대기 패킷 큐와 재전송 타이머를 관리합니다. */
class RetransmissionQueue {
public:
    RetransmissionQueue() = default;

    /** @brief 내부 상태를 초기화합니다. */
    void reset();

    /** @brief ACK 대기 패킷을 큐에 추가합니다. */
    void push(std::uint32_t packet_number, std::uint64_t sent_unix_ms, std::vector<std::uint8_t> encoded_frame);

    /** @brief `ack_largest + ack_mask(64)`로 ACK 처리합니다. */
    void mark_acked(std::uint32_t ack_largest, std::uint64_t ack_mask);

    /**
     * @brief 재전송 시점이 지난 패킷을 최대 `max_batch`개 반환합니다.
     * @return 재전송 대상 패킷 목록(프레임 복사본)
     */
    std::vector<PendingPacket> collect_timeouts(std::uint64_t now_unix_ms,
                                                std::uint32_t rto_ms,
                                                std::size_t max_batch);

    /** @brief 현재 inflight 패킷 수를 반환합니다. */
    std::size_t inflight_packets() const noexcept { return pending_.size(); }

    /** @brief 현재 inflight 바이트 수를 반환합니다. */
    std::size_t inflight_bytes() const noexcept { return inflight_bytes_; }

private:
    static bool is_acked_by_mask(std::uint32_t packet_number, std::uint32_t ack_largest, std::uint64_t ack_mask);
    void erase_acked();

    struct Entry {
        PendingPacket packet;
        bool acked{false};
    };

    std::deque<Entry> pending_;
    std::size_t inflight_bytes_{0};
};

} // namespace server::core::net::rudp
