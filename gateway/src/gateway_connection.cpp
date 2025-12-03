#include "gateway/gateway_connection.hpp"

#include <atomic>
#include <string>
#include <utility>

#include "gateway/gateway_app.hpp"
#include "server/core/util/log.hpp"
#include "server/core/protocol/frame.hpp"
#include "server/protocol/game_opcodes.hpp"

namespace gateway {
namespace game_proto = server::protocol;

namespace {
// 게이트웨이 최초 메시지는 "client_id:opaque_token"을 기대하며,
// client_id가 비어 있으면 remote_ip 등으로 대체한다.
std::string extract_token_legacy(const std::string& message, std::string& client_id) {
    auto pos = message.find(':');
    if (pos == std::string::npos) {
        client_id.clear();
        return message;
    }
    client_id = message.substr(0, pos);
    return message.substr(pos + 1);
}
} // namespace

GatewayConnection::GatewayConnection(std::shared_ptr<server::core::net::Hive> hive,
                                     std::shared_ptr<auth::IAuthenticator> authenticator,
                                     GatewayApp& app)
    : server::core::net::Connection(std::move(hive))
    , authenticator_(std::move(authenticator))
    , app_(app) {}

void GatewayConnection::handle_backend_payload(std::vector<std::uint8_t> payload) {
    if (payload.empty() || closing_.load(std::memory_order_relaxed)) {
        return;
    }
    // gRPC 스트림에서 받은 raw 바이트를 TCP 세션으로 그대로 재전송한다.
    async_send(payload);
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
    std::string remote_ip;
    try {
        const auto remote = socket().remote_endpoint();
        remote_ip = remote.address().to_string();
        server::core::log::info("GatewayConnection accepted from " + remote_ip);
    } catch (const std::exception& ex) {
        server::core::log::warn(std::string("GatewayConnection remote endpoint unknown: ") + ex.what());
    }

    // pre-auth 단계에서는 remote_ip만으로 라우팅 정책(IP allowlist 등)을 적용할 수 있다.
    // 아직 클라이언트가 누구인지(ID)는 모르지만, 어디서 왔는지(IP)는 알 수 있습니다.
    auth::AuthRequest request{};
    request.remote_address = remote_ip;
    if (authenticator_) {
        last_auth_result_ = authenticator_->authenticate(request);
        authenticated_ = last_auth_result_.success;
    } else {
        authenticated_ = true;
        last_auth_result_.success = true;
        last_auth_result_.subject = request.client_id.empty() ? "anonymous" : request.client_id;
    }

    if (!authenticated_) {
        server::core::log::warn(std::string("GatewayConnection pre-auth failed: ")
            + last_auth_result_.failure_reason);
        stop();
        return;
    }

    client_id_ = !last_auth_result_.subject.empty() ? last_auth_result_.subject : remote_ip;
    if (client_id_.empty()) {
        client_id_ = "anonymous";
    }

    open_lb_session(std::string{});
}

void GatewayConnection::on_disconnect() {
    closing_.store(true, std::memory_order_relaxed);
    if (!session_id_.empty()) {
        app_.close_lb_session(session_id_);
    }
    lb_session_.reset();
    server::core::log::info("GatewayConnection disconnected");
}

void GatewayConnection::on_read(const std::uint8_t* data, std::size_t length) {
    if (length == 0) {
        return;
    }

    const std::string raw_message(reinterpret_cast<const char*>(data), length);

    // 첫 payload는 인증 토큰이므로 authenticator로 위임한 뒤 LB 세션을 연다.
    if (!authenticated_) {
        auth::AuthRequest request{};
        
        // 1. MSG_LOGIN_REQ 프레임인지 확인 (바이너리 프로토콜)
        bool is_login_frame = false;
        if (length >= server::core::protocol::k_header_bytes) {
            server::core::protocol::FrameHeader header{};
            server::core::protocol::decode_header(data, header);
            if (header.msg_id == game_proto::MSG_LOGIN_REQ) {
                is_login_frame = true;
                // Payload 파싱: [User(LP)][Token(LP)]
                auto payload = std::span<const std::uint8_t>(data + server::core::protocol::k_header_bytes, 
                                                           length - server::core::protocol::k_header_bytes);
                std::string user, token;
                if (server::core::protocol::read_lp_utf8(payload, user)) {
                    request.client_id = user;
                    // Token은 옵션
                    server::core::protocol::read_lp_utf8(payload, token);
                    request.token = token;
                }
            }
        }

        // 2. 아니면 레거시 "user:token" 포맷 시도
        if (!is_login_frame) {
            request.token = extract_token_legacy(raw_message, request.client_id);
        }

        try {
            request.remote_address = socket().remote_endpoint().address().to_string();
        } catch (...) {
            request.remote_address.clear();
        }

        if (authenticator_) {
            last_auth_result_ = authenticator_->authenticate(request);
            authenticated_ = last_auth_result_.success;
        } else {
            authenticated_ = true;
            last_auth_result_.success = true;
            last_auth_result_.subject = request.client_id.empty() ? "anonymous" : request.client_id;
        }

    if (!authenticated_) {
        server::core::log::warn(std::string("GatewayConnection authentication failed: ")
            + last_auth_result_.failure_reason);
            stop();
            return;
        }

        client_id_ = !request.client_id.empty() ? request.client_id : last_auth_result_.subject;
        if (client_id_.empty()) {
            client_id_ = "anonymous";
        }

        open_lb_session(raw_message);
        return;
    }

    if (!lb_session_) {
        server::core::log::warn("GatewayConnection missing LB session; dropping payload");
        return;
    }

    // 인증 이후에는 모든 클라이언트 바이트를 LB 세션으로 전달한다.
    // Gateway는 L7 프로토콜을 깊게 이해하지 않고, 단순히 바이트 스트림을 중계(Tunneling)합니다.
    std::vector<std::uint8_t> payload(data, data + length);
    send_to_lb(payload, gateway::lb::ROUTE_KIND_CLIENT_PAYLOAD);
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

// 인증이 끝나면 gRPC Stream을 열고 첫 payload(handshake)를 LB로 전달한다.
void GatewayConnection::open_lb_session(const std::string& handshake_payload) {
    if (lb_session_) {
        return;
    }

    auto self = std::static_pointer_cast<GatewayConnection>(shared_from_this());
    std::weak_ptr<GatewayConnection> weak_self = self;
    lb_session_ = app_.create_lb_session(client_id_, weak_self);
    if (!lb_session_) {
        server::core::log::error("GatewayConnection failed to create load balancer session");
        stop();
        return;
    }

    session_id_ = lb_session_->session_id();

    std::vector<std::uint8_t> payload(handshake_payload.begin(), handshake_payload.end());
    send_to_lb(payload, gateway::lb::ROUTE_KIND_CLIENT_HELLO);
}

// 모든 gateway→LB 메시지는 kind에 따라 HELLO/일반 payload/close 로 구분된다.
void GatewayConnection::send_to_lb(const std::vector<std::uint8_t>& payload, gateway::lb::RouteMessageKind kind) {
    if (!lb_session_) {
        return;
    }
    // LB 세션은 내부적으로 mutex로 직렬화되므로 여기서는 간단히 proxy만 수행한다.
    if (!lb_session_->send(kind, payload)) {
        server::core::log::warn("GatewayConnection failed to forward payload to load balancer");
    }
}

} // namespace gateway
