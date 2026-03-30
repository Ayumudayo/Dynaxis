/**
 * @file gateway_tcp_listener.cpp
 * @brief GatewayApp TCP listener bootstrap 구현입니다.
 */
#include "gateway/gateway_app.hpp"

#include <stdexcept>

#include <boost/asio/ip/tcp.hpp>

#include "gateway/gateway_connection.hpp"
#include "gateway_app_access.hpp"
#include "gateway_app_state.hpp"
#include "server/core/net/listener.hpp"
#include "server/core/util/log.hpp"

namespace gateway {

void GatewayAppAccess::start_listener(GatewayApp& app) {
    auto* impl_ = app.impl_.get();
    using tcp = boost::asio::ip::tcp;

    boost::system::error_code ec;
    boost::asio::ip::address address = boost::asio::ip::address_v4::any();
    if (!impl_->listen_host_.empty()) {
        const auto parsed = boost::asio::ip::make_address(impl_->listen_host_, ec);
        if (!ec) {
            address = parsed;
        } else {
            server::core::log::warn("GatewayApp failed to parse listen address; defaulting to 0.0.0.0");
        }
    }

    tcp::endpoint endpoint{address, impl_->listen_port_};
    impl_->listener_ = std::make_shared<server::core::net::TransportListener>(
        impl_->hive_,
        endpoint,
        [authenticator = impl_->authenticator_, &app](std::shared_ptr<server::core::net::Hive> hive) {
            return std::make_shared<GatewayConnection>(std::move(hive), authenticator, app);
        });

    if (impl_->listener_->is_stopped()) {
        throw std::runtime_error("GatewayApp listener failed to start");
    }

    impl_->listener_->start();
    const auto bound = impl_->listener_->local_endpoint();
    server::core::log::info("GatewayApp listening on " + bound.address().to_string() + ":" + std::to_string(bound.port()));
}

} // namespace gateway
