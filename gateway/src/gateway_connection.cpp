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

    open_backend_session();
}

void GatewayConnection::on_disconnect() {
    closing_.store(true, std::memory_order_relaxed);
    if (!session_id_.empty()) {
        app_.close_backend_session(session_id_);
    }
    backend_session_.reset();
    server::core::log::info("GatewayConnection disconnected");
}

void GatewayConnection::on_read(const std::uint8_t* data, std::size_t length) {
    if (length == 0) {
        return;
    }

    const std::string raw_message(reinterpret_cast<const char*>(data), length);

    if (!authenticated_) {
        auth::AuthRequest request{};
        
        bool is_login_frame = false;
        if (length >= server::core::protocol::k_header_bytes) {
            server::core::protocol::FrameHeader header{};
            server::core::protocol::decode_header(data, header);
            if (header.msg_id == game_proto::MSG_LOGIN_REQ) {
                is_login_frame = true;
                auto payload = std::span<const std::uint8_t>(data + server::core::protocol::k_header_bytes, 
                                                           length - server::core::protocol::k_header_bytes);
                std::string user, token;
                if (server::core::protocol::read_lp_utf8(payload, user)) {
                    request.client_id = user;
                    server::core::protocol::read_lp_utf8(payload, token);
                    request.token = token;
                }
            }
        }

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

        open_backend_session();
    }

    if (!backend_session_) {
        server::core::log::warn("GatewayConnection missing backend session; dropping payload");
        return;
    }

    std::vector<std::uint8_t> payload(data, data + length);
    send_to_backend(payload);
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

void GatewayConnection::open_backend_session() {
    if (backend_session_) {
        return;
    }

    auto self = std::static_pointer_cast<GatewayConnection>(shared_from_this());
    std::weak_ptr<GatewayConnection> weak_self = self;
    backend_session_ = app_.create_backend_session(client_id_, weak_self);
    if (!backend_session_) {
        server::core::log::error("GatewayConnection failed to create backend session");
        stop();
        return;
    }

    session_id_ = backend_session_->session_id();
}

void GatewayConnection::send_to_backend(const std::vector<std::uint8_t>& payload) {
    if (!backend_session_) {
        return;
    }
    backend_session_->send(payload);
}

} // namespace gateway
