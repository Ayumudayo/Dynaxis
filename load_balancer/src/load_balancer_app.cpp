#include "load_balancer/load_balancer_app.hpp"

#include <chrono>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>

#include "server/core/util/log.hpp"
#include "server/storage/redis/client.hpp"

namespace load_balancer {

using namespace std::chrono_literals;

namespace {
constexpr std::chrono::seconds kRedisTtl{30};
constexpr std::chrono::milliseconds kHeartbeatDelay{5};
constexpr const char* kDefaultGrpcListen = "127.0.0.1:7001";
}

LoadBalancerApp::GrpcServiceImpl::GrpcServiceImpl(std::atomic<bool>& forward_flag)
    : forward_flag_(forward_flag) {}

grpc::Status LoadBalancerApp::GrpcServiceImpl::Forward(
    grpc::ServerContext*, const gateway::lb::RouteRequest* request,
    gateway::lb::RouteResponse* response) {
    server::core::log::info("LoadBalancerApp gRPC Forward session=" + request->session_id());
    forward_flag_.store(true, std::memory_order_relaxed);
    response->set_accepted(true);
    return grpc::Status::OK;
}

LoadBalancerApp::LoadBalancerApp()
    : hive_(std::make_shared<server::core::net::Hive>(io_))
    , heartbeat_timer_(io_)
    , state_backend_(create_backend()) {
    if (const char* listen_env = std::getenv("LB_GRPC_LISTEN")) {
        if (std::string_view(listen_env).length() > 0) {
            grpc_listen_address_ = listen_env;
        }
    }
    if (grpc_listen_address_.empty()) {
        grpc_listen_address_ = kDefaultGrpcListen;
    }
}

bool LoadBalancerApp::run_smoke_test() {
    heartbeat_executed_.store(false, std::memory_order_relaxed);
    grpc_forward_called_.store(false, std::memory_order_relaxed);
    start_grpc_server();
    schedule_heartbeat();

    std::thread worker([this]() { hive_->run(); });
    worker.join();

    stop_grpc_server();

    auto records = state_backend_->list_instances();
    const bool wrote_state = !records.empty() && records.front().instance_id == "lb-smoke";
    const bool ok = heartbeat_executed_.load(std::memory_order_relaxed) && wrote_state;
    if (ok) {
        server::core::log::info("LoadBalancerApp state backend smoke test completed");
    } else {
        server::core::log::warn("LoadBalancerApp smoke test failed to record instance state");
    }
    return ok;
}

void LoadBalancerApp::schedule_heartbeat() {
    heartbeat_timer_.expires_after(kHeartbeatDelay);
    heartbeat_timer_.async_wait([this](const boost::system::error_code& ec) {
        if (ec) {
            server::core::log::warn(std::string("LoadBalancerApp heartbeat timer error: ") + ec.message());
            hive_->stop();
            return;
        }

        auto record = build_smoke_record();
        state_backend_->upsert(record);
        state_backend_->touch(record.instance_id, record.last_heartbeat_ms);
        heartbeat_executed_.store(true, std::memory_order_relaxed);
        hive_->stop();
    });
}

std::unique_ptr<server::state::IInstanceStateBackend> LoadBalancerApp::create_backend() {
    const char* uri = std::getenv("LB_REDIS_URI");
    if (!uri || std::string_view(uri).empty()) {
        uri = std::getenv("REDIS_URI");
    }

    if (uri && std::string_view(uri).length() > 0) {
        try {
            server::storage::redis::Options opts;
            auto client = server::storage::redis::make_redis_client(uri, opts);
            if (client) {
                auto state_client = server::state::make_redis_state_client(client);
                server::core::log::info("LoadBalancerApp using Redis state backend");
                return std::make_unique<server::state::RedisInstanceStateBackend>(
                    state_client,
                    "gateway/instances",
                    kRedisTtl);
            }
        } catch (const std::exception& ex) {
            server::core::log::warn(std::string("LoadBalancerApp Redis backend init failed: ") + ex.what());
        }
    }

    server::core::log::warn("LoadBalancerApp falling back to in-memory state backend");
    return std::make_unique<server::state::InMemoryStateBackend>();
}

server::state::InstanceRecord LoadBalancerApp::build_smoke_record() const {
    server::state::InstanceRecord record{};
    record.instance_id = "lb-smoke";
    record.host = "127.0.0.1";
    record.port = 6100;
    record.role = "load_balancer";
    record.capacity = 1;
    record.active_sessions = 0;
    record.last_heartbeat_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    return record;
}

void LoadBalancerApp::start_grpc_server() {
    grpc_service_ = std::make_unique<GrpcServiceImpl>(grpc_forward_called_);
    grpc::ServerBuilder builder;
    builder.AddListeningPort(grpc_listen_address_, grpc::InsecureServerCredentials(), &grpc_selected_port_);
    builder.RegisterService(grpc_service_.get());
    grpc_server_ = builder.BuildAndStart();
    if (!grpc_server_) {
        server::core::log::warn("LoadBalancerApp failed to start gRPC server at " + grpc_listen_address_);
        grpc_service_.reset();
        return;
    }
    server::core::log::info("LoadBalancerApp gRPC listening on " + grpc_listen_address_);
    grpc_thread_ = std::thread([this]() { grpc_server_->Wait(); });
}

void LoadBalancerApp::stop_grpc_server() {
    if (!grpc_server_) {
        return;
    }
    grpc_server_->Shutdown();
    if (grpc_thread_.joinable()) {
        grpc_thread_.join();
    }
    grpc_server_.reset();
    grpc_service_.reset();
}

} // namespace load_balancer
