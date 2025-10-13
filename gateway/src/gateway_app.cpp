#include "gateway/gateway_app.hpp"

#include <chrono>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <grpcpp/grpcpp.h>

#include "gateway_lb.grpc.pb.h"
#include "server/core/util/log.hpp"

namespace gateway {

namespace {
constexpr std::uint16_t kSmokeTestPort = 0; // let the OS choose an available port
const std::chrono::milliseconds kClientDelay{5};
}

using tcp = boost::asio::ip::tcp;

GatewayApp::GatewayApp()
    : hive_(std::make_shared<server::core::net::Hive>(io_))
    , stop_timer_(io_)
    , authenticator_(std::make_shared<auth::NoopAuthenticator>()) {
    if (const char* lb_env = std::getenv("LB_GRPC_ENDPOINT")) {
        if (std::string_view(lb_env).length() > 0) {
            lb_endpoint_ = lb_env;
            server::core::log::info("GatewayApp configured LB gRPC endpoint: " + lb_endpoint_);
        }
    }
}

bool GatewayApp::run_smoke_test() {
    payload_received_.store(false, std::memory_order_relaxed);
    watchdog_fired_.store(false, std::memory_order_relaxed);
    lb_forward_ok_.store(false, std::memory_order_relaxed);
    last_payload_.clear();

    const auto port = setup_listener(kSmokeTestPort);
    arm_stop_timer();

    std::thread worker([this]() { hive_->run(); });

    std::this_thread::sleep_for(kClientDelay);

    boost::asio::io_context client_io;
    tcp::socket client_socket(client_io);
    boost::system::error_code ec;
    tcp::endpoint destination(boost::asio::ip::address_v4::loopback(), port);
    client_socket.connect(destination, ec);
    if (!ec) {
        const std::string handshake = "client1:dummy-token";
        boost::asio::write(client_socket, boost::asio::buffer(handshake), ec);
        if (!ec) {
            std::this_thread::sleep_for(kClientDelay);
            const std::string payload = "chat:ping";
            boost::asio::write(client_socket, boost::asio::buffer(payload), ec);
        }
    }
    if (ec) {
        server::core::log::warn(std::string("GatewayApp smoke client error: ") + ec.message());
    }
    boost::system::error_code shutdown_ec;
    client_socket.shutdown(tcp::socket::shutdown_both, shutdown_ec);
    client_socket.close();

    worker.join();

    if (listener_) {
        listener_->stop();
        listener_.reset();
    }

    std::vector<std::uint8_t> payload_copy;
    {
        std::lock_guard<std::mutex> lock(payload_mutex_);
        payload_copy = last_payload_;
    }
    const std::string payload_str(payload_copy.begin(), payload_copy.end());
    const bool payload_ok = payload_received_.load(std::memory_order_relaxed) && payload_str == "chat:ping";
    const bool require_lb = !lb_endpoint_.empty() && (std::getenv("LB_GRPC_REQUIRED") != nullptr);
    const bool lb_ok = !require_lb || lb_forward_ok_.load(std::memory_order_relaxed);
    const bool ok = payload_ok && lb_ok && !watchdog_fired_.load(std::memory_order_relaxed);
    if (ok) {
        server::core::log::info("GatewayApp Hive/Connection smoke test completed");
    } else {
        server::core::log::warn("GatewayApp smoke test failed (payload_ok="
            + std::string(payload_ok ? "true" : "false") + ", lb_ok="
            + std::string(lb_ok ? "true" : "false") + ", watchdog="
            + std::string(watchdog_fired_.load(std::memory_order_relaxed) ? "true" : "false") + ")");
    }
    return ok;
}

void GatewayApp::arm_stop_timer() {
    using namespace std::chrono_literals;
    stop_timer_.expires_after(200ms);
    stop_timer_.async_wait([this](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted) {
            return;
        }
        if (ec) {
            server::core::log::warn(std::string("GatewayApp timer error: ") + ec.message());
        }
        watchdog_fired_.store(true, std::memory_order_relaxed);
        hive_->stop();
    });
}

std::uint16_t GatewayApp::setup_listener(std::uint16_t port) {
    auto payload_callback = [this](std::vector<std::uint8_t> payload) {
        payload_received_.store(true, std::memory_order_relaxed);
        std::string payload_str(payload.begin(), payload.end());
        {
            std::lock_guard<std::mutex> lock(payload_mutex_);
            last_payload_ = payload;
        }
        bool forwarded = forward_to_load_balancer(payload_str);
        lb_forward_ok_.store(forwarded, std::memory_order_relaxed);
        stop_timer_.cancel();
        hive_->stop();
    };

    auto authenticator = authenticator_;
    tcp::endpoint endpoint{tcp::v4(), port};
    listener_ = std::make_shared<server::core::net::Listener>(
        hive_,
        endpoint,
        [payload_callback, authenticator](std::shared_ptr<server::core::net::Hive> hive) {
            return std::make_shared<GatewayConnection>(std::move(hive), authenticator, payload_callback);
        });
    listener_->start();

    auto bound_endpoint = listener_->local_endpoint();
    server::core::log::info("GatewayApp listener bound to port " + std::to_string(bound_endpoint.port()));
    return bound_endpoint.port();
}

bool GatewayApp::forward_to_load_balancer(const std::string& payload) {
    if (lb_endpoint_.empty()) {
        return true;
    }

    auto channel = grpc::CreateChannel(lb_endpoint_, grpc::InsecureChannelCredentials());
    auto stub = gateway::lb::LoadBalancerService::NewStub(channel);

    gateway::lb::RouteRequest request;
    request.set_session_id("session-smoke");
    request.set_gateway_id("gateway-smoke");
    request.set_payload(payload);

    grpc::ClientContext context;
    gateway::lb::RouteResponse response;
    auto status = stub->Forward(&context, request, &response);
    if (!status.ok()) {
        server::core::log::warn(std::string("GatewayApp gRPC forward failed: ") + status.error_message());
        return false;
    }
    if (!response.accepted()) {
        server::core::log::warn(std::string("GatewayApp gRPC forward rejected: ") + response.reason());
        return false;
    }
    return true;
}

} // namespace gateway
