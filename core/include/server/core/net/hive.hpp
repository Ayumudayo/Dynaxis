#pragma once

#include <atomic>
#include <memory>

#include <boost/asio.hpp>

namespace server::core::net {

class Hive : public std::enable_shared_from_this<Hive> {
public:
    using io_context = boost::asio::io_context;
    using executor_guard = boost::asio::executor_work_guard<io_context::executor_type>;

    explicit Hive(io_context& io);
    ~Hive();

    Hive(const Hive&) = delete;
    Hive& operator=(const Hive&) = delete;

    io_context& context();

    void run();
    void stop();
    bool is_stopped() const;

private:
    io_context& io_;
    executor_guard guard_;
    std::atomic<bool> stopped_{false};
};

} // namespace server::core::net
