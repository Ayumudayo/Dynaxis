#include "server/core/net/hive.hpp"

/**
 * @brief Hive(io_context 래퍼)의 run/stop 수명주기 구현입니다.
 *
 * work_guard를 통해 이벤트 루프 조기 종료를 막고,
 * stop 경로를 단일화해 멀티스레드 종료 순서를 예측 가능하게 유지합니다.
 */
namespace server::core::net {

Hive::Hive(io_context& io)
    : io_(io)
    , guard_(boost::asio::make_work_guard(io_)) {}

Hive::~Hive() {
    stop();
}

Hive::io_context& Hive::context() {
    return io_;
}

void Hive::run() {
    // work_guard가 살아 있는 동안 io_context::run()은 리턴하지 않고 계속 실행됩니다.
    // 즉, 이 함수를 호출한 스레드는 이벤트 루프가 되어 I/O 작업을 처리합니다.
    // 여러 스레드에서 이 함수를 호출하면 멀티스레드 I/O 처리가 가능해집니다.
    stopped_.store(false, std::memory_order_relaxed);
    io_.run();
}

void Hive::stop() {
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    // guard를 reset()하면 io_context에 더 이상 할 일이 없다고 알리게 됩니다.
    // 처리 중인 작업이 모두 끝나면 run()이 반환되고 스레드가 종료됩니다.
    guard_.reset();
    io_.stop();
}

bool Hive::is_stopped() const {
    return stopped_.load(std::memory_order_relaxed);
}

} // namespace server::core::net
