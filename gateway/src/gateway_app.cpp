#include "gateway/gateway_app.hpp"

#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <random>
#include <algorithm>
#include <deque>
#include <iomanip>
#include <sstream>
#include <type_traits>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/hmac.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "gateway/gateway_connection.hpp"
#include "server/core/protocol/packet.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/protocol/system_opcodes.hpp"
#include "server/core/util/log.hpp"
#include "server/core/metrics/build_info.hpp"
#include "server/protocol/game_opcodes.hpp"
#include "server/storage/redis/client.hpp"
#include "server/state/instance_registry.hpp"

/**
 * @brief GatewayApp/BackendConnection의 라우팅·브리지 구현입니다.
 *
 * sticky + least-connections 정책으로 backend를 선택하고,
 * connect timeout/송신 큐 상한으로 장애 전파를 제한합니다.
 */
namespace gateway {

namespace {

constexpr const char* kEnvGatewayListen = "GATEWAY_LISTEN";
constexpr const char* kEnvGatewayId = "GATEWAY_ID";
constexpr const char* kEnvRedisUri = "REDIS_URI";
constexpr const char* kEnvServerRegistryPrefix = "SERVER_REGISTRY_PREFIX";
constexpr const char* kEnvServerRegistryTtl = "SERVER_REGISTRY_TTL";
constexpr const char* kEnvGatewayBackendConnectTimeoutMs = "GATEWAY_BACKEND_CONNECT_TIMEOUT_MS";
constexpr const char* kEnvGatewayBackendSendQueueMaxBytes = "GATEWAY_BACKEND_SEND_QUEUE_MAX_BYTES";
constexpr const char* kEnvAllowAnonymous = "ALLOW_ANONYMOUS";
constexpr const char* kEnvGatewayUdpListen = "GATEWAY_UDP_LISTEN";
constexpr const char* kEnvGatewayUdpBindSecret = "GATEWAY_UDP_BIND_SECRET";
constexpr const char* kEnvGatewayUdpBindTtlMs = "GATEWAY_UDP_BIND_TTL_MS";
constexpr const char* kEnvGatewayUdpBindFailWindowMs = "GATEWAY_UDP_BIND_FAIL_WINDOW_MS";
constexpr const char* kEnvGatewayUdpBindFailLimit = "GATEWAY_UDP_BIND_FAIL_LIMIT";
constexpr const char* kEnvGatewayUdpBindBlockMs = "GATEWAY_UDP_BIND_BLOCK_MS";
constexpr const char* kDefaultGatewayListen = "0.0.0.0:6000";
constexpr const char* kDefaultGatewayId = "gateway-default";
constexpr const char* kDefaultRedisUri = "tcp://127.0.0.1:6379";
constexpr const char* kDefaultServerRegistryPrefix = "gateway/instances/";
constexpr std::uint32_t kDefaultBackendConnectTimeoutMs = 5000;
constexpr std::size_t kDefaultBackendSendQueueMaxBytes = 256 * 1024;
constexpr std::uint32_t kDefaultUdpBindTtlMs = 5000;
constexpr std::uint32_t kDefaultUdpBindFailWindowMs = 10000;
constexpr std::uint32_t kDefaultUdpBindFailLimit = 5;
constexpr std::uint32_t kDefaultUdpBindBlockMs = 60000;

#if defined(KNIGHTS_ENABLE_GATEWAY_UDP_INGRESS) && (KNIGHTS_ENABLE_GATEWAY_UDP_INGRESS == 1)
constexpr bool kGatewayUdpIngressBuildEnabled = true;
#else
constexpr bool kGatewayUdpIngressBuildEnabled = false;
#endif

std::uint64_t unix_time_ms() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    return static_cast<std::uint64_t>(now.count());
}

void write_be64(std::uint64_t value, std::vector<std::uint8_t>& out) {
    out.push_back(static_cast<std::uint8_t>((value >> 56) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 48) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 40) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 32) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
}

bool read_be64(std::span<const std::uint8_t>& in, std::uint64_t& out) {
    if (in.size() < 8) {
        return false;
    }

    out = (static_cast<std::uint64_t>(in[0]) << 56)
        | (static_cast<std::uint64_t>(in[1]) << 48)
        | (static_cast<std::uint64_t>(in[2]) << 40)
        | (static_cast<std::uint64_t>(in[3]) << 32)
        | (static_cast<std::uint64_t>(in[4]) << 24)
        | (static_cast<std::uint64_t>(in[5]) << 16)
        | (static_cast<std::uint64_t>(in[6]) << 8)
        | static_cast<std::uint64_t>(in[7]);
    in = in.subspan(8);
    return true;
}

std::string to_hex(std::span<const std::uint8_t> bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(bytes.size() * 2);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out[2 * i] = kHex[(bytes[i] >> 4) & 0x0F];
        out[2 * i + 1] = kHex[bytes[i] & 0x0F];
    }
    return out;
}

std::string make_bind_signing_input(std::string_view session_id,
                                    std::uint64_t nonce,
                                    std::uint64_t expires_unix_ms) {
    return std::string(session_id)
        + "|" + std::to_string(nonce)
        + "|" + std::to_string(expires_unix_ms);
}

std::string hmac_sha256_hex(std::string_view secret, std::string_view message) {
    unsigned int digest_len = 0;
    unsigned char digest[EVP_MAX_MD_SIZE]{};

    const auto* result = HMAC(EVP_sha256(),
                              secret.data(),
                              static_cast<int>(secret.size()),
                              reinterpret_cast<const unsigned char*>(message.data()),
                              message.size(),
                              digest,
                              &digest_len);
    if (result == nullptr || digest_len == 0) {
        return {};
    }
    return to_hex(std::span<const std::uint8_t>(digest, digest_len));
}

bool secure_equals(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    if (lhs.empty()) {
        return true;
    }
    return CRYPTO_memcmp(lhs.data(), rhs.data(), lhs.size()) == 0;
}

template <typename T>
T parse_env_integral_bounded(const char* key,
                             T fallback,
                             T min_value,
                             T max_value,
                             const char* warning_message) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    if (const char* value = std::getenv(key); value && *value) {
        try {
            const auto parsed = std::stoull(value);
            if (parsed >= min_value && parsed <= max_value) {
                return static_cast<T>(parsed);
            }
        } catch (...) {
        }
        server::core::log::warn(warning_message);
    }
    return fallback;
}

std::uint32_t parse_env_u32_bounded(const char* key,
                                    std::uint32_t fallback,
                                    std::uint32_t min_value,
                                    std::uint32_t max_value,
                                    const char* warning_message) {
    return parse_env_integral_bounded<std::uint32_t>(key, fallback, min_value, max_value, warning_message);
}

std::size_t parse_env_size_bounded(const char* key,
                                   std::size_t fallback,
                                   std::size_t min_value,
                                   std::size_t max_value,
                                   const char* warning_message) {
    return parse_env_integral_bounded<std::size_t>(key, fallback, min_value, max_value, warning_message);
}

std::pair<std::string, std::uint16_t> parse_listen(std::string_view value, std::uint16_t fallback_port) {
    if (value.empty()) {
        return {"0.0.0.0", fallback_port};
    }

    auto delimiter = value.find(':');
    if (delimiter == std::string_view::npos) {
        return {std::string(value), fallback_port};
    }

    std::string host(value.substr(0, delimiter));
    std::string_view port_view = value.substr(delimiter + 1);
    std::uint16_t port = fallback_port;
    if (!port_view.empty()) {
        try {
            port = static_cast<std::uint16_t>(std::stoul(std::string(port_view)));
        } catch (...) {
            server::core::log::warn("GatewayApp invalid port in GATEWAY_LISTEN; falling back to default");
            port = fallback_port;
        }
    }
    return {std::move(host), port};
}

std::string make_boot_id() {
    std::random_device rd;
    const std::uint64_t v = (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd());
    std::ostringstream oss;
    oss << std::hex << std::setw(8) << std::setfill('0') << static_cast<std::uint32_t>(v & 0xFFFFFFFFu);
    return oss.str();
}

std::string endpoint_key(const boost::asio::ip::udp::endpoint& endpoint) {
    return endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
}

} // namespace

// --- BackendConnection Implementation ---

// Re-implementing GatewayApp::BackendConnection methods directly

GatewayApp::BackendConnection::BackendConnection(GatewayApp& app,
                                            std::string session_id,
                                            std::string client_id,
                                            std::string backend_instance_id,
                                            bool sticky_hit,
                                            std::weak_ptr<GatewayConnection> connection,
                                            std::size_t send_queue_max_bytes,
                                            std::chrono::milliseconds connect_timeout)
    : app_(app)
    , session_id_(std::move(session_id))
    , client_id_(std::move(client_id))
    , backend_instance_id_(std::move(backend_instance_id))
    , sticky_hit_(sticky_hit)
    , connection_(std::move(connection))
    , socket_(app.io_)
    , connect_timer_(app.io_)
    , send_queue_max_bytes_(send_queue_max_bytes > 0 ? send_queue_max_bytes : kDefaultBackendSendQueueMaxBytes)
    , connect_timeout_(connect_timeout > std::chrono::milliseconds{0}
                           ? connect_timeout
                           : std::chrono::milliseconds{kDefaultBackendConnectTimeoutMs}) {
}

GatewayApp::BackendConnection::~BackendConnection() {
    close();
}

void GatewayApp::BackendConnection::connect(const std::string& host, std::uint16_t port) {
    if (closed_.load(std::memory_order_relaxed)) return;
    do_connect(host, port);
}

void GatewayApp::BackendConnection::do_connect(const std::string& host, std::uint16_t port) {
    auto self = shared_from_this();
    auto resolver = std::make_shared<boost::asio::ip::tcp::resolver>(app_.io_);
    
    resolver->async_resolve(host, std::to_string(port),
        [self, this, resolver](const boost::system::error_code& ec, boost::asio::ip::tcp::resolver::results_type results) {
            if (closed_.load(std::memory_order_relaxed)) {
                return;
            }

            if (ec) {
                app_.record_backend_resolve_fail();
                server::core::log::error("BackendConnection resolve failed: " + ec.message());
                if (auto conn = connection_.lock()) {
                    conn->handle_backend_close("resolve failed");
                } else {
                    close();
                }
                return;
            }

            connect_timer_.expires_after(connect_timeout_);
            connect_timer_.async_wait([self, this](const boost::system::error_code& timer_ec) {
                if (timer_ec == boost::asio::error::operation_aborted) {
                    return;
                }
                on_connect_timeout();
            });
            
            boost::asio::async_connect(socket_, results,
                [self, this](const boost::system::error_code& ec, const boost::asio::ip::tcp::endpoint& /*endpoint*/) {
                    (void)connect_timer_.cancel();

                    if (closed_.load(std::memory_order_relaxed)) {
                        return;
                    }

                    if (ec) {
                        app_.record_backend_connect_fail();
                        server::core::log::error("BackendConnection connect failed: " + ec.message());
                        if (auto conn = connection_.lock()) {
                            conn->handle_backend_close("connect failed");
                        } else {
                            close();
                        }
                        return;
                    }
                    
                    {
                        std::lock_guard<std::mutex> lock(send_mutex_);
                        connected_ = true;
                        if (!write_queue_.empty()) {
                            do_write();
                        }
                    }

                    // Backend TCP 연결이 성공했으므로 sticky binding을 갱신한다.
                    // connect 성공 후에만 바인딩해야, 연결 실패로 인한 zombie mapping을 피할 수 있다.
                    app_.on_backend_connected(client_id_, backend_instance_id_, sticky_hit_);
                    do_read();
                });
        });
}

void GatewayApp::BackendConnection::on_connect_timeout() {
    bool expected = false;
    if (!closed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    app_.record_backend_connect_timeout();
    server::core::log::warn(
        "BackendConnection connect timeout after " + std::to_string(connect_timeout_.count()) + "ms"
    );

    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        write_queue_.clear();
        queued_bytes_ = 0;
        connected_ = false;
        write_in_progress_ = false;
    }

    boost::system::error_code ignored;
    if (socket_.is_open()) {
        socket_.cancel(ignored);
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
        socket_.close(ignored);
    }

    if (auto conn = connection_.lock()) {
        conn->handle_backend_close("connect timeout");
    }
}

void GatewayApp::BackendConnection::send(std::vector<std::uint8_t> payload) {
    if (payload.empty()) {
        return;
    }

    bool overflow = false;

    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        if (closed_) {
            return;
        }

        const auto payload_bytes = payload.size();
        if (payload_bytes > send_queue_max_bytes_ ||
            queued_bytes_ > (send_queue_max_bytes_ - payload_bytes)) {
            overflow = true;
        } else {
            queued_bytes_ += payload_bytes;
            write_queue_.push_back(std::move(payload));
            if (connected_ && !write_in_progress_) {
                do_write();
            }
        }
    }

    if (overflow) {
        app_.record_backend_send_queue_overflow();
        server::core::log::warn(
            "BackendConnection send queue overflow: max_bytes=" + std::to_string(send_queue_max_bytes_)
        );
        if (auto conn = connection_.lock()) {
            conn->handle_backend_close("backend send queue overflow");
        } else {
            close();
        }
    }
}

void GatewayApp::BackendConnection::send(const std::uint8_t* data, std::size_t length) {
    if (!data || length == 0) {
        return;
    }

    // 브리지 read 버퍼를 바로 큐에 복사해 caller의 중간 임시 payload vector를 줄인다.
    bool overflow = false;

    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        if (closed_) {
            return;
        }

        if (length > send_queue_max_bytes_ ||
            queued_bytes_ > (send_queue_max_bytes_ - length)) {
            overflow = true;
        } else {
            queued_bytes_ += length;
            write_queue_.emplace_back(data, data + length);
            if (connected_ && !write_in_progress_) {
                do_write();
            }
        }
    }

    if (overflow) {
        app_.record_backend_send_queue_overflow();
        server::core::log::warn(
            "BackendConnection send queue overflow: max_bytes=" + std::to_string(send_queue_max_bytes_)
        );
        if (auto conn = connection_.lock()) {
            conn->handle_backend_close("backend send queue overflow");
        } else {
            close();
        }
    }
}

void GatewayApp::BackendConnection::do_write() {
    if (write_queue_.empty()) {
        write_in_progress_ = false;
        return;
    }

    write_in_progress_ = true;
    auto& msg = write_queue_.front();
    
    auto self = shared_from_this();
    boost::asio::async_write(socket_, boost::asio::buffer(msg),
        [self, this](const boost::system::error_code& ec, std::size_t /*bytes_transferred*/) {
            if (ec) {
                app_.record_backend_write_error();
                if (auto conn = connection_.lock()) {
                    conn->handle_backend_close("backend write failed");
                } else {
                    close();
                }
                return;
            }

            std::lock_guard<std::mutex> lock(send_mutex_);
            if (!write_queue_.empty()) {
                const auto sent = write_queue_.front().size();
                queued_bytes_ = queued_bytes_ >= sent ? (queued_bytes_ - sent) : 0;
                write_queue_.pop_front();
            }
            if (!write_queue_.empty()) {
                do_write();
            } else {
                write_in_progress_ = false;
            }
        });
}

void GatewayApp::BackendConnection::do_read() {
    auto self = shared_from_this();
    socket_.async_read_some(boost::asio::buffer(buffer_),
        [self, this](const boost::system::error_code& ec, std::size_t bytes_transferred) {
            on_read(ec, bytes_transferred);
        });
}

void GatewayApp::BackendConnection::on_read(const boost::system::error_code& ec, std::size_t bytes_transferred) {
    if (ec) {
        if (ec != boost::asio::error::operation_aborted) {
            if (auto conn = connection_.lock()) {
                conn->handle_backend_close(ec.message());
            }
            close();
        }
        return;
    }

    if (bytes_transferred > 0) {
        if (auto conn = connection_.lock()) {
            std::vector<std::uint8_t> data(buffer_.begin(), buffer_.begin() + bytes_transferred);
            conn->handle_backend_payload(std::move(data));
        }
        do_read();
    }
}

void GatewayApp::BackendConnection::close() {
    bool expected = false;
    if (!closed_.compare_exchange_strong(expected, true)) return;

    (void)connect_timer_.cancel();

    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        write_queue_.clear();
        queued_bytes_ = 0;
        connected_ = false;
        write_in_progress_ = false;
    }

    boost::system::error_code ignored;
    if (socket_.is_open()) {
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
        socket_.close(ignored);
    }
}

const std::string& GatewayApp::BackendConnection::session_id() const {
    return session_id_;
}

// --- GatewayApp Implementation ---

GatewayApp::GatewayApp()
    : hive_(std::make_shared<server::core::net::Hive>(io_))
    , authenticator_(std::make_shared<auth::NoopAuthenticator>()) {
    app_host_.set_lifecycle_phase(server::core::app::AppHost::LifecyclePhase::kBootstrapping);
    
    configure_gateway();

    boot_id_ = make_boot_id();
    server::core::log::info("GatewayApp boot_id=" + boot_id_);

    if (udp_listen_port_ != 0 && udp_bind_secret_.empty()) {
        udp_bind_secret_ = boot_id_;
        server::core::log::warn("GatewayApp GATEWAY_UDP_BIND_SECRET not set; using boot_id derived secret");
    }

    configure_infrastructure();

    // Redis is required for backend discovery and sticky routing.
    app_host_.declare_dependency("redis", server::core::app::AppHost::DependencyRequirement::kRequired);
    app_host_.set_ready(false);

    if (const char* port_env = std::getenv("METRICS_PORT")) {
        try {
            metrics_port_ = static_cast<std::uint16_t>(std::stoul(port_env));
        } catch (...) {
            server::core::log::warn("GatewayApp invalid METRICS_PORT; using default");
        }
    }

    app_host_.start_admin_http(metrics_port_, [this]() {
        std::ostringstream stream;

        // Build metadata (git hash/describe + build time)
        server::core::metrics::append_build_info(stream);

        stream << "# TYPE gateway_sessions_active gauge\n";
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            stream << "gateway_sessions_active " << sessions_.size() << "\n";
        }
        stream << "# TYPE gateway_connections_total counter\n";
        stream << "gateway_connections_total " << connections_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE gateway_backend_resolve_fail_total counter\n";
        stream << "gateway_backend_resolve_fail_total "
               << backend_resolve_fail_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE gateway_backend_connect_fail_total counter\n";
        stream << "gateway_backend_connect_fail_total "
               << backend_connect_fail_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE gateway_backend_connect_timeout_total counter\n";
        stream << "gateway_backend_connect_timeout_total "
               << backend_connect_timeout_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE gateway_backend_write_error_total counter\n";
        stream << "gateway_backend_write_error_total "
               << backend_write_error_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE gateway_backend_send_queue_overflow_total counter\n";
        stream << "gateway_backend_send_queue_overflow_total "
               << backend_send_queue_overflow_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE gateway_backend_connect_timeout_ms gauge\n";
        stream << "gateway_backend_connect_timeout_ms " << backend_connect_timeout_ms_ << "\n";

        stream << "# TYPE gateway_backend_send_queue_max_bytes gauge\n";
        stream << "gateway_backend_send_queue_max_bytes " << backend_send_queue_max_bytes_ << "\n";

        stream << "# TYPE gateway_udp_enabled gauge\n";
        stream << "gateway_udp_enabled " << (udp_enabled_.load(std::memory_order_relaxed) ? 1 : 0) << "\n";

        stream << "# TYPE gateway_udp_ingress_feature_enabled gauge\n";
        stream << "gateway_udp_ingress_feature_enabled " << (kGatewayUdpIngressBuildEnabled ? 1 : 0) << "\n";

        stream << "# TYPE gateway_udp_packets_total counter\n";
        stream << "gateway_udp_packets_total " << udp_packets_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE gateway_udp_receive_error_total counter\n";
        stream << "gateway_udp_receive_error_total " << udp_receive_error_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE gateway_udp_bind_ticket_issued_total counter\n";
        stream << "gateway_udp_bind_ticket_issued_total "
               << udp_bind_ticket_issued_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE gateway_udp_bind_success_total counter\n";
        stream << "gateway_udp_bind_success_total "
               << udp_bind_success_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE gateway_udp_bind_reject_total counter\n";
        stream << "gateway_udp_bind_reject_total "
               << udp_bind_reject_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE gateway_udp_bind_block_total counter\n";
        stream << "gateway_udp_bind_block_total "
               << udp_bind_block_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE gateway_udp_bind_rate_limit_reject_total counter\n";
        stream << "gateway_udp_bind_rate_limit_reject_total "
               << udp_bind_rate_limit_reject_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE gateway_udp_bind_fail_window_ms gauge\n";
        stream << "gateway_udp_bind_fail_window_ms " << udp_bind_fail_window_ms_ << "\n";

        stream << "# TYPE gateway_udp_bind_fail_limit gauge\n";
        stream << "gateway_udp_bind_fail_limit " << udp_bind_fail_limit_ << "\n";

        stream << "# TYPE gateway_udp_bind_block_ms gauge\n";
        stream << "gateway_udp_bind_block_ms " << udp_bind_block_ms_ << "\n";

        stream << "# TYPE gateway_udp_bind_ttl_ms gauge\n";
        stream << "gateway_udp_bind_ttl_ms " << udp_bind_ttl_ms_ << "\n";

        stream << "# TYPE gateway_udp_forward_total counter\n";
        stream << "gateway_udp_forward_total "
               << udp_forward_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE gateway_transport_delivery_forward_total counter\n";
        stream << "gateway_transport_delivery_forward_total{transport=\"udp\",delivery=\"reliable_ordered\"} "
               << udp_forward_reliable_ordered_total_.load(std::memory_order_relaxed) << "\n";
        stream << "gateway_transport_delivery_forward_total{transport=\"udp\",delivery=\"reliable\"} "
               << udp_forward_reliable_total_.load(std::memory_order_relaxed) << "\n";
        stream << "gateway_transport_delivery_forward_total{transport=\"udp\",delivery=\"unreliable_sequenced\"} "
               << udp_forward_unreliable_sequenced_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE gateway_udp_replay_drop_total counter\n";
        stream << "gateway_udp_replay_drop_total "
               << udp_replay_drop_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE gateway_udp_reorder_drop_total counter\n";
        stream << "gateway_udp_reorder_drop_total "
               << udp_reorder_drop_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE gateway_udp_duplicate_drop_total counter\n";
        stream << "gateway_udp_duplicate_drop_total "
               << udp_duplicate_drop_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE gateway_transport_delivery_drop_total counter\n";
        stream << "gateway_transport_delivery_drop_total{transport=\"udp\",delivery=\"unreliable_sequenced\",reason=\"replay\"} "
               << udp_replay_drop_total_.load(std::memory_order_relaxed) << "\n";
        stream << "gateway_transport_delivery_drop_total{transport=\"udp\",delivery=\"unreliable_sequenced\",reason=\"reorder\"} "
               << udp_reorder_drop_total_.load(std::memory_order_relaxed) << "\n";
        stream << "gateway_transport_delivery_drop_total{transport=\"udp\",delivery=\"unreliable_sequenced\",reason=\"duplicate\"} "
               << udp_duplicate_drop_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE gateway_udp_retransmit_total counter\n";
        stream << "gateway_udp_retransmit_total "
               << udp_retransmit_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE gateway_udp_loss_estimated_total counter\n";
        stream << "gateway_udp_loss_estimated_total "
               << udp_loss_estimated_total_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE gateway_udp_jitter_ms_last gauge\n";
        stream << "gateway_udp_jitter_ms_last "
               << udp_jitter_ms_last_.load(std::memory_order_relaxed) << "\n";

        stream << "# TYPE gateway_udp_rtt_ms_last gauge\n";
        stream << "gateway_udp_rtt_ms_last "
               << udp_rtt_ms_last_.load(std::memory_order_relaxed) << "\n";

        stream << app_host_.dependency_metrics_text();
        stream << app_host_.lifecycle_metrics_text();
        return stream.str();
    });

    app_host_.add_shutdown_step("stop gateway", [this]() { stop(); });
}

GatewayApp::~GatewayApp() {
    stop();
}

int GatewayApp::run() {
    start_listener();
    start_udp_listener();
    app_host_.set_ready(true);
    app_host_.set_lifecycle_phase(server::core::app::AppHost::LifecyclePhase::kRunning);
    start_infrastructure_probe();
    app_host_.install_asio_termination_signals(io_, {});

    server::core::log::info("GatewayApp starting main loop");
    hive_->run();

    stop_infrastructure_probe();
    app_host_.set_ready(false);
    app_host_.set_lifecycle_phase(server::core::app::AppHost::LifecyclePhase::kStopped);
    server::core::log::info("GatewayApp stopped");
    return 0;
}

void GatewayApp::stop() {
    app_host_.request_stop();
    app_host_.set_ready(false);
    stop_infrastructure_probe();
    app_host_.stop_admin_http();

    if (listener_) {
        listener_->stop();
    }

    stop_udp_listener();

    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        for (auto& [_, state] : sessions_) {
            if (state.session) {
                state.session->close();
            }
        }
        sessions_.clear();
    }

    if (hive_) {
        hive_->stop();
    }
    io_.stop();
}

void GatewayApp::start_infrastructure_probe() {
    if (infra_probe_thread_.joinable()) {
        return;
    }

    infra_probe_stop_.store(false, std::memory_order_relaxed);
    infra_probe_thread_ = std::thread([this]() {
        bool last_ok = true;
        while (!infra_probe_stop_.load(std::memory_order_relaxed) && !app_host_.stop_requested()) {
            bool ok = false;
            try {
                if (redis_client_) {
                    ok = redis_client_->health_check();
                }
            } catch (const std::exception& e) {
                server::core::log::warn(std::string("GatewayApp Redis health_check exception: ") + e.what());
                ok = false;
            } catch (...) {
                server::core::log::warn("GatewayApp Redis health_check unknown exception");
                ok = false;
            }

            app_host_.set_dependency_ok("redis", ok);

            if (ok != last_ok) {
                if (ok) {
                    server::core::log::info("GatewayApp Redis health_check OK");
                } else {
                    server::core::log::warn("GatewayApp Redis health_check FAILED");
                }
                last_ok = ok;
            }

            for (int i = 0; i < 20; ++i) {
                if (infra_probe_stop_.load(std::memory_order_relaxed) || app_host_.stop_requested()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        app_host_.set_dependency_ok("redis", false);
    });
}

void GatewayApp::stop_infrastructure_probe() {
    infra_probe_stop_.store(true, std::memory_order_relaxed);
    if (infra_probe_thread_.joinable() && infra_probe_thread_.get_id() != std::this_thread::get_id()) {
        infra_probe_thread_.join();
    }
}

GatewayApp::BackendConnectionPtr GatewayApp::create_backend_connection(const std::string& client_id,
                                                                 std::weak_ptr<GatewayConnection> connection) {
    auto selected = select_best_server(client_id);
    if (!selected) {
        server::core::log::error("GatewayApp: No available backend server found");
        return nullptr;
    }

    // 고유 세션 ID 생성
    // 더 나은 생성기를 사용할 수 있지만, 현재는 원자적 카운터로 충분합니다.
    static std::atomic<std::uint64_t> counter{0};
    std::string session_id = gateway_id_ + "-" + boot_id_ + "-" + std::to_string(++counter);

    auto session = std::make_shared<BackendConnection>(
        *this,
        session_id,
        client_id,
        selected->record.instance_id,
        selected->sticky_hit,
        std::move(connection),
        backend_send_queue_max_bytes_,
        std::chrono::milliseconds{backend_connect_timeout_ms_}
    );

    server::core::log::info(
        "GatewayApp connecting session " + session_id +
        " backend=" + selected->record.instance_id +
        " addr=" + selected->record.host + ":" + std::to_string(selected->record.port)
    );
    session->connect(selected->record.host, selected->record.port);

    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        SessionState state{};
        state.session = session;
        state.client_id = client_id;
        sessions_[session_id] = std::move(state);
    }

    return session;
}

void GatewayApp::close_backend_connection(const std::string& session_id) {
    BackendConnectionPtr session;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            session = it->second.session;
            sessions_.erase(it);
        }
    }
    if (session) {
        session->close();
    }
}

std::string GatewayApp::make_udp_bind_token(std::string_view session_id,
                                            std::uint64_t nonce,
                                            std::uint64_t expires_unix_ms) const {
    if (udp_bind_secret_.empty()) {
        return {};
    }

    const auto signing_input = make_bind_signing_input(session_id, nonce, expires_unix_ms);
    return hmac_sha256_hex(udp_bind_secret_, signing_input);
}

std::vector<std::uint8_t> GatewayApp::make_udp_bind_res_frame(std::uint16_t code,
                                                               const UdpBindTicket& ticket,
                                                               std::string_view message,
                                                               std::uint32_t seq) const {
    return make_udp_bind_res_frame(
        code,
        ticket.session_id,
        ticket.nonce,
        ticket.expires_unix_ms,
        ticket.token,
        message,
        seq
    );
}

std::vector<std::uint8_t> GatewayApp::make_udp_bind_res_frame(std::uint16_t code,
                                                               std::string_view session_id,
                                                               std::uint64_t nonce,
                                                               std::uint64_t expires_unix_ms,
                                                               std::string_view token,
                                                               std::string_view message,
                                                               std::uint32_t seq) const {
    namespace proto = server::core::protocol;

    std::vector<std::uint8_t> payload;
    payload.reserve(2 + 2 + session_id.size() + 8 + 8 + 2 + token.size() + 2 + message.size());

    std::array<std::uint8_t, 2> code_buf{};
    proto::write_be16(code, code_buf.data());
    payload.insert(payload.end(), code_buf.begin(), code_buf.end());

    proto::write_lp_utf8(payload, session_id);
    write_be64(nonce, payload);
    write_be64(expires_unix_ms, payload);
    proto::write_lp_utf8(payload, token);
    proto::write_lp_utf8(payload, message);

    proto::PacketHeader header{};
    header.length = static_cast<std::uint16_t>(payload.size());
    header.msg_id = server::protocol::MSG_UDP_BIND_RES;
    header.flags = 0;
    header.seq = seq;
    header.utc_ts_ms32 = static_cast<std::uint32_t>(unix_time_ms() & 0xFFFFFFFFu);

    std::vector<std::uint8_t> frame(proto::k_header_bytes + payload.size());
    proto::encode_header(header, frame.data());
    if (!payload.empty()) {
        std::memcpy(frame.data() + proto::k_header_bytes, payload.data(), payload.size());
    }
    return frame;
}

std::optional<std::vector<std::uint8_t>> GatewayApp::make_udp_bind_ticket_frame(const std::string& session_id) {
    if (!kGatewayUdpIngressBuildEnabled) {
        return std::nullopt;
    }

    if (udp_listen_port_ == 0) {
        return std::nullopt;
    }

    if (udp_bind_secret_.empty()) {
        return std::nullopt;
    }

    UdpBindTicket ticket{};
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            return std::nullopt;
        }

        std::random_device rd;
        const std::uint64_t nonce = (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd());
        const std::uint64_t issued_unix_ms = unix_time_ms();
        const std::uint64_t expires_unix_ms = issued_unix_ms + static_cast<std::uint64_t>(udp_bind_ttl_ms_);
        const std::string token = make_udp_bind_token(session_id, nonce, expires_unix_ms);
        if (token.empty()) {
            return std::nullopt;
        }

        it->second.udp_nonce = nonce;
        it->second.udp_expires_unix_ms = expires_unix_ms;
        it->second.udp_ticket_issued_unix_ms = issued_unix_ms;
        it->second.udp_token = token;
        it->second.udp_bound = false;
        it->second.udp_endpoint = {};
        it->second.udp_sequenced_metrics.reset();

        ticket.session_id = session_id;
        ticket.nonce = nonce;
        ticket.expires_unix_ms = expires_unix_ms;
        ticket.token = token;
    }

    (void)udp_bind_ticket_issued_total_.fetch_add(1, std::memory_order_relaxed);
    return make_udp_bind_res_frame(0, ticket, "issued");
}

bool GatewayApp::parse_udp_bind_req(std::span<const std::uint8_t> payload, ParsedUdpBindRequest& out) const {
    namespace proto = server::core::protocol;

    std::span<const std::uint8_t> in = payload;
    std::string session_id;
    if (!proto::read_lp_utf8(in, session_id)) {
        return false;
    }

    std::uint64_t nonce = 0;
    if (!read_be64(in, nonce)) {
        return false;
    }

    std::uint64_t expires_unix_ms = 0;
    if (!read_be64(in, expires_unix_ms)) {
        return false;
    }

    std::string token;
    if (!proto::read_lp_utf8(in, token)) {
        return false;
    }

    if (!in.empty()) {
        return false;
    }

    out.session_id = std::move(session_id);
    out.nonce = nonce;
    out.expires_unix_ms = expires_unix_ms;
    out.token = std::move(token);
    return true;
}

std::uint16_t GatewayApp::apply_udp_bind_request(const ParsedUdpBindRequest& req,
                                                 const boost::asio::ip::udp::endpoint& endpoint,
                                                 UdpBindTicket& applied_ticket,
                                                 std::string& message) {
    using server::core::protocol::errc::FORBIDDEN;
    using server::core::protocol::errc::INVALID_PAYLOAD;
    using server::core::protocol::errc::UNAUTHORIZED;

    if (req.session_id.empty() || req.token.empty()) {
        message = "invalid bind payload";
        return INVALID_PAYLOAD;
    }

    const auto now_ms = unix_time_ms();

    if (req.expires_unix_ms < now_ms) {
        message = "ticket expired";
        return UNAUTHORIZED;
    }

    std::lock_guard<std::mutex> lock(session_mutex_);
    auto it = sessions_.find(req.session_id);
    if (it == sessions_.end()) {
        message = "unknown session";
        return UNAUTHORIZED;
    }

    auto& state = it->second;
    if (state.udp_expires_unix_ms == 0 || state.udp_nonce == 0 || state.udp_token.empty()) {
        message = "bind ticket not issued";
        return UNAUTHORIZED;
    }

    if (state.udp_expires_unix_ms < now_ms) {
        message = "ticket expired";
        return UNAUTHORIZED;
    }

    if (req.expires_unix_ms != state.udp_expires_unix_ms || req.nonce != state.udp_nonce) {
        message = "ticket mismatch";
        return UNAUTHORIZED;
    }

    const std::string expected = make_udp_bind_token(req.session_id, req.nonce, req.expires_unix_ms);
    if (!secure_equals(req.token, state.udp_token) || !secure_equals(req.token, expected)) {
        message = "invalid token";
        return UNAUTHORIZED;
    }

    if (state.udp_bound && state.udp_endpoint != endpoint) {
        message = "session already bound";
        return FORBIDDEN;
    }

    state.udp_bound = true;
    state.udp_endpoint = endpoint;
    state.udp_sequenced_metrics.reset();

    if (state.udp_ticket_issued_unix_ms != 0 && now_ms >= state.udp_ticket_issued_unix_ms) {
        const auto bind_rtt_ms = now_ms - state.udp_ticket_issued_unix_ms;
        udp_rtt_ms_last_.store(bind_rtt_ms, std::memory_order_relaxed);
    }

    applied_ticket.session_id = req.session_id;
    applied_ticket.nonce = req.nonce;
    applied_ticket.expires_unix_ms = req.expires_unix_ms;
    applied_ticket.token = req.token;

    message = "bound";
    return 0;
}

void GatewayApp::send_udp_datagram(std::vector<std::uint8_t> frame,
                                   const boost::asio::ip::udp::endpoint& endpoint) {
    if (!udp_socket_) {
        return;
    }

    udp_socket_->async_send_to(
        boost::asio::buffer(frame),
        endpoint,
        [frame = std::move(frame)](const boost::system::error_code&, std::size_t) mutable {
            frame.clear();
        });
}

std::optional<GatewayApp::SelectedBackend> GatewayApp::select_best_server(const std::string& client_id) {
    if (!backend_registry_) {
        return std::nullopt;
    }

    auto instances = backend_registry_->list_instances();
    if (instances.empty()) {
        return std::nullopt;
    }

    // 1) 세션 스티키니스: 기존 바인딩이 유효하면 우선 사용한다.
    if (session_directory_ && !client_id.empty() && client_id != "anonymous") {
        if (auto backend_id = session_directory_->find_backend(client_id)) {
            auto it = std::find_if(instances.begin(), instances.end(), [&](const auto& rec) {
                return rec.instance_id == *backend_id && rec.ready;
            });
            if (it != instances.end()) {
                return SelectedBackend{*it, true};
            }
            // 바인딩된 인스턴스가 사라졌거나 비활성화되었으므로 바인딩을 해제한다.
            session_directory_->release_backend(client_id, *backend_id);
        }
    }

    // 2) 신규 선택: least-connections(active_sessions) 기반.
    const server::state::InstanceRecord* selected = nullptr;
    for (const auto& rec : instances) {
        if (!rec.ready || rec.instance_id.empty() || rec.host.empty() || rec.port == 0) {
            continue;
        }
        if (selected == nullptr || rec.active_sessions < selected->active_sessions) {
            selected = &rec;
        }
    }

    if (selected == nullptr) {
        return std::nullopt;
    }

    return SelectedBackend{*selected, false};
}

void GatewayApp::on_backend_connected(const std::string& client_id,
                                     const std::string& backend_instance_id,
                                     bool sticky_hit) {
    if (!session_directory_) {
        return;
    }
    if (client_id.empty() || client_id == "anonymous") {
        return;
    }
    if (backend_instance_id.empty()) {
        return;
    }

    // Post-connect binding: only commit sticky mapping after the backend TCP connection succeeds.
    // ensure_backend() creates the mapping if absent (SETNX) and refreshes TTL if already bound.
    auto bound = session_directory_->ensure_backend(client_id, backend_instance_id);
    if (sticky_hit && bound && *bound != backend_instance_id) {
        server::core::log::warn(
            "GatewayApp sticky mismatch: client_id=" + client_id +
            " desired=" + backend_instance_id +
            " existing=" + *bound
        );
    }
}

void GatewayApp::configure_gateway() {
    const char* listen_env = std::getenv(kEnvGatewayListen);
    const auto [host, port] = parse_listen(listen_env ? std::string_view(listen_env) : std::string_view(kDefaultGatewayListen),
                                           listen_port_);
    listen_host_ = host;
    listen_port_ = port;

    const char* id_env = std::getenv(kEnvGatewayId);
    if (id_env && *id_env) {
        gateway_id_ = id_env;
    } else {
        gateway_id_ = kDefaultGatewayId;
    }

    backend_connect_timeout_ms_ = parse_env_u32_bounded(
        kEnvGatewayBackendConnectTimeoutMs,
        kDefaultBackendConnectTimeoutMs,
        100,
        60000,
        "GatewayApp invalid GATEWAY_BACKEND_CONNECT_TIMEOUT_MS; using default"
    );

    backend_send_queue_max_bytes_ = parse_env_size_bounded(
        kEnvGatewayBackendSendQueueMaxBytes,
        kDefaultBackendSendQueueMaxBytes,
        1024,
        16 * 1024 * 1024,
        "GatewayApp invalid GATEWAY_BACKEND_SEND_QUEUE_MAX_BYTES; using default"
    );

    if (const char* udp_listen_env = std::getenv(kEnvGatewayUdpListen); udp_listen_env && *udp_listen_env) {
        const auto [udp_host, udp_port] = parse_listen(std::string_view(udp_listen_env), 0);
        udp_listen_host_ = udp_host;
        udp_listen_port_ = udp_port;
    }

    if (const char* udp_secret_env = std::getenv(kEnvGatewayUdpBindSecret); udp_secret_env && *udp_secret_env) {
        udp_bind_secret_ = udp_secret_env;
    }

    udp_bind_ttl_ms_ = parse_env_u32_bounded(
        kEnvGatewayUdpBindTtlMs,
        kDefaultUdpBindTtlMs,
        1000,
        120000,
        "GatewayApp invalid GATEWAY_UDP_BIND_TTL_MS; using default"
    );

    udp_bind_fail_window_ms_ = parse_env_u32_bounded(
        kEnvGatewayUdpBindFailWindowMs,
        kDefaultUdpBindFailWindowMs,
        1000,
        120000,
        "GatewayApp invalid GATEWAY_UDP_BIND_FAIL_WINDOW_MS; using default"
    );

    udp_bind_fail_limit_ = parse_env_u32_bounded(
        kEnvGatewayUdpBindFailLimit,
        kDefaultUdpBindFailLimit,
        2,
        100,
        "GatewayApp invalid GATEWAY_UDP_BIND_FAIL_LIMIT; using default"
    );

    udp_bind_block_ms_ = parse_env_u32_bounded(
        kEnvGatewayUdpBindBlockMs,
        kDefaultUdpBindBlockMs,
        1000,
        300000,
        "GatewayApp invalid GATEWAY_UDP_BIND_BLOCK_MS; using default"
    );

    if (!kGatewayUdpIngressBuildEnabled && udp_listen_port_ != 0) {
        server::core::log::warn("GatewayApp UDP ingress build flag is OFF; ignoring GATEWAY_UDP_LISTEN");
        udp_listen_host_.clear();
        udp_listen_port_ = 0;
    }

    udp_bind_abuse_guard_.configure(udp_bind_fail_window_ms_, udp_bind_fail_limit_, udp_bind_block_ms_);

    allow_anonymous_ = true;
    if (const char* anonymous_env = std::getenv(kEnvAllowAnonymous); anonymous_env && *anonymous_env) {
        allow_anonymous_ = (std::string_view(anonymous_env) != "0");
    }

    server::core::log::info("GatewayApp configured: gateway_id=" + gateway_id_
        + " listen=" + listen_host_ + ":" + std::to_string(listen_port_)
        + " udp_listen="
        + (udp_listen_port_ == 0 ? std::string("disabled") : (udp_listen_host_ + ":" + std::to_string(udp_listen_port_)))
        + " udp_bind_ttl_ms=" + std::to_string(udp_bind_ttl_ms_)
        + " udp_bind_fail_window_ms=" + std::to_string(udp_bind_fail_window_ms_)
        + " udp_bind_fail_limit=" + std::to_string(udp_bind_fail_limit_)
        + " udp_bind_block_ms=" + std::to_string(udp_bind_block_ms_)
        + " udp_ingress_feature=" + std::string(kGatewayUdpIngressBuildEnabled ? "on" : "off")
        + " backend_connect_timeout_ms=" + std::to_string(backend_connect_timeout_ms_)
        + " backend_send_queue_max_bytes=" + std::to_string(backend_send_queue_max_bytes_)
        + " allow_anonymous=" + std::string(allow_anonymous_ ? "1" : "0"));
}

void GatewayApp::configure_infrastructure() {
    const char* redis_env = std::getenv(kEnvRedisUri);
    redis_uri_ = redis_env ? redis_env : kDefaultRedisUri;

    try {
        server::storage::redis::Options opts;
        redis_client_ = server::storage::redis::make_redis_client(redis_uri_, opts);
        
        if (redis_client_) {
             std::string registry_prefix = kDefaultServerRegistryPrefix;
             if (const char* v = std::getenv(kEnvServerRegistryPrefix); v && *v) {
                 registry_prefix = v;
             }

             std::chrono::seconds registry_ttl{30};
             if (const char* v = std::getenv(kEnvServerRegistryTtl); v && *v) {
                 try {
                     auto parsed = std::stoul(v);
                     if (parsed > 0) {
                         registry_ttl = std::chrono::seconds{static_cast<long long>(parsed)};
                     }
                 } catch (...) {
                     server::core::log::warn("GatewayApp invalid SERVER_REGISTRY_TTL; using default");
                 }
             }

              auto state_client = server::state::make_redis_state_client(redis_client_);
              backend_registry_ = std::make_unique<server::state::RedisInstanceStateBackend>(
                  state_client,
                  std::move(registry_prefix),
                  registry_ttl
              );
              
              session_directory_ = std::make_unique<SessionDirectory>(
                  redis_client_,
                  "gateway/session/",
                  std::chrono::seconds(600) // 10 minutes session stickiness
              );

             server::core::log::info("GatewayApp Redis client initialised");
         } else {
            server::core::log::error("GatewayApp failed to create Redis client (REDIS_URI redacted)");
         }
    } catch (const std::exception& e) {
        server::core::log::error(std::string("GatewayApp infrastructure init failed: ") + e.what());
    }
}

void GatewayApp::start_listener() {
    using tcp = boost::asio::ip::tcp;

    boost::system::error_code ec;
    boost::asio::ip::address address = boost::asio::ip::address_v4::any();
    if (!listen_host_.empty()) {
        auto parsed = boost::asio::ip::make_address(listen_host_, ec);
        if (!ec) {
            address = parsed;
        } else {
            server::core::log::warn("GatewayApp failed to parse listen address; defaulting to 0.0.0.0");
        }
    }

    tcp::endpoint endpoint{address, listen_port_};
    listener_ = std::make_shared<server::core::net::TransportListener>(
        hive_,
        endpoint,
        [authenticator = authenticator_, this](std::shared_ptr<server::core::net::Hive> hive) {
            return std::make_shared<GatewayConnection>(std::move(hive), authenticator, *this);
        });

    if (listener_->is_stopped()) {
        throw std::runtime_error("GatewayApp listener failed to start");
    }

    listener_->start();
    auto bound = listener_->local_endpoint();
    server::core::log::info("GatewayApp listening on " + bound.address().to_string() + ":" + std::to_string(bound.port()));
}

void GatewayApp::start_udp_listener() {
    if (!kGatewayUdpIngressBuildEnabled) {
        udp_enabled_.store(false, std::memory_order_relaxed);
        return;
    }

    if (udp_listen_port_ == 0) {
        udp_enabled_.store(false, std::memory_order_relaxed);
        return;
    }

    if (udp_bind_secret_.empty()) {
        server::core::log::warn("GatewayApp UDP bind secret is empty; UDP disabled");
        udp_enabled_.store(false, std::memory_order_relaxed);
        return;
    }

    boost::system::error_code ec;
    auto address = boost::asio::ip::make_address(udp_listen_host_.empty() ? "0.0.0.0" : udp_listen_host_, ec);
    if (ec) {
        server::core::log::warn("GatewayApp failed to parse UDP listen address; UDP disabled");
        udp_enabled_.store(false, std::memory_order_relaxed);
        return;
    }

    auto socket = std::make_unique<boost::asio::ip::udp::socket>(io_);
    socket->open(boost::asio::ip::udp::v4(), ec);
    if (ec) {
        server::core::log::warn("GatewayApp failed to open UDP socket; UDP disabled");
        udp_enabled_.store(false, std::memory_order_relaxed);
        return;
    }

    socket->set_option(boost::asio::socket_base::reuse_address(true), ec);
    if (ec) {
        server::core::log::warn("GatewayApp failed to set UDP reuse_address; UDP disabled");
        udp_enabled_.store(false, std::memory_order_relaxed);
        return;
    }

    socket->bind(boost::asio::ip::udp::endpoint{address, udp_listen_port_}, ec);
    if (ec) {
        server::core::log::warn("GatewayApp failed to bind UDP socket; UDP disabled");
        udp_enabled_.store(false, std::memory_order_relaxed);
        return;
    }

    udp_socket_ = std::move(socket);
    udp_enabled_.store(true, std::memory_order_relaxed);
    do_udp_receive();
    server::core::log::info("GatewayApp UDP listening on " + address.to_string() + ":" + std::to_string(udp_listen_port_));
}

void GatewayApp::stop_udp_listener() {
    udp_enabled_.store(false, std::memory_order_relaxed);
    if (!udp_socket_) {
        return;
    }

    boost::system::error_code ec;
    udp_socket_->cancel(ec);
    udp_socket_->close(ec);
    udp_socket_.reset();
}

void GatewayApp::do_udp_receive() {
    if (!kGatewayUdpIngressBuildEnabled) {
        return;
    }

    if (!udp_socket_) {
        return;
    }

    udp_socket_->async_receive_from(
        boost::asio::buffer(udp_read_buffer_),
        udp_remote_endpoint_,
        [this](const boost::system::error_code& ec, std::size_t bytes) {
            if (ec) {
                if (ec != boost::asio::error::operation_aborted) {
                    (void)udp_receive_error_total_.fetch_add(1, std::memory_order_relaxed);
                    do_udp_receive();
                }
                return;
            }

            if (bytes > 0) {
                (void)udp_packets_total_.fetch_add(1, std::memory_order_relaxed);
            }

            namespace proto = server::core::protocol;
            if (bytes < proto::k_header_bytes) {
                (void)udp_receive_error_total_.fetch_add(1, std::memory_order_relaxed);
                do_udp_receive();
                return;
            }

            proto::PacketHeader header{};
            proto::decode_header(udp_read_buffer_.data(), header);
            const auto body_len = static_cast<std::size_t>(header.length);
            if (body_len != (bytes - proto::k_header_bytes)) {
                (void)udp_receive_error_total_.fetch_add(1, std::memory_order_relaxed);
                do_udp_receive();
                return;
            }

            const auto payload = std::span<const std::uint8_t>(
                udp_read_buffer_.data() + proto::k_header_bytes,
                body_len
            );
            const auto now_ms = unix_time_ms();

            if (header.msg_id == server::protocol::MSG_UDP_BIND_REQ) {
                const auto remote_key = endpoint_key(udp_remote_endpoint_);
                const auto block_state = udp_bind_abuse_guard_.block_state(remote_key, now_ms);
                if (block_state.blocked) {
                    (void)udp_bind_rate_limit_reject_total_.fetch_add(1, std::memory_order_relaxed);
                    (void)udp_bind_reject_total_.fetch_add(1, std::memory_order_relaxed);
                    auto frame = make_udp_bind_res_frame(
                        server::core::protocol::errc::SERVER_BUSY,
                        std::string_view{},
                        0,
                        0,
                        std::string_view{},
                        "bind temporarily blocked",
                        header.seq
                    );
                    send_udp_datagram(std::move(frame), udp_remote_endpoint_);
                    do_udp_receive();
                    return;
                }

                auto record_bind_failure = [&]() {
                    if (udp_bind_abuse_guard_.record_failure(remote_key, now_ms)) {
                        (void)udp_bind_block_total_.fetch_add(1, std::memory_order_relaxed);
                    }
                };

                ParsedUdpBindRequest req{};
                if (!parse_udp_bind_req(payload, req)) {
                    (void)udp_bind_reject_total_.fetch_add(1, std::memory_order_relaxed);
                    record_bind_failure();
                    auto frame = make_udp_bind_res_frame(
                        server::core::protocol::errc::INVALID_PAYLOAD,
                        std::string_view{},
                        0,
                        0,
                        std::string_view{},
                        "invalid bind payload",
                        header.seq
                    );
                    send_udp_datagram(std::move(frame), udp_remote_endpoint_);
                    do_udp_receive();
                    return;
                }

                UdpBindTicket ticket{};
                std::string message;
                const auto code = apply_udp_bind_request(req, udp_remote_endpoint_, ticket, message);
                if (code == 0) {
                    (void)udp_bind_success_total_.fetch_add(1, std::memory_order_relaxed);
                    udp_bind_abuse_guard_.record_success(remote_key);
                } else {
                    (void)udp_bind_reject_total_.fetch_add(1, std::memory_order_relaxed);
                    record_bind_failure();
                }

                auto frame = (code == 0)
                    ? make_udp_bind_res_frame(code, ticket, message, header.seq)
                    : make_udp_bind_res_frame(code,
                                              req.session_id,
                                              req.nonce,
                                              req.expires_unix_ms,
                                              req.token,
                                              message,
                                              header.seq);
                send_udp_datagram(std::move(frame), udp_remote_endpoint_);
                do_udp_receive();
                return;
            }

            GatewayApp::BackendConnectionPtr bound_session;
            std::string bound_session_id;
            {
                std::lock_guard<std::mutex> lock(session_mutex_);
                for (auto& [sid, state] : sessions_) {
                    if (state.udp_bound && state.udp_endpoint == udp_remote_endpoint_) {
                        bound_session = state.session;
                        bound_session_id = sid;
                        break;
                    }
                }
            }

            if (!bound_session) {
                (void)udp_bind_reject_total_.fetch_add(1, std::memory_order_relaxed);
                auto frame = make_udp_bind_res_frame(
                    server::core::protocol::errc::UNAUTHORIZED,
                    std::string_view{},
                    0,
                    0,
                    std::string_view{},
                    "udp session not bound",
                    header.seq
                );
                send_udp_datagram(std::move(frame), udp_remote_endpoint_);
                do_udp_receive();
                return;
            }

            const bool is_game_opcode = !server::protocol::opcode_name(header.msg_id).empty();
            const bool is_core_opcode = !server::core::protocol::opcode_name(header.msg_id).empty();
            if (!is_game_opcode && !is_core_opcode) {
                (void)udp_bind_reject_total_.fetch_add(1, std::memory_order_relaxed);
                auto frame = make_udp_bind_res_frame(
                    server::core::protocol::errc::UNKNOWN_MSG_ID,
                    std::string_view{},
                    0,
                    0,
                    std::string_view{},
                    "unknown udp msg_id",
                    header.seq
                );
                send_udp_datagram(std::move(frame), udp_remote_endpoint_);
                do_udp_receive();
                return;
            }

            const auto policy = is_game_opcode
                ? server::protocol::opcode_policy(header.msg_id)
                : server::core::protocol::opcode_policy(header.msg_id);
            if (!server::core::protocol::transport_allows(policy.transport, server::core::protocol::TransportKind::kUdp)) {
                (void)udp_bind_reject_total_.fetch_add(1, std::memory_order_relaxed);
                auto frame = make_udp_bind_res_frame(
                    server::core::protocol::errc::FORBIDDEN,
                    std::string_view{},
                    0,
                    0,
                    std::string_view{},
                    "opcode not allowed on udp",
                    header.seq
                );
                send_udp_datagram(std::move(frame), udp_remote_endpoint_);
                do_udp_receive();
                return;
            }

            if (policy.delivery == server::core::protocol::DeliveryClass::kUnreliableSequenced) {
                gateway::UdpSequencedMetrics::UpdateResult update{};
                {
                    std::lock_guard<std::mutex> lock(session_mutex_);
                    auto it = sessions_.find(bound_session_id);
                    if (it != sessions_.end()
                        && it->second.udp_bound
                        && it->second.udp_endpoint == udp_remote_endpoint_) {
                        update = it->second.udp_sequenced_metrics.on_packet(header.seq, now_ms);
                    }
                }

                if (!update.accepted) {
                    (void)udp_replay_drop_total_.fetch_add(1, std::memory_order_relaxed);
                    (void)udp_bind_reject_total_.fetch_add(1, std::memory_order_relaxed);

                    if (update.duplicate) {
                        (void)udp_duplicate_drop_total_.fetch_add(1, std::memory_order_relaxed);
                        (void)udp_retransmit_total_.fetch_add(1, std::memory_order_relaxed);
                    } else if (update.reordered) {
                        (void)udp_reorder_drop_total_.fetch_add(1, std::memory_order_relaxed);
                    }

                    auto frame = make_udp_bind_res_frame(
                        server::core::protocol::errc::FORBIDDEN,
                        std::string_view{},
                        0,
                        0,
                        std::string_view{},
                        "stale sequenced udp packet",
                        header.seq
                    );
                    send_udp_datagram(std::move(frame), udp_remote_endpoint_);
                    do_udp_receive();
                    return;
                }

                if (update.estimated_lost_packets > 0) {
                    (void)udp_loss_estimated_total_.fetch_add(update.estimated_lost_packets, std::memory_order_relaxed);
                }
                if (update.jitter_ms > 0) {
                    udp_jitter_ms_last_.store(update.jitter_ms, std::memory_order_relaxed);
                }
            }

            bound_session->send(udp_read_buffer_.data(), bytes);
            (void)udp_forward_total_.fetch_add(1, std::memory_order_relaxed);
            switch (policy.delivery) {
                case server::core::protocol::DeliveryClass::kReliableOrdered:
                    (void)udp_forward_reliable_ordered_total_.fetch_add(1, std::memory_order_relaxed);
                    break;
                case server::core::protocol::DeliveryClass::kReliable:
                    (void)udp_forward_reliable_total_.fetch_add(1, std::memory_order_relaxed);
                    break;
                case server::core::protocol::DeliveryClass::kUnreliableSequenced:
                    (void)udp_forward_unreliable_sequenced_total_.fetch_add(1, std::memory_order_relaxed);
                    break;
            }
            do_udp_receive();
        });
}

} // namespace gateway
