#pragma once

#include <cstdint>

namespace server::core::realtime {

/**
 * @brief 하나의 바인딩된 세션에 대한 sequenced direct-UDP transport 품질을 추적합니다.
 *
 * direct path는 "붙었다"만으로 충분하지 않습니다. duplicate, reorder, 추정 손실, jitter를
 * 함께 봐야 실제 품질을 판단할 수 있습니다. 이 추적기를 별도 타입으로 둔 이유는, 품질
 * 판단 로직을 gateway의 바인딩/세션 상태와 분리해 재사용 가능하게 만들기 위해서입니다.
 */
class UdpSequencedMetrics {
public:
    /** @brief 패킷 1개 처리 결과와 파생 품질 변화량입니다. */
    struct UpdateResult {
        bool accepted{false};                    ///< 정상적인 전진 진행으로 받아들였는지 여부
        bool duplicate{false};                   ///< 마지막으로 받아들인 packet과 같은 sequence인지 여부
        bool reordered{false};                   ///< 마지막으로 받아들인 packet보다 더 오래된 sequence인지 여부
        std::uint64_t estimated_lost_packets{0}; ///< 이번 packet 이전에 누락된 것으로 추정한 패킷 수
        std::uint64_t jitter_ms{0};              ///< 도착 간격 변화량을 단순 jitter 추정치로 계산한 값(ms)
    };

    /** @brief 추적기를 초기화합니다. 보통 새 bind/rebind 직후 호출합니다. */
    void reset() {
        initialized_ = false;
        last_seq_ = 0;
        last_recv_ms_ = 0;
        last_interarrival_ms_ = 0;
    }

    /**
     * @brief 패킷 샘플 1개를 소비하고 품질 통계를 갱신합니다.
     * @param seq transport header에서 읽은 packet sequence number
     * @param recv_unix_ms 패킷 수신 시각(unix ms)
     * @return 패킷 분류 결과와 파생 품질 변화량
     */
    [[nodiscard]] UpdateResult on_packet(std::uint32_t seq, std::uint64_t recv_unix_ms) {
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

} // namespace server::core::realtime
