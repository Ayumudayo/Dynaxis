#pragma once

#include <atomic>
#include <functional>
#include <memory>

#include <boost/asio.hpp>

#include "server/core/net/connection.hpp"
#include "server/core/net/hive.hpp"

namespace server::core::net {

class Listener : public std::enable_shared_from_this<Listener> {
public:
    using connection_factory = std::function<std::shared_ptr<Connection>(std::shared_ptr<Hive>)>;

    Listener(std::shared_ptr<Hive> hive,
             const boost::asio::ip::tcp::endpoint& endpoint,
             connection_factory factory);
    virtual ~Listener();

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    void start();
    void stop();
    bool is_stopped() const;

protected:
    virtual void on_accept(std::shared_ptr<Connection> connection);
    virtual void on_error(const boost::system::error_code& ec);

private:
    void do_accept();

    std::shared_ptr<Hive> hive_;
    boost::asio::ip::tcp::acceptor acceptor_;
    connection_factory factory_;
    std::atomic<bool> stopped_{false};
};

} // namespace server::core::net
