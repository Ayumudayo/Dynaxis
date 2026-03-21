#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace server::core::net::rudp {

/** @brief 재전송 큐 엔트리입니다. 최초 전송 시각과 재전송 횟수를 함께 보관합니다. */
struct PendingPacket {
    std::uint32_t packet_number{0};
    std::uint64_t first_sent_unix_ms{0};
    std::uint64_t last_sent_unix_ms{0};
    std::uint32_t retransmit_count{0};
    std::vector<std::uint8_t> encoded_frame;
};

/**
 * @brief ACK 대기 패킷 큐와 재전송 타이머를 관리합니다.
 *
 * 이 계층을 별도 큐로 두는 이유는, "전송은 됐지만 아직 확인되지 않은 패킷"의 수명주기를
 * 소켓/엔진 본체와 분리해 관리하기 위해서입니다. 그래야 inflight 상한과 timeout 재전송을
 * 한곳에서 계산할 수 있습니다.
 */
class RetransmissionQueue {
public:
    RetransmissionQueue() = default;

    /** @brief 내부 상태를 초기화합니다. */
    void reset();

    /**
     * @brief ACK 대기 패킷을 큐에 추가합니다.
     * @param packet_number 전송한 packet number
     * @param sent_unix_ms 최초/최근 전송 시각(unix ms)
     * @param encoded_frame 전송된 RUDP 프레임 바이트
     */
    void push(std::uint32_t packet_number, std::uint64_t sent_unix_ms, std::vector<std::uint8_t> encoded_frame);

    /** @brief `ack_largest + ack_mask(64)` 조합으로 ACK 처리합니다. */
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

    /** @brief ACK 처리 전 내부 큐 엔트리입니다. */
    struct Entry {
        PendingPacket packet;
        bool acked{false};
    };

    std::deque<Entry> pending_;
    std::size_t inflight_bytes_{0};
};

} // namespace server::core::net::rudp
