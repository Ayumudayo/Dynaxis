#include "load_balancer/load_balancer_app.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <functional>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <utility>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>

#include "load_balancer/session_directory.hpp"
#include "server/core/util/log.hpp"
#include "server/core/util/paths.hpp"
#include "server/storage/redis/client.hpp"

namespace load_balancer {

// LoadBalancerApp는 gRPC Stream을 통해 Gateway와 통신하고, 각 스트림마다 TCP backend 세션을
// 생성해 바이트를 그대로 브릿지한다. Redis 기반 session_directory가 활성화되면 클라이언트별로
// backend를 sticky하게 유지하고, health/idle 정책은 `mark_backend_*` 및 idle_watcher가 담당한다.

namespace {

using namespace std::chrono_literals;

constexpr const char* kEnvGrpcListen = "LB_GRPC_LISTEN";
constexpr const char* kEnvBackendEndpoints = "LB_BACKEND_ENDPOINTS";
constexpr const char* kEnvRedisUri = "LB_REDIS_URI";
constexpr const char* kEnvInstanceId = "LB_INSTANCE_ID";
constexpr const char* kEnvDynamicBackends = "LB_DYNAMIC_BACKENDS";
constexpr const char* kEnvBackendRefreshInterval = "LB_BACKEND_REFRESH_INTERVAL";
constexpr const char* kEnvBackendRegistryPrefix = "LB_BACKEND_REGISTRY_PREFIX";
constexpr const char* kEnvBackendIdleTimeout = "LB_BACKEND_IDLE_TIMEOUT";
constexpr const char* kDefaultGrpcListen = "127.0.0.1:7001";
constexpr const char* kDefaultBackendEndpoint = "127.0.0.1:5000";

// LB_GRPC_LISTEN / LB_BACKEND_ENDPOINTS는 host 또는 host:port 둘 다 허용하므로,
// 포트가 생략되면 fallback을 사용해 구성 실수를 방지한다.
std::pair<std::string, std::uint16_t> parse_endpoint(std::string_view value, std::uint16_t fallback_port) {
    if (value.empty()) {
        return {"127.0.0.1", fallback_port};
    }
    auto delim = value.find(':');
    if (delim == std::string_view::npos) {
        return {std::string(value), fallback_port};
    }
    std::string host(value.substr(0, delim));
    std::string_view port_view = value.substr(delim + 1);
    std::uint16_t port = fallback_port;
    if (!port_view.empty()) {
        try {
            port = static_cast<std::uint16_t>(std::stoul(std::string(port_view)));
        } catch (...) {
            server::core::log::warn("LoadBalancerApp invalid port detected; fallback applied");
            port = fallback_port;
        }
    }
    return {std::move(host), port};
}

std::uint64_t to_ms(std::chrono::steady_clock::time_point tp) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count());
}

} // namespace

LoadBalancerApp::GrpcServiceImpl::GrpcServiceImpl(LoadBalancerApp& owner)
    : owner_(owner) {}

grpc::Status LoadBalancerApp::GrpcServiceImpl::Forward(
    grpc::ServerContext*, const gateway::lb::RouteRequest*, gateway::lb::RouteResponse* response) {
    response->set_accepted(false);
    response->set_reason("Use Stream RPC");
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Stream RPC required");
}

grpc::Status LoadBalancerApp::GrpcServiceImpl::Stream(
    grpc::ServerContext* context,
    grpc::ServerReaderWriter<gateway::lb::RouteMessage, gateway::lb::RouteMessage>* stream) {
    return owner_.handle_stream(context, stream);
}

LoadBalancerApp::LoadBalancerApp()
    : hive_(std::make_shared<server::core::net::Hive>(io_))
    , heartbeat_timer_(io_)
    , signals_(io_)
    , backend_refresh_timer_(io_) {
    state_backend_ = create_backend();
    configure();
}

LoadBalancerApp::~LoadBalancerApp() {
    stop();
}

int LoadBalancerApp::run() {
    start_grpc_server();
    schedule_heartbeat();
    handle_signals();

    server::core::log::info("LoadBalancerApp entering run loop");
    hive_->run();
    server::core::log::info("LoadBalancerApp stopped");

    stop_grpc_server();
    return 0;
}

void LoadBalancerApp::stop() {
    heartbeat_timer_.cancel();
    backend_refresh_timer_.cancel();
    dynamic_backends_active_ = false;
    stop_grpc_server();
    if (hive_) {
        hive_->stop();
    }
    io_.stop();
}

void LoadBalancerApp::configure() {
    const char* listen_env = std::getenv(kEnvGrpcListen);
    if (listen_env && *listen_env) {
        grpc_listen_address_ = listen_env;
    } else {
        grpc_listen_address_ = kDefaultGrpcListen;
    }

    const char* instance_env = std::getenv(kEnvInstanceId);
    if (instance_env && *instance_env) {
        instance_id_ = instance_env;
    } else {
        instance_id_ = "lb-" + std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    if (const char* prefix_env = std::getenv(kEnvBackendRegistryPrefix); prefix_env && *prefix_env) {
        backend_registry_prefix_ = prefix_env;
    }
    if (!backend_registry_prefix_.empty() && backend_registry_prefix_.back() != '/') {
        backend_registry_prefix_.push_back('/');
    }

    const char* backend_env = std::getenv(kEnvBackendEndpoints);
    if (backend_env && *backend_env) {
        configure_backends(backend_env);
    } else {
        configure_backends(kDefaultBackendEndpoint);
    }

    if (static_backends_.empty()) {
        configure_backends(kDefaultBackendEndpoint);
    }

    if (const char* ttl_env = std::getenv("LB_SESSION_TTL"); ttl_env && *ttl_env) {
        try {
            auto parsed = std::stoul(ttl_env);
            if (parsed > 0) {
                session_binding_ttl_ = std::chrono::seconds{static_cast<long long>(parsed)};
            }
        } catch (const std::exception& ex) {
            server::core::log::warn(std::string("LoadBalancerApp invalid LB_SESSION_TTL: ") + ex.what());
        }
    }

    if (const char* threshold_env = std::getenv("LB_BACKEND_FAILURE_THRESHOLD"); threshold_env && *threshold_env) {
        try {
            auto parsed = std::stoul(threshold_env);
            if (parsed > 0) {
                backend_failure_threshold_ = parsed;
            }
        } catch (const std::exception& ex) {
            server::core::log::warn(std::string("LoadBalancerApp invalid LB_BACKEND_FAILURE_THRESHOLD: ") + ex.what());
        }
    }

    if (const char* cooldown_env = std::getenv("LB_BACKEND_COOLDOWN"); cooldown_env && *cooldown_env) {
        try {
            auto parsed = std::stoul(cooldown_env);
            if (parsed > 0) {
                backend_retry_cooldown_ = std::chrono::seconds{static_cast<long long>(parsed)};
            }
        } catch (const std::exception& ex) {
            server::core::log::warn(std::string("LoadBalancerApp invalid LB_BACKEND_COOLDOWN: ") + ex.what());
        }
    }

    if (const char* refresh_env = std::getenv(kEnvBackendRefreshInterval); refresh_env && *refresh_env) {
        try {
            auto parsed = std::stoul(refresh_env);
            if (parsed > 0) {
                backend_refresh_interval_ = std::chrono::seconds{static_cast<long long>(parsed)};
            }
        } catch (const std::exception& ex) {
            server::core::log::warn(std::string("LoadBalancerApp invalid LB_BACKEND_REFRESH_INTERVAL: ") + ex.what());
        }
    }

    if (const char* idle_env = std::getenv(kEnvBackendIdleTimeout); idle_env && *idle_env) {
        try {
            auto parsed = std::stoul(idle_env);
            if (parsed >= 5 && parsed <= 3600) {
                backend_idle_timeout_ = std::chrono::seconds{static_cast<long long>(parsed)};
            } else {
                server::core::log::warn("LoadBalancerApp LB_BACKEND_IDLE_TIMEOUT out of range (5~3600)");
            }
        } catch (const std::exception& ex) {
            server::core::log::warn(std::string("LoadBalancerApp invalid LB_BACKEND_IDLE_TIMEOUT: ") + ex.what());
        }
    }
    if (backend_idle_timeout_ <= std::chrono::seconds::zero()) {
        backend_idle_timeout_ = std::chrono::seconds{30};
    }

    const bool dynamic_requested = [&]() {
        if (const char* dynamic_env = std::getenv(kEnvDynamicBackends); dynamic_env && *dynamic_env) {
            return std::strcmp(dynamic_env, "0") != 0;
        }
        return false;
    }();

    if (redis_client_) {
        session_directory_ = std::make_unique<SessionDirectory>(redis_client_, "gateway/session", session_binding_ttl_);
    } else {
        session_directory_.reset();
    }

    auto initial_count = set_backends(static_backends_);

    if (dynamic_requested && state_backend_ && redis_client_) {
        dynamic_backends_active_ = true;
        refresh_backends();
    } else {
        if (dynamic_requested && (!state_backend_ || !redis_client_)) {
            server::core::log::warn("LoadBalancerApp dynamic backends requested but registry backend unavailable; using static configuration");
        }
        dynamic_backends_active_ = false;
        backend_refresh_timer_.cancel();
    }

    std::size_t active_backends = backends_.size();
    server::core::log::info("LoadBalancerApp grpc_listen=" + grpc_listen_address_
        + " instance_id=" + instance_id_
        + " backends=" + std::to_string(active_backends > 0 ? active_backends : initial_count)
        + " dynamic_backends=" + std::string(dynamic_backends_active_ ? "1" : "0")
        + " idle_timeout=" + std::to_string(backend_idle_timeout_.count()) + "s");
}

void LoadBalancerApp::configure_backends(std::string_view list) {
    std::vector<BackendEndpoint> parsed;
    std::size_t index = 0;
    std::size_t start = 0;
    while (start < list.size()) {
        auto end = list.find(',', start);
        if (end == std::string_view::npos) {
            end = list.size();
        }
        auto token = list.substr(start, end - start);
        if (!token.empty()) {
            auto [host, port] = parse_endpoint(token, 5000);
            BackendEndpoint endpoint{};
            endpoint.id = "backend-" + std::to_string(index++);
            endpoint.host = std::move(host);
            endpoint.port = port;
            parsed.push_back(std::move(endpoint));
        }
        start = end + 1;
    }
    static_backends_ = std::move(parsed);
}

std::size_t LoadBalancerApp::set_backends(std::vector<BackendEndpoint> backends) {
    std::unordered_map<std::string, std::size_t> new_index;
    for (std::size_t i = 0; i < backends.size(); ++i) {
        if (backends[i].id.empty()) {
            backends[i].id = "backend-" + std::to_string(i);
        }
        new_index[backends[i].id] = i;
    }

    std::size_t count = backends.size();
    {
        std::lock_guard<std::mutex> lock(hash_mutex_);
        backends_ = std::move(backends);
        backend_index_map_ = std::move(new_index);
    }

    rebuild_hash_ring();

    {
        std::lock_guard<std::mutex> lock(health_mutex_);
        for (auto it = backend_health_.begin(); it != backend_health_.end();) {
            if (backend_index_map_.find(it->first) == backend_index_map_.end()) {
                it = backend_health_.erase(it);
            } else {
                ++it;
            }
        }
    }

    backend_index_.store(0, std::memory_order_relaxed);
    return count;
}

void LoadBalancerApp::schedule_backend_refresh() {
    if (!dynamic_backends_active_) {
        return;
    }
    backend_refresh_timer_.expires_after(backend_refresh_interval_);
    backend_refresh_timer_.async_wait([this](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted) {
            return;
        }
        if (ec) {
            server::core::log::warn(std::string("LoadBalancerApp backend refresh timer error: ") + ec.message());
            schedule_backend_refresh();
            return;
        }
        refresh_backends();
    });
}

void LoadBalancerApp::refresh_backends() {
    if (!dynamic_backends_active_) {
        return;
    }

    std::vector<BackendEndpoint> dynamic_backends;
    if (state_backend_) {
        try {
            auto records = state_backend_->list_instances();
            dynamic_backends = make_backends_from_records(records);
        } catch (const std::exception& ex) {
            server::core::log::warn(std::string("LoadBalancerApp failed to fetch backend registry: ") + ex.what());
        }
    }

    bool applied = false;
    if (!dynamic_backends.empty()) {
        applied = apply_backend_snapshot(std::move(dynamic_backends), "registry");
    } else if (!static_backends_.empty()) {
        applied = apply_backend_snapshot(static_backends_, "static");
        if (!applied && backends_.empty()) {
            set_backends(static_backends_);
            applied = true;
        }
    }

    if (!applied && backends_.empty()) {
        server::core::log::warn("LoadBalancerApp has no backend endpoints available");
    }

    if (dynamic_backends_active_) {
        schedule_backend_refresh();
    }
}

bool LoadBalancerApp::apply_backend_snapshot(std::vector<BackendEndpoint> candidates, std::string_view source) {
    if (candidates.empty()) {
        return false;
    }
    if (backends_equal(backends_, candidates)) {
        return false;
    }
    auto count = set_backends(std::move(candidates));
    server::core::log::info("LoadBalancerApp applied backend snapshot via " + std::string(source)
        + " count=" + std::to_string(count));
    return true;
}

std::vector<LoadBalancerApp::BackendEndpoint>
LoadBalancerApp::make_backends_from_records(const std::vector<server::state::InstanceRecord>& records) const {
    std::vector<BackendEndpoint> result;
    result.reserve(records.size());
    for (const auto& record : records) {
        if (record.instance_id == instance_id_) {
            continue;
        }
        if (!record.role.empty()) {
            if (record.role != "server" && record.role != "backend" && record.role != "game_server") {
                continue;
            }
        }
        BackendEndpoint endpoint{};
        endpoint.id = record.instance_id.empty()
            ? (record.host.empty() ? std::string("backend-") + std::to_string(result.size()) : record.host + ":" + std::to_string(record.port))
            : record.instance_id;
        endpoint.host = record.host.empty() ? "127.0.0.1" : record.host;
        endpoint.port = record.port == 0 ? 5000 : record.port;
        result.push_back(std::move(endpoint));
    }
    return result;
}

bool LoadBalancerApp::backends_equal(const std::vector<BackendEndpoint>& lhs,
                                     const std::vector<BackendEndpoint>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    std::unordered_map<std::string, std::pair<std::string, std::uint16_t>> map;
    map.reserve(lhs.size());
    for (const auto& ep : lhs) {
        map.emplace(ep.id, std::make_pair(ep.host, ep.port));
    }
    for (const auto& ep : rhs) {
        auto it = map.find(ep.id);
        if (it == map.end()) {
            return false;
        }
        if (it->second.first != ep.host || it->second.second != ep.port) {
            return false;
        }
    }
    return true;
}

void LoadBalancerApp::schedule_heartbeat() {
    heartbeat_timer_.expires_after(heartbeat_interval_);
    heartbeat_timer_.async_wait([this](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted) {
            return;
        }
        if (ec) {
            server::core::log::warn(std::string("LoadBalancerApp heartbeat timer error: ") + ec.message());
        } else {
            publish_heartbeat();
        }
        schedule_heartbeat();
    });
}

void LoadBalancerApp::publish_heartbeat() {
    if (!state_backend_) {
        return;
    }
    server::state::InstanceRecord record{};
    record.instance_id = instance_id_;
    auto endpoint = parse_endpoint(grpc_listen_address_, 7001);
    if (grpc_selected_port_ > 0) {
        endpoint.second = static_cast<std::uint16_t>(grpc_selected_port_);
    }
    record.host = std::move(endpoint.first);
    record.port = endpoint.second;
    record.role = "load_balancer";
    record.capacity = static_cast<std::uint32_t>(backends_.size());
    record.active_sessions = 0;
    record.last_heartbeat_ms = to_ms(std::chrono::steady_clock::now());

    if (!state_backend_->upsert(record)) {
        server::core::log::warn("LoadBalancerApp heartbeat upsert failed");
    }
}

std::unique_ptr<server::state::IInstanceStateBackend> LoadBalancerApp::create_backend() {
    const char* uri = std::getenv(kEnvRedisUri);
    if (!uri || std::string_view(uri).empty()) {
        uri = std::getenv("REDIS_URI");
    }

    std::string registry_prefix = backend_registry_prefix_;
    if (const char* prefix_env = std::getenv(kEnvBackendRegistryPrefix); prefix_env && *prefix_env) {
        registry_prefix = prefix_env;
    }
    if (!registry_prefix.empty() && registry_prefix.back() != '/') {
        registry_prefix.push_back('/');
    }

    if (uri && std::string_view(uri).length() > 0) {
        try {
            server::storage::redis::Options opts;
            auto client = server::storage::redis::make_redis_client(uri, opts);
            if (client) {
                redis_client_ = client;
                auto state_client = server::state::make_redis_state_client(client);
                server::core::log::info("LoadBalancerApp using Redis state backend");
                 backend_registry_prefix_ = registry_prefix;
                return std::make_unique<server::state::RedisInstanceStateBackend>(
                    state_client,
                    registry_prefix,
                    backend_state_ttl_);
            }
        } catch (const std::exception& ex) {
            redis_client_.reset();
            server::core::log::warn(std::string("LoadBalancerApp Redis backend init failed: ") + ex.what());
        }
    }

    server::core::log::warn("LoadBalancerApp falling back to in-memory state backend");
    redis_client_.reset();
    backend_registry_prefix_ = registry_prefix;
    return std::make_unique<server::state::InMemoryStateBackend>();
}

void LoadBalancerApp::rebuild_hash_ring() {
    std::lock_guard<std::mutex> lock(hash_mutex_);
    hash_ring_.clear();
    if (backends_.empty()) {
        return;
    }
    constexpr int kReplicas = 128;
    for (std::size_t i = 0; i < backends_.size(); ++i) {
        const auto& backend = backends_[i];
        for (int replica = 0; replica < kReplicas; ++replica) {
            std::string key = backend.id + "#" + std::to_string(replica);
            auto hash = static_cast<std::uint32_t>(std::hash<std::string>{}(key));
            hash_ring_.emplace(hash, i);
        }
    }
}

bool LoadBalancerApp::is_backend_available(const BackendEndpoint& endpoint, std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(health_mutex_);
    auto it = backend_health_.find(endpoint.id);
    if (it == backend_health_.end()) {
        return true;
    }
    if (it->second.retry_at != std::chrono::steady_clock::time_point{} && now >= it->second.retry_at) {
        it->second.retry_at = std::chrono::steady_clock::time_point{};
        it->second.failure_count = 0;
        return true;
    }
    if (it->second.retry_at != std::chrono::steady_clock::time_point{} && now < it->second.retry_at) {
        return false;
    }
    return true;
}

// backend 호출이 성공하면 실패 카운터를 리셋해 health window를 갱신한다.
void LoadBalancerApp::mark_backend_success(const std::string& backend_id) {
    if (backend_id.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(health_mutex_);
    auto& health = backend_health_[backend_id];
    health.failure_count = 0;
    health.retry_at = std::chrono::steady_clock::time_point{};
}

// 실패 카운터가 임계치를 넘으면 backend_retry_cooldown 이후에만 후속 요청을 받게 한다.
void LoadBalancerApp::mark_backend_failure(const std::string& backend_id) {
    if (backend_id.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(health_mutex_);
    auto& health = backend_health_[backend_id];
    health.failure_count += 1;
    if (health.failure_count >= static_cast<int>(backend_failure_threshold_)) {
        health.retry_at = std::chrono::steady_clock::now() + backend_retry_cooldown_;
    }
}

std::optional<LoadBalancerApp::BackendEndpoint> LoadBalancerApp::find_backend_by_id(const std::string& backend_id) const {
    if (backend_id.empty()) {
        return std::nullopt;
    }
    auto it = backend_index_map_.find(backend_id);
    if (it == backend_index_map_.end()) {
        return std::nullopt;
    }
    if (it->second >= backends_.size()) {
        return std::nullopt;
    }
    return backends_[it->second];
}

// 클라이언트 ID가 있으면 consistent hash ring으로 sticky routing을 시도하고,
// 없으면 round-robin + health 상태로 backend를 고른다.
std::optional<LoadBalancerApp::BackendEndpoint> LoadBalancerApp::select_backend(const std::string& client_id) {
    if (backends_.empty()) {
        return std::nullopt;
    }

    auto now = std::chrono::steady_clock::now();

    if (!client_id.empty()) {
        std::lock_guard<std::mutex> lock(hash_mutex_);
        if (!hash_ring_.empty()) {
            auto hash = static_cast<std::uint32_t>(std::hash<std::string>{}(client_id));
            auto it = hash_ring_.lower_bound(hash);
            std::size_t inspected = 0;
            if (it == hash_ring_.end()) {
                it = hash_ring_.begin();
            }
            while (inspected < backends_.size() && !hash_ring_.empty()) {
                if (it == hash_ring_.end()) {
                    it = hash_ring_.begin();
                }
                auto idx = it->second;
                if (idx < backends_.size()) {
                    const auto& candidate = backends_[idx];
                    if (is_backend_available(candidate, now)) {
                        return candidate;
                    }
                }
                ++inspected;
                ++it;
            }
        }
    }

    for (std::size_t attempt = 0; attempt < backends_.size(); ++attempt) {
        auto idx = backend_index_.fetch_add(1, std::memory_order_relaxed);
        const auto& candidate = backends_[idx % backends_.size()];
        if (is_backend_available(candidate, now)) {
            return candidate;
        }
    }

    return backends_.front();
}

// gateway 당 gRPC 스트림은 단일 backend와의 TCP 터널을 생성하므로 동기 connect로 충분하다.
bool LoadBalancerApp::connect_backend(const BackendEndpoint& endpoint,
                                      boost::asio::ip::tcp::socket& socket,
                                      std::string& error) const {
    boost::asio::ip::tcp::resolver resolver(socket.get_executor());
    boost::system::error_code ec;
    auto results = resolver.resolve(endpoint.host, std::to_string(endpoint.port), ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    boost::asio::connect(socket, results, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    return true;
}

grpc::Status LoadBalancerApp::handle_stream(
    grpc::ServerContext*,
    grpc::ServerReaderWriter<gateway::lb::RouteMessage, gateway::lb::RouteMessage>* stream) {
    gateway::lb::RouteMessage request;
    if (!stream->Read(&request)) {
        return grpc::Status::OK;
    }

    auto session_id = request.session_id();
    auto gateway_id = request.gateway_id();
    auto client_id = request.client_id();

    auto backend_opt = select_backend(client_id);
    if (!backend_opt) {
        gateway::lb::RouteMessage error_msg;
        error_msg.set_kind(gateway::lb::ROUTE_KIND_SERVER_ERROR);
        error_msg.set_session_id(session_id);
        error_msg.set_gateway_id(gateway_id);
        error_msg.set_error("no backend available");
        stream->Write(error_msg);
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "no backend");
    }
    auto backend = *backend_opt;
    std::string assigned_backend_id = backend.id;

    std::atomic<bool> running{true};
    std::mutex write_mutex;
    bool session_bound = false;

    // sticky session을 유지하기 위해 session_directory에 backend 할당 상태를 기록/해제한다.
    auto release_session = [&]() {
        if (session_bound && session_directory_ && !client_id.empty()) {
            session_directory_->release_backend(client_id, assigned_backend_id);
            session_bound = false;
        }
    };

    auto now = std::chrono::steady_clock::now();
    // sticky 세션을 지원하기 위해 Redis session_directory에서 기존 backend를 찾는다.
    if (session_directory_ && !client_id.empty()) {
        // session_directory는 "client_id -> backend_id" 매핑을 Redis에 저장해
        // gateway 재연결 시에도 같은 backend를 선택하게 한다. backend가 죽었거나 health 체크에
        // 실패하면 release_backend를 호출해 매핑을 제거하고 새 backend를 배정한다.
        bool resolved = false;
        if (auto existing = session_directory_->find_backend(client_id)) {
            if (auto mapped = find_backend_by_id(*existing)) {
                if (is_backend_available(*mapped, now)) {
                    backend = *mapped;
                    assigned_backend_id = backend.id;
                    session_directory_->refresh_backend(client_id, assigned_backend_id);
                    resolved = true;
                } else {
                    session_directory_->release_backend(client_id, *existing);
                }
            } else {
                session_directory_->release_backend(client_id, *existing);
            }
        }
        if (!resolved) {
            if (auto resolved_id = session_directory_->ensure_backend(client_id, backend.id)) {
                bool bind_ok = false;
                if (auto mapped = find_backend_by_id(*resolved_id)) {
                    if (is_backend_available(*mapped, now)) {
                        backend = *mapped;
                        assigned_backend_id = backend.id;
                        bind_ok = true;
                    } else {
                        session_directory_->release_backend(client_id, *resolved_id);
                    }
                } else {
                    session_directory_->release_backend(client_id, *resolved_id);
                }
                if (!bind_ok) {
                    if (auto retry_id = session_directory_->ensure_backend(client_id, backend.id)) {
                        if (auto mapped_retry = find_backend_by_id(*retry_id)) {
                            if (is_backend_available(*mapped_retry, now)) {
                                backend = *mapped_retry;
                                assigned_backend_id = backend.id;
                                bind_ok = true;
                            } else {
                                session_directory_->release_backend(client_id, *retry_id);
                            }
                        } else {
                            session_directory_->release_backend(client_id, *retry_id);
                        }
                    }
                }
                resolved = bind_ok;
            }
        }
        session_bound = resolved;
    }

    boost::asio::io_context backend_io;
    boost::asio::ip::tcp::socket backend_socket(backend_io);
    std::atomic<std::chrono::steady_clock::time_point> last_backend_activity{std::chrono::steady_clock::now()};
    std::string connect_error;
    if (!connect_backend(backend, backend_socket, connect_error)) {
        mark_backend_failure(assigned_backend_id);
        gateway::lb::RouteMessage error_msg;
        error_msg.set_kind(gateway::lb::ROUTE_KIND_SERVER_ERROR);
        error_msg.set_session_id(session_id);
        error_msg.set_gateway_id(gateway_id);
        error_msg.set_backend_id(assigned_backend_id);
        error_msg.set_error(connect_error);
        stream->Write(error_msg);
        release_session();
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, connect_error);
    }

    mark_backend_success(assigned_backend_id);
    server::core::log::info("LoadBalancerApp routed session=" + session_id
        + " gateway=" + gateway_id + " client=" + client_id
        + " backend=" + backend.host + ":" + std::to_string(backend.port)
        + " sticky=" + (session_bound ? "1" : "0"));
    last_backend_activity.store(std::chrono::steady_clock::now(), std::memory_order_relaxed);

    // gRPC 스트림은 다중 쓰레드에서 write할 수 있으므로, 모든 송신을 mutex로 직렬화한다.
    auto send_to_gateway = [&](gateway::lb::RouteMessage message) {
        message.set_session_id(session_id);
        message.set_gateway_id(gateway_id);
        message.set_backend_id(assigned_backend_id);
        std::lock_guard<std::mutex> lock(write_mutex);
        if (!stream->Write(message)) {
            running.store(false, std::memory_order_relaxed);
            return false;
        }
        return true;
    };

    // gateway→backend 패킷은 TCP로 그대로 전달하되, 오류 시 LB가 metric/재시도 정책을 결정한다.
    auto forward_to_backend = [&](const std::string& payload) {
        boost::system::error_code write_ec;
        boost::asio::write(backend_socket, boost::asio::buffer(payload), write_ec);
        if (write_ec) {
            gateway::lb::RouteMessage error_msg;
            error_msg.set_kind(gateway::lb::ROUTE_KIND_SERVER_ERROR);
            error_msg.set_error(write_ec.message());
            mark_backend_failure(assigned_backend_id);
            send_to_gateway(std::move(error_msg));
            running.store(false, std::memory_order_relaxed);
            return false;
        }
        last_backend_activity.store(std::chrono::steady_clock::now(), std::memory_order_relaxed);
        return true;
    };

    // backend_reader는 backend→gateway 데이터 흐름을 중계하고, 소켓 종료 원인을 gateway에 알린다.
    std::thread backend_reader([&]() {
        std::array<std::uint8_t, 8192> buffer{};
        while (running.load(std::memory_order_relaxed)) {
            boost::system::error_code read_ec;
            std::size_t bytes = backend_socket.read_some(boost::asio::buffer(buffer), read_ec);
            if (read_ec) {
                if (running.load(std::memory_order_relaxed)) {
                    gateway::lb::RouteMessage close_msg;
                    if (read_ec == boost::asio::error::eof) {
                        close_msg.set_kind(gateway::lb::ROUTE_KIND_SERVER_CLOSE);
                    } else {
                        close_msg.set_kind(gateway::lb::ROUTE_KIND_SERVER_ERROR);
                        close_msg.set_error(read_ec.message());
                    }
                    send_to_gateway(std::move(close_msg));
                }
                running.store(false, std::memory_order_relaxed);
                break;
            }

            if (bytes > 0) {
                gateway::lb::RouteMessage payload_msg;
                payload_msg.set_kind(gateway::lb::ROUTE_KIND_SERVER_PAYLOAD);
                payload_msg.set_payload(reinterpret_cast<const char*>(buffer.data()), static_cast<int>(bytes));
                if (!send_to_gateway(std::move(payload_msg))) {
                    break;
                }
            }
        }
    });

    gateway::lb::RouteMessage response;
    while (running.load(std::memory_order_relaxed) && stream->Read(&response)) {
        switch (response.kind()) {
        case gateway::lb::ROUTE_KIND_CLIENT_PAYLOAD: {
            auto payload = response.payload();
            if (!forward_to_backend(payload)) {
                running.store(false, std::memory_order_relaxed);
            }
            break;
        }
        case gateway::lb::ROUTE_KIND_CLIENT_CLOSE:
            running.store(false, std::memory_order_relaxed);
            break;
        default:
            break;
        }
    }

    running.store(false, std::memory_order_relaxed);
    if (backend_socket.is_open()) {
        boost::system::error_code ignored;
        backend_socket.close(ignored);
    }
    if (backend_reader.joinable()) {
        backend_reader.join();
    }

    release_session();
    return grpc::Status::OK;
}

void LoadBalancerApp::start_grpc_server() {
    std::string server_address = grpc_listen_address_;
    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials(), &grpc_selected_port_);
    
    grpc_service_ = std::make_unique<GrpcServiceImpl>(*this);
    builder.RegisterService(grpc_service_.get());
    
    grpc_server_ = builder.BuildAndStart();
    server::core::log::info("LoadBalancerApp gRPC server listening on " + server_address);
    
    grpc_thread_ = std::thread([this]() {
        if (grpc_server_) {
            grpc_server_->Wait();
        }
    });
}

void LoadBalancerApp::stop_grpc_server() {
    if (grpc_server_) {
        grpc_server_->Shutdown();
        grpc_server_.reset();
    }
    if (grpc_thread_.joinable()) {
        grpc_thread_.join();
    }
}

void LoadBalancerApp::handle_signals() {
    signals_.add(SIGINT);
    signals_.add(SIGTERM);
    signals_.async_wait([this](const boost::system::error_code& error, int signal_number) {
        if (!error) {
            server::core::log::info("LoadBalancerApp received signal " + std::to_string(signal_number));
            stop();
        }
    });
}

} // namespace load_balancer
