#pragma once

#include <cstdint>

namespace server::core::net::rudp {

/** @brief ACK 윈도우 업데이트 결과입니다. 중복, 역순, 추정 손실을 한 번에 담습니다. */
struct AckObservation {
    bool accepted{false};
    bool duplicate{false};
    bool reordered{false};
    std::uint64_t estimated_lost_packets{0};
    std::uint32_t ack_largest{0};
    std::uint64_t ack_mask{0};
};

/**
 * @brief packet number 기반 ACK 윈도우 추적기입니다.
 *
 * `ack_largest + ack_mask(64)` 형태를 유지하며,
 * 중복/역순 패킷 판단에 필요한 최소 상태만 보관합니다. ACK 상태를 별도 타입으로 유지하면
 * 엔진은 전송 품질 판단을 재사용할 수 있고, 실제 소켓/세션 구현과도 느슨하게 분리됩니다.
 */
class AckWindow {
public:
    AckWindow() = default;

    /** @brief 내부 상태를 초기화합니다. */
    void reset();

    /**
     * @brief 새 packet number를 관측해 ACK 상태를 갱신합니다.
     * @param packet_number 관측한 packet number
     * @return 중복/역순/추정 손실 정보와 최신 ACK 상태
     */
    AckObservation observe(std::uint32_t packet_number);

    /** @brief 가장 큰 연속 ACK 번호를 반환합니다. */
    std::uint32_t ack_largest() const noexcept { return largest_; }
    /** @brief 최근 64개 ACK 비트마스크를 반환합니다. */
    std::uint64_t ack_mask() const noexcept { return mask_; }

private:
    bool initialized_{false};
    std::uint32_t largest_{0};
    std::uint64_t mask_{0};
};

} // namespace server::core::net::rudp
