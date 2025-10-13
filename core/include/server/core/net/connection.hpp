#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include <boost/asio.hpp>

#include "server/core/net/hive.hpp"

namespace server::core::net {

class Connection : public std::enable_shared_from_this<Connection> {
public:
    using socket_type = boost::asio::ip::tcp::socket;

    explicit Connection(std::shared_ptr<Hive> hive);
    virtual ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    socket_type& socket();

    void start();
    void stop();
    bool is_stopped() const;

    void async_send(const std::vector<std::uint8_t>& data);

protected:
    virtual void on_connect();
    virtual void on_disconnect();
    virtual void on_read(const std::uint8_t* data, std::size_t length);
    virtual void on_write(std::size_t length);
    virtual void on_error(const boost::system::error_code& ec);

    boost::asio::io_context& io();

private:
    void read_loop();
    void handle_read(const boost::system::error_code& ec, std::size_t bytes_transferred);
    void handle_write(const boost::system::error_code& ec, std::size_t bytes_transferred);
    void do_write();

    std::shared_ptr<Hive> hive_;
    socket_type socket_;
    std::array<std::uint8_t, 4096> read_buffer_{};
    std::deque<std::vector<std::uint8_t>> write_queue_;
    std::atomic<bool> stopped_{false};
};

} // namespace server::core::net
