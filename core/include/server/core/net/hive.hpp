#pragma once

#include <atomic>
#include <memory>

#include <boost/asio.hpp>

namespace server::core::net {

/**
 * @brief Asio `io_context`의 실행 수명주기를 관리하는 래퍼입니다.
 *
 * 왜 필요한가?
 * - 여러 모듈이 동일 `io_context`를 공유할 때 run/stop 책임을 한곳에 모아
 *   중복 정지나 조기 종료 같은 수명주기 오류를 줄입니다.
 * - work guard를 함께 소유해, 아직 남은 비동기 작업이 있는데도 `run()`이 조기에 끝나는 실수를 줄입니다.
 */
class Hive : public std::enable_shared_from_this<Hive> {
public:
    using io_context = boost::asio::io_context;
    using executor_guard = boost::asio::executor_work_guard<io_context::executor_type>;

    explicit Hive(io_context& io);
    ~Hive();

    Hive(const Hive&) = delete;
    Hive& operator=(const Hive&) = delete;

    /**
     * @brief 내부 `io_context` 참조를 반환합니다.
     * @return 공유 실행 컨텍스트 참조
     */
    io_context& context();

    /** @brief 이벤트 루프를 실행합니다. 공유 `io_context`의 실제 실행 지점입니다. */
    void run();
    /** @brief 이벤트 루프 정지를 요청합니다. */
    void stop();
    /**
     * @brief 정지 상태를 조회합니다.
     * @return 정지 상태면 `true`
     */
    bool is_stopped() const;

private:
    io_context& io_;
    executor_guard guard_;
    std::atomic<bool> stopped_{false};
};

} // namespace server::core::net
