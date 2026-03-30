#include "gateway/gateway_connection.hpp"

#include <atomic>
#include <array>
#include <string>
#include <utility>

#include <openssl/sha.h>

#include "gateway_app_access.hpp"
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

constexpr std::string_view kResumeTokenPrefix = "resume:";
constexpr std::string_view kResumeRoutingPrefix = "resume-hash:";

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

std::string sha256_hex(std::string_view input) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    if (SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest.data()) == nullptr) {
        return {};
    }

    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (unsigned char byte : digest) {
        out.push_back(kHexDigits[(byte >> 4) & 0x0F]);
        out.push_back(kHexDigits[byte & 0x0F]);
    }
    return out;
}

std::string make_resume_routing_key(std::string_view raw_resume_token) {
    const std::string digest = sha256_hex(raw_resume_token);
    if (digest.empty()) {
        return {};
    }
    return std::string(kResumeRoutingPrefix) + digest;
}

bool is_resume_routing_key(std::string_view value) {
    return value.rfind(kResumeRoutingPrefix, 0) == 0;
}

bool read_varint(std::span<const std::uint8_t> input, std::size_t& offset, std::uint64_t& value) {
    value = 0;
    unsigned int shift = 0;
    while (offset < input.size()) {
        const std::uint8_t byte = input[offset++];
        value |= static_cast<std::uint64_t>(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0) {
            return true;
        }
        shift += 7;
        if (shift > 63) {
            return false;
        }
    }
    return false;
}

bool skip_wire_value(std::span<const std::uint8_t> input, std::size_t& offset, std::uint64_t wire_type) {
    switch (wire_type) {
    case 0: {
        std::uint64_t ignored = 0;
        return read_varint(input, offset, ignored);
    }
    case 2: {
        std::uint64_t len = 0;
        if (!read_varint(input, offset, len) || offset + len > input.size()) {
            return false;
        }
        offset += static_cast<std::size_t>(len);
        return true;
    }
    default:
        return false;
    }
}

std::optional<std::string> parse_resume_token_from_login_res(std::span<const std::uint8_t> payload) {
    std::size_t offset = 0;
    while (offset < payload.size()) {
        std::uint64_t tag = 0;
        if (!read_varint(payload, offset, tag)) {
            return std::nullopt;
        }

        const std::uint64_t field_number = tag >> 3;
        const std::uint64_t wire_type = tag & 0x07u;
        if (field_number == 6 && wire_type == 2) {
            std::uint64_t len = 0;
            if (!read_varint(payload, offset, len) || offset + len > payload.size()) {
                return std::nullopt;
            }
            return std::string(reinterpret_cast<const char*>(payload.data() + offset), static_cast<std::size_t>(len));
        }

        if (!skip_wire_value(payload, offset, wire_type)) {
            return std::nullopt;
        }
    }
    return std::nullopt;
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

    constexpr std::size_t kMaxInspectableBytes = 256 * 1024;
    if (backend_prebuffer_.size() + payload.size() > kMaxInspectableBytes) {
        backend_prebuffer_.clear();
    }

    backend_prebuffer_.insert(backend_prebuffer_.end(), payload.begin(), payload.end());

    std::vector<std::uint8_t> tcp_payload;
    while (backend_prebuffer_.size() >= core_proto::k_header_bytes) {
        core_proto::PacketHeader header{};
        core_proto::decode_header(backend_prebuffer_.data(), header);
        const std::size_t frame_bytes =
            core_proto::k_header_bytes + static_cast<std::size_t>(header.length);
        if (backend_prebuffer_.size() < frame_bytes) {
            break;
        }

        const auto frame_span = std::span<const std::uint8_t>(backend_prebuffer_.data(), frame_bytes);
        const auto body = std::span<const std::uint8_t>(
            backend_prebuffer_.data() + core_proto::k_header_bytes,
            static_cast<std::size_t>(header.length));

        if (header.msg_id == game_proto::MSG_LOGIN_RES && backend_connection_) {
            if (const auto resume_token = parse_resume_token_from_login_res(body);
                resume_token.has_value() && !resume_token->empty()) {
                const std::string routing_key = make_resume_routing_key(*resume_token);
                if (!routing_key.empty() && !backend_instance_id_.empty()) {
                    GatewayAppAccess::register_resume_routing_key(app_, routing_key, backend_instance_id_);
                }
            }
        }

        const bool delivered_direct =
            !session_id_.empty()
            && GatewayAppAccess::try_send_direct_client_frame(app_, session_id_, header.msg_id, frame_span);
        if (!delivered_direct) {
            tcp_payload.insert(tcp_payload.end(), frame_span.begin(), frame_span.end());
        }

        backend_prebuffer_.erase(
            backend_prebuffer_.begin(),
            backend_prebuffer_.begin() + static_cast<std::ptrdiff_t>(frame_bytes));
    }

    if (!tcp_payload.empty()) {
        async_send(std::move(tcp_payload));
    }
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
    const auto admission = GatewayAppAccess::admit_ingress_connection(app_);
    if (admission != GatewayAppAccess::IngressAdmission::kAccept) {
        server::core::log::warn(
            "GatewayConnection rejected by ingress admission: reason="
            + std::string(GatewayAppAccess::ingress_admission_name(admission))
        );
        stop();
        return;
    }

    GatewayAppAccess::record_connection_accept(app_);

    try {
        const auto remote = socket().remote_endpoint();
        remote_ip_ = remote.address().to_string();
        server::core::log::info("GatewayConnection accepted from " + remote_ip_);
    } catch (const std::exception& ex) {
        remote_ip_.clear();
        server::core::log::warn(std::string("GatewayConnection remote endpoint unknown: ") + ex.what());
    }

    // 핸드셰이크 흐름:
    // - 첫 프레임이 완전히 올 때까지 기다린다(TCP 분할 수신에도 안전).
    // - `MSG_LOGIN_REQ`를 파싱해 식별자를 추출한다.
    // - 인증을 수행한다(플러그형).
    // - backend를 선택하고 투명 브리지를 시작한다.
    phase_ = Phase::kWaitingForLogin;
    prebuffer_.clear();
    start_handshake_deadline();
}

void GatewayConnection::on_disconnect() {
    closing_.store(true, std::memory_order_relaxed);

    (void)handshake_timer_.cancel();

    if (!session_id_.empty()) {
        GatewayAppAccess::close_backend_connection(app_, session_id_);
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

    // 호출자 쪽 임시 `vector` 생성을 피하고 `BackendConnection` 큐에서 1회 복사로 마무리한다.
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
        if (token.rfind(kResumeTokenPrefix, 0) == 0) {
            const std::string raw_resume_token(token.substr(kResumeTokenPrefix.size()));
            if (!raw_resume_token.empty()) {
                const std::string resume_routing_key = make_resume_routing_key(raw_resume_token);
                if (!resume_routing_key.empty()) {
                    resume_routing_key_ = resume_routing_key;
                    routing_key = resume_routing_key_;
                }
            }
        }
        if (routing_key.empty() || routing_key == "guest") {
            routing_key = "anonymous";
        }

        if (!GatewayAppAccess::allow_anonymous(app_)) {
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

        if (auto udp_bind_ticket = GatewayAppAccess::make_udp_bind_ticket_frame(app_, session_id_)) {
            async_send(std::move(*udp_bind_ticket));
        }

        (void)handshake_timer_.cancel();

        // handshake를 통과한 첫 프레임은 받은 바이트 그대로 backend로 넘긴다.
        // gateway가 이 시점에 payload를 다시 구성하기 시작하면 "투명 브리지" 성질이 흐려진다.
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
    auto created_backend = GatewayAppAccess::create_backend_connection(app_, client_id_, weak_self);
    if (!created_backend.has_value() || !created_backend->session) {
        server::core::log::error("GatewayConnection failed to create backend connection");
        stop();
        return;
    }

    backend_connection_ = std::move(created_backend->session);
    session_id_ = std::move(created_backend->session_id);
    backend_instance_id_ = std::move(created_backend->backend_instance_id);
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

void GatewayConnection::inspect_backend_payload(std::span<const std::uint8_t> payload) {
    if (payload.empty()) {
        return;
    }

    constexpr std::size_t kMaxInspectableBytes = 256 * 1024;
    if (backend_prebuffer_.size() + payload.size() > kMaxInspectableBytes) {
        backend_prebuffer_.clear();
    }

    backend_prebuffer_.insert(backend_prebuffer_.end(), payload.begin(), payload.end());
    while (backend_prebuffer_.size() >= core_proto::k_header_bytes) {
        core_proto::PacketHeader header{};
        core_proto::decode_header(backend_prebuffer_.data(), header);
        const std::size_t frame_bytes =
            core_proto::k_header_bytes + static_cast<std::size_t>(header.length);
        if (backend_prebuffer_.size() < frame_bytes) {
            return;
        }

        if (header.msg_id == game_proto::MSG_LOGIN_RES && backend_connection_) {
            const auto body = std::span<const std::uint8_t>(
                backend_prebuffer_.data() + core_proto::k_header_bytes,
                static_cast<std::size_t>(header.length));
            if (const auto resume_token = parse_resume_token_from_login_res(body);
                resume_token.has_value() && !resume_token->empty()) {
                const std::string routing_key = make_resume_routing_key(*resume_token);
                if (!routing_key.empty() && !backend_instance_id_.empty()) {
                    GatewayAppAccess::register_resume_routing_key(app_, routing_key, backend_instance_id_);
                }
            }
        }

        backend_prebuffer_.erase(
            backend_prebuffer_.begin(),
            backend_prebuffer_.begin() + static_cast<std::ptrdiff_t>(frame_bytes));
    }
}

} // namespace gateway
