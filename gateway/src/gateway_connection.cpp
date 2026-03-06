#include "gateway/gateway_connection.hpp"

#include <atomic>
#include <string>
#include <utility>

#include "gateway/gateway_app.hpp"
#include "server/core/util/log.hpp"
#include "server/core/protocol/packet.hpp"
#include "server/core/protocol/system_opcodes.hpp"
#include "server/protocol/game_opcodes.hpp"

/**
 * @brief 클라이언트 세션의 핸드셰이크/인증/브리지 전환 구현입니다.
 *
 * 첫 로그인 프레임을 안전하게 완성 파싱한 뒤 backend를 연결하고,
 * 완료 후에는 payload를 투명 전달해 gateway가 애플리케이션 데이터를 변형하지 않게 유지합니다.
 */
namespace gateway {
namespace game_proto = server::protocol;
namespace core_proto = server::core::protocol;

namespace {

std::vector<std::uint8_t> make_control_frame(const core_proto::PacketHeader& header,
                                             std::uint16_t msg_id,
                                             std::span<const std::uint8_t> payload = {}) {
    std::vector<std::uint8_t> frame(core_proto::k_header_bytes + payload.size());
    core_proto::PacketHeader response{};
    response.length = static_cast<std::uint16_t>(payload.size());
    response.msg_id = msg_id;
    response.flags = header.flags;
    response.seq = header.seq;
    response.utc_ts_ms32 = header.utc_ts_ms32;
    core_proto::encode_header(response, frame.data());
    if (!payload.empty()) {
        std::memcpy(frame.data() + core_proto::k_header_bytes, payload.data(), payload.size());
    }
    return frame;
}

} // namespace

GatewayConnection::GatewayConnection(std::shared_ptr<server::core::net::Hive> hive,
                                     std::shared_ptr<auth::IAuthenticator> authenticator,
                                     GatewayApp& app)
    : server::core::net::TransportConnection(std::move(hive))
    , authenticator_(std::move(authenticator))
    , app_(app)
    , handshake_timer_(io()) {}

// 백엔드(게임 서버)로부터 수신한 데이터를 클라이언트에게 전달합니다.
// 게이트웨이는 투명한 프록시 역할을 하므로 데이터를 변조하지 않고 그대로 전달합니다.
void GatewayConnection::handle_backend_payload(std::vector<std::uint8_t> payload) {
    if (payload.empty() || closing_.load(std::memory_order_relaxed)) {
        return;
    }
    async_send(std::move(payload));
}

void GatewayConnection::handle_backend_close(const std::string& reason) {
    bool expected = false;
    if (!closing_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    if (!reason.empty()) {
        server::core::log::info("GatewayConnection backend closed: " + reason);
    }
    stop();
}

void GatewayConnection::on_connect() {
    const auto admission = app_.admit_ingress_connection();
    if (admission != GatewayApp::IngressAdmission::kAccept) {
        server::core::log::warn(
            "GatewayConnection rejected by ingress admission: reason="
            + std::string(GatewayApp::ingress_admission_name(admission))
        );
        stop();
        return;
    }

    app_.record_connection_accept();

    try {
        const auto remote = socket().remote_endpoint();
        remote_ip_ = remote.address().to_string();
        server::core::log::info("GatewayConnection accepted from " + remote_ip_);
    } catch (const std::exception& ex) {
        remote_ip_.clear();
        server::core::log::warn(std::string("GatewayConnection remote endpoint unknown: ") + ex.what());
    }

    // Handshake flow:
    // - Wait for a full first frame (TCP fragmentation-safe).
    // - Parse MSG_LOGIN_REQ to extract identity.
    // - Authenticate (pluggable).
    // - Select backend and begin transparent bridging.
    phase_ = Phase::kWaitingForLogin;
    prebuffer_.clear();
    start_handshake_deadline();
}

void GatewayConnection::on_disconnect() {
    closing_.store(true, std::memory_order_relaxed);

    (void)handshake_timer_.cancel();

    if (!session_id_.empty()) {
        app_.close_backend_connection(session_id_);
    }
    backend_connection_.reset();
    server::core::log::info("GatewayConnection disconnected");
}

void GatewayConnection::on_read(const std::uint8_t* data, std::size_t length) {
    if (length == 0) {
        return;
    }

    if (phase_ == Phase::kWaitingForLogin) {
        constexpr std::size_t kMaxHandshakeBytes = 64 * 1024;
        if (prebuffer_.size() + length > kMaxHandshakeBytes) {
            server::core::log::warn("GatewayConnection handshake buffer limit exceeded; closing");
            stop();
            return;
        }

        prebuffer_.insert(prebuffer_.end(), data, data + length);
        (void)try_finish_handshake();
        return;
    }

    if (!backend_connection_) {
        server::core::log::warn("GatewayConnection missing backend connection; dropping payload");
        return;
    }

    // 호출자 측 임시 vector 생성을 피하고 BackendConnection 큐에서 1회 복사로 마무리한다.
    send_to_backend(data, length);
}

void GatewayConnection::on_error(const boost::system::error_code& ec) {
    using boost::asio::error::eof;
    using boost::asio::error::operation_aborted;
    using boost::asio::error::connection_reset;
    if (ec == eof || ec == operation_aborted || ec == connection_reset) {
        server::core::log::info(std::string("GatewayConnection closed: ") + ec.message());
        return;
    }
    server::core::log::warn(std::string("GatewayConnection error: ") + ec.message());
}

void GatewayConnection::start_handshake_deadline() {
    (void)handshake_timer_.cancel();
    handshake_timer_.expires_after(std::chrono::seconds(3));

    auto self = std::static_pointer_cast<GatewayConnection>(shared_from_this());
    std::weak_ptr<GatewayConnection> weak_self = self;
    handshake_timer_.async_wait([weak_self](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted) {
            return;
        }
        auto locked = weak_self.lock();
        if (!locked) {
            return;
        }
        if (locked->closing_.load(std::memory_order_relaxed) || locked->is_stopped()) {
            return;
        }
        if (locked->phase_ == Phase::kWaitingForLogin) {
            server::core::log::warn("GatewayConnection handshake timeout; closing");
            locked->stop();
        }
    });
}

bool GatewayConnection::try_finish_handshake() {
    constexpr std::size_t kMaxLoginPayloadBytes = 32 * 1024;

    if (phase_ != Phase::kWaitingForLogin) {
        return true;
    }
    while (phase_ == Phase::kWaitingForLogin) {
        if (prebuffer_.size() < core_proto::k_header_bytes) {
            return false;
        }

        core_proto::PacketHeader header{};
        core_proto::decode_header(prebuffer_.data(), header);

        if (header.length > kMaxLoginPayloadBytes) {
            server::core::log::warn("GatewayConnection login payload too large; closing");
            stop();
            return true;
        }

        const std::size_t frame_bytes =
            core_proto::k_header_bytes + static_cast<std::size_t>(header.length);
        if (prebuffer_.size() < frame_bytes) {
            return false;
        }

        const auto segment_type = core_proto::classify_segment_type(header.msg_id);
        if (segment_type != core_proto::SegmentType::kApplicationPayload) {
            if (header.msg_id == core_proto::MSG_PING) {
                const auto payload = std::span<const std::uint8_t>(
                    prebuffer_.data() + core_proto::k_header_bytes,
                    static_cast<std::size_t>(header.length));
                async_send(make_control_frame(header, core_proto::MSG_PONG, payload));
                prebuffer_.erase(prebuffer_.begin(), prebuffer_.begin() + static_cast<std::ptrdiff_t>(frame_bytes));
                continue;
            }
            if (header.msg_id == core_proto::MSG_PONG) {
                prebuffer_.erase(prebuffer_.begin(), prebuffer_.begin() + static_cast<std::ptrdiff_t>(frame_bytes));
                continue;
            }

            server::core::log::warn(
                std::string("GatewayConnection expected application payload at handshake; got system msg_id=")
                + std::to_string(header.msg_id)
            );
            stop();
            return true;
        }

        if (header.msg_id != game_proto::MSG_LOGIN_REQ) {
            server::core::log::warn(
                std::string("GatewayConnection expected MSG_LOGIN_REQ first; got msg_id=")
                + std::to_string(header.msg_id)
            );
            stop();
            return true;
        }

        auto payload = std::span<const std::uint8_t>(
            prebuffer_.data() + core_proto::k_header_bytes,
            static_cast<std::size_t>(header.length)
        );

        std::string user;
        std::string token;
        if (!core_proto::read_lp_utf8(payload, user) || !core_proto::read_lp_utf8(payload, token)) {
            server::core::log::warn("GatewayConnection invalid login payload; closing");
            stop();
            return true;
        }

        auth::AuthRequest request{};
        request.client_id = user;
        request.token = token;
        request.remote_address = remote_ip_;

        if (authenticator_) {
            last_auth_result_ = authenticator_->authenticate(request);
        } else {
            last_auth_result_.success = true;
            last_auth_result_.subject = request.client_id.empty() ? "anonymous" : request.client_id;
            last_auth_result_.failure_reason.clear();
        }

        if (!last_auth_result_.success) {
            server::core::log::warn(
                std::string("GatewayConnection authentication failed: ") + last_auth_result_.failure_reason
            );
            stop();
            return true;
        }

        std::string routing_key = !request.client_id.empty() ? request.client_id : last_auth_result_.subject;
        if (routing_key.empty() || routing_key == "guest") {
            routing_key = "anonymous";
        }

        if (!app_.allow_anonymous()) {
            if (request.token.empty()) {
                server::core::log::warn("GatewayConnection anonymous login disabled: token required");
                stop();
                return true;
            }
            if (routing_key == "anonymous") {
                server::core::log::warn("GatewayConnection anonymous login disabled");
                stop();
                return true;
            }
        }

        client_id_ = std::move(routing_key);

        open_backend_connection();
        if (!backend_connection_) {
            stop();
            return true;
        }

        if (auto udp_bind_ticket = app_.make_udp_bind_ticket_frame(session_id_)) {
            async_send(std::move(*udp_bind_ticket));
        }

        (void)handshake_timer_.cancel();

        // Forward the raw bytes exactly as received (transparent proxy).
        send_to_backend(std::move(prebuffer_));
        phase_ = Phase::kBridging;
        return true;
    }
    return false;
}

void GatewayConnection::open_backend_connection() {
    if (backend_connection_) {
        return;
    }

    if (client_id_.empty()) {
        client_id_ = "anonymous";
    }

    auto self = std::static_pointer_cast<GatewayConnection>(shared_from_this());
    std::weak_ptr<GatewayConnection> weak_self = self;
    backend_connection_ = app_.create_backend_connection(client_id_, weak_self);
    if (!backend_connection_) {
        server::core::log::error("GatewayConnection failed to create backend connection");
        stop();
        return;
    }

    session_id_ = backend_connection_->session_id();
}

void GatewayConnection::send_to_backend(std::vector<std::uint8_t> payload) {
    if (!backend_connection_) {
        return;
    }
    backend_connection_->send(std::move(payload));
}

void GatewayConnection::send_to_backend(const std::uint8_t* data, std::size_t length) {
    if (!backend_connection_ || !data || length == 0) {
        return;
    }
    backend_connection_->send(data, length);
}

} // namespace gateway
