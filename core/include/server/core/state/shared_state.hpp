#pragma once

#include <atomic>
#include <cstddef>
#include <random>

namespace server::core {

/**
 * @brief 세션 계층이 공유하는 최소 전역 상태입니다.
 *
 * connection_count/max_connections는 수용량 보호에 사용하고,
 * next_session_id는 충돌 가능성을 낮춘 난수 시작값 기반으로 발급합니다.
 */
struct SharedState {
    std::atomic<std::size_t> connection_count{0};
    std::size_t max_connections = 10'000; ///< 0이면 최대 연결 수를 제한하지 않습니다.
    std::atomic<std::uint32_t> next_session_id;
    
    // 생성자: session_id의 시작값을 랜덤하게 설정합니다.
    // 이유: 서버가 재시작되거나 여러 서버가 동시에 실행될 때,
    // 모든 서버가 session_id를 1부터 시작하면 클라이언트에서 "나(me)"를 식별하는 데 혼란이 생깁니다.
    // 랜덤한 시작값을 사용하여 각 서버 인스턴스가 서로 다른 ID 범위를 가지도록 합니다.
    SharedState() {
        std::random_device rd;
        std::mt19937 gen(rd());
        // 1000 ~ 42억 사이의 랜덤한 값으로 시작합니다.
        std::uniform_int_distribution<std::uint32_t> dis(1000, 0xFFFFFF00u);
        next_session_id.store(dis(gen));
    }
};

} // namespace server::core
