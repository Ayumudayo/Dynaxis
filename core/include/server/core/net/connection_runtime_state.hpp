#pragma once

#include <atomic>
#include <cstddef>
#include <random>

namespace server::core::net {

/**
 * @brief packet session 계층이 공유하는 런타임 상태입니다.
 *
 * 활성 연결 수, 선택적 최대 연결 상한, 난수화된 세션 ID 시드를 함께 보관합니다.
 * 이 상태를 별도로 두는 이유는 accept/session 계층이 같은 카운터와 guardrail을 보되,
 * 세션 객체마다 각각 따로 추적하지 않게 하기 위해서입니다.
 */
struct ConnectionRuntimeState {
    std::atomic<std::size_t> connection_count{0};
    std::size_t max_connections = 10'000;
    std::atomic<std::uint32_t> next_session_id;

    ConnectionRuntimeState() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<std::uint32_t> dis(1000, 0xFFFFFF00u);
        next_session_id.store(dis(gen));
    }
};

} // namespace server::core::net
