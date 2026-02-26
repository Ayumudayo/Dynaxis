#pragma once

#include <cstdint>

namespace gateway {

/**
 * @brief 단일 바인딩 세션의 UDP 시퀀스 기반 품질 신호를 추적합니다.
 *
 * 패킷 시퀀스와 수신 시각 차이를 이용해
 * 중복/역순 패킷을 식별하고 손실 및 지터를 추정합니다.
 */
class UdpSequencedMetrics {
public:
    /** @brief 단일 패킷 업데이트에 대한 분류 결과와 품질 변화량입니다. */
    struct UpdateResult {
        bool accepted{false};                    ///< 정상 진행 패킷으로 수용됨
        bool duplicate{false};                   ///< 마지막 수용 시퀀스와 동일한 중복 패킷
        bool reordered{false};                   ///< 마지막 수용 시퀀스보다 오래된 역순 패킷
        std::uint64_t estimated_lost_packets{0}; ///< 이번 패킷 이전 누락으로 추정되는 패킷 수
        std::uint64_t jitter_ms{0};              ///< 패킷 간 도착 지터 변화량(ms)
    };

    /** @brief 추적기 상태를 초기화합니다(주로 재바인딩 시). */
    void reset() {
        initialized_ = false;
        last_seq_ = 0;
        last_recv_ms_ = 0;
        last_interarrival_ms_ = 0;
    }

    /**
     * @brief 단일 패킷 샘플을 처리하고 품질 통계를 갱신합니다.
     * @param seq 전송 헤더의 패킷 시퀀스 번호
     * @param recv_unix_ms 패킷 수신 유닉스 시각(ms)
     * @return 패킷 분류 결과 및 추정 품질 변화량
     */
    UpdateResult on_packet(std::uint32_t seq, std::uint64_t recv_unix_ms) {
        UpdateResult result{};

        if (!initialized_) {
            initialized_ = true;
            last_seq_ = seq;
            last_recv_ms_ = recv_unix_ms;
            last_interarrival_ms_ = 0;
            result.accepted = true;
            return result;
        }

        if (seq == last_seq_) {
            result.duplicate = true;
            return result;
        }

        if (seq < last_seq_) {
            result.reordered = true;
            return result;
        }

        result.accepted = true;
        if (seq > (last_seq_ + 1u)) {
            result.estimated_lost_packets = static_cast<std::uint64_t>(seq - last_seq_ - 1u);
        }

        if (last_recv_ms_ != 0) {
            if (recv_unix_ms >= last_recv_ms_) {
                const auto interarrival_ms = recv_unix_ms - last_recv_ms_;
                if (last_interarrival_ms_ != 0) {
                    result.jitter_ms = (interarrival_ms >= last_interarrival_ms_)
                        ? (interarrival_ms - last_interarrival_ms_)
                        : (last_interarrival_ms_ - interarrival_ms);
                }
                last_interarrival_ms_ = interarrival_ms;
            } else {
                last_interarrival_ms_ = 0;
            }
        }

        last_seq_ = seq;
        last_recv_ms_ = recv_unix_ms;
        return result;
    }

private:
    bool initialized_{false};
    std::uint32_t last_seq_{0};
    std::uint64_t last_recv_ms_{0};
    std::uint64_t last_interarrival_ms_{0};
};

} // namespace gateway
