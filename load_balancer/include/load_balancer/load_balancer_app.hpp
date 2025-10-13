#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <grpcpp/grpcpp.h>

#include "gateway_lb.grpc.pb.h"
#include "server/core/net/hive.hpp"
#include "server/state/instance_registry.hpp"

namespace load_balancer {

class LoadBalancerApp {
public:
    LoadBalancerApp();
    ~LoadBalancerApp() = default;

    bool run_smoke_test();

    class GrpcServiceImpl final : public gateway::lb::LoadBalancerService::Service {
    public:
        explicit GrpcServiceImpl(std::atomic<bool>& forward_flag);
        grpc::Status Forward(grpc::ServerContext*, const gateway::lb::RouteRequest*,
                             gateway::lb::RouteResponse*) override;
    private:
        std::atomic<bool>& forward_flag_;
    };
private:

    void schedule_heartbeat();
    std::unique_ptr<server::state::IInstanceStateBackend> create_backend();
    server::state::InstanceRecord build_smoke_record() const;
    void start_grpc_server();
    void stop_grpc_server();

    boost::asio::io_context io_;
    std::shared_ptr<server::core::net::Hive> hive_;
    boost::asio::steady_timer heartbeat_timer_;
    std::unique_ptr<server::state::IInstanceStateBackend> state_backend_;

    std::string grpc_listen_address_;
    std::unique_ptr<GrpcServiceImpl> grpc_service_;
    std::unique_ptr<grpc::Server> grpc_server_;
    std::thread grpc_thread_;
    std::atomic<bool> grpc_forward_called_{false};
    int grpc_selected_port_{0};

    std::atomic<bool> heartbeat_executed_{false};
};

} // namespace load_balancer
