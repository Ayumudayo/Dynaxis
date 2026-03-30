#include "gateway/gateway_app.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/hmac.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/udp.hpp>

#include "gateway/direct_egress_route.hpp"
#include "gateway_app_access.hpp"
#include "gateway_app_state.hpp"
#include "server/core/net/rudp/rudp_engine.hpp"
#include "server/core/protocol/packet.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/protocol/system_opcodes.hpp"
#include "server/core/realtime/direct_bind.hpp"
#include "server/core/runtime_metrics.hpp"
#include "server/core/util/log.hpp"
#include "server/protocol/game_opcodes.hpp"

namespace gateway {

namespace {

std::uint64_t unix_time_ms() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    return static_cast<std::uint64_t>(now.count());
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

std::string endpoint_key(const boost::asio::ip::udp::endpoint& endpoint) {
    return endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
}

std::string udp_frame_summary(std::span<const std::uint8_t> frame) {
    namespace proto = server::core::protocol;

    std::ostringstream oss;
    oss << "bytes=" << frame.size();
    if (frame.size() < proto::k_header_bytes) {
        oss << " short";
        return oss.str();
    }

    proto::PacketHeader header{};
    proto::decode_header(frame.data(), header);
    oss << " msg_id=" << header.msg_id
        << " seq=" << header.seq
        << " payload_len=" << header.length;
    return oss.str();
}

} // namespace

std::uint32_t GatewayAppAccess::udp_bind_retry_delay_ms(const GatewayApp& app, std::uint32_t attempt) {
    const auto capped_attempt = std::min<std::uint32_t>(attempt, 8);
    const auto factor = 1ull << (capped_attempt == 0 ? 0 : (capped_attempt - 1));
    const auto base_delay = static_cast<std::uint64_t>(app.impl_->udp_bind_retry_backoff_ms_) * factor;
    const auto bounded_delay = std::min<std::uint64_t>(base_delay, app.impl_->udp_bind_retry_backoff_max_ms_);
    return static_cast<std::uint32_t>(bounded_delay);
}

std::string GatewayAppAccess::make_udp_bind_token(const GatewayApp& app,
                                                  std::string_view session_id,
                                                  std::uint64_t nonce,
                                                  std::uint64_t expires_unix_ms) {
    if (app.impl_->udp_bind_secret_.empty()) {
        return {};
    }

    const auto signing_input = make_bind_signing_input(session_id, nonce, expires_unix_ms);
    return hmac_sha256_hex(app.impl_->udp_bind_secret_, signing_input);
}

std::vector<std::uint8_t> GatewayAppAccess::make_udp_bind_res_frame(const GatewayApp& app,
                                                                    std::uint16_t code,
                                                                    const UdpBindTicket& ticket,
                                                                    std::string_view message,
                                                                    std::uint32_t seq) {
    return make_udp_bind_res_frame(
        app,
        code,
        ticket.session_id,
        ticket.nonce,
        ticket.expires_unix_ms,
        ticket.token,
        message,
        seq
    );
}

std::vector<std::uint8_t> GatewayAppAccess::make_udp_bind_res_frame(const GatewayApp&,
                                                                    std::uint16_t code,
                                                                    std::string_view session_id,
                                                                    std::uint64_t nonce,
                                                                    std::uint64_t expires_unix_ms,
                                                                    std::string_view token,
                                                                    std::string_view message,
                                                                    std::uint32_t seq) {
    namespace proto = server::core::protocol;
    const auto payload = server::core::realtime::encode_direct_bind_response_payload(
        code,
        server::core::realtime::DirectBindTicket{
            .session_id = std::string(session_id),
            .nonce = nonce,
            .expires_unix_ms = expires_unix_ms,
            .token = std::string(token),
        },
        message);

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

std::optional<std::vector<std::uint8_t>> GatewayAppAccess::make_udp_bind_ticket_frame(GatewayApp& app,
                                                                                       const std::string& session_id) {
    if (app.impl_->udp_listen_port_ == 0) {
        return std::nullopt;
    }

    if (app.impl_->udp_bind_secret_.empty()) {
        return std::nullopt;
    }

    UdpBindTicket ticket{};
    bool rudp_selected = false;
    {
        std::lock_guard<std::mutex> lock(app.impl_->session_mutex_);
        auto it = app.impl_->sessions_.find(session_id);
        if (it == app.impl_->sessions_.end()) {
            return std::nullopt;
        }
        auto& state = *it->second;

        std::random_device rd;
        const std::uint64_t nonce = (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd());
        const std::uint64_t issued_unix_ms = unix_time_ms();
        const std::uint64_t expires_unix_ms = issued_unix_ms + static_cast<std::uint64_t>(app.impl_->udp_bind_ttl_ms_);
        const std::string token = make_udp_bind_token(app, session_id, nonce, expires_unix_ms);
        if (token.empty()) {
            return std::nullopt;
        }

        state.udp_nonce = nonce;
        state.udp_expires_unix_ms = expires_unix_ms;
        state.udp_ticket_issued_unix_ms = issued_unix_ms;
        state.udp_bind_fail_attempts = 0;
        state.udp_bind_retry_after_unix_ms = 0;
        state.udp_token = token;
        state.udp_bound = false;
        state.udp_endpoint = {};
        state.udp_sequenced_metrics.reset();
        state.rudp_fallback_to_tcp = false;
        const auto attach_decision = server::core::realtime::evaluate_direct_attach(
            app.impl_->rudp_rollout_policy_,
            session_id,
            nonce);
        state.rudp_selected =
            attach_decision.mode == server::core::realtime::DirectAttachMode::kRudpCanary;
        if (state.rudp_selected) {
            state.rudp_engine = std::make_unique<server::core::net::rudp::RudpEngine>(*app.impl_->rudp_config_);
        } else {
            state.rudp_engine.reset();
        }

        ticket.session_id = session_id;
        ticket.nonce = nonce;
        ticket.expires_unix_ms = expires_unix_ms;
        ticket.token = token;
        rudp_selected = state.rudp_selected;
    }

    (void)app.impl_->udp_bind_ticket_issued_total_.fetch_add(1, std::memory_order_relaxed);
    server::core::log::info(
        "GatewayApp UDP bind ticket issued: session=" + ticket.session_id
        + " nonce=" + std::to_string(ticket.nonce)
        + " expires_unix_ms=" + std::to_string(ticket.expires_unix_ms)
        + " rudp_selected=" + std::string(rudp_selected ? "1" : "0"));
    return make_udp_bind_res_frame(app, 0, ticket, "issued");
}

bool GatewayAppAccess::parse_udp_bind_req(const GatewayApp&,
                                          std::span<const std::uint8_t> payload,
                                          ParsedUdpBindRequest& out) {
    return server::core::realtime::decode_direct_bind_request_payload(payload, out);
}

std::uint16_t GatewayAppAccess::apply_udp_bind_request(GatewayApp& app,
                                                       const ParsedUdpBindRequest& req,
                                                       const boost::asio::ip::udp::endpoint& endpoint,
                                                       UdpBindTicket& applied_ticket,
                                                       std::string& message) {
    using server::core::protocol::errc::FORBIDDEN;
    using server::core::protocol::errc::INVALID_PAYLOAD;
    using server::core::protocol::errc::SERVER_BUSY;
    using server::core::protocol::errc::UNAUTHORIZED;

    if (req.session_id.empty() || req.token.empty()) {
        message = "invalid bind payload";
        return INVALID_PAYLOAD;
    }

    const auto now_ms = unix_time_ms();

    std::lock_guard<std::mutex> lock(app.impl_->session_mutex_);
    auto it = app.impl_->sessions_.find(req.session_id);
    if (it == app.impl_->sessions_.end()) {
        message = "unknown session";
        return UNAUTHORIZED;
    }

    auto& state = *it->second;

    const auto apply_retry_backoff = [&]() {
        const auto next_attempt = std::min<std::uint32_t>(state.udp_bind_fail_attempts + 1u,
                                                          app.impl_->udp_bind_retry_max_attempts_);
        state.udp_bind_fail_attempts = next_attempt;
        const auto delay_ms = udp_bind_retry_delay_ms(app, next_attempt);
        state.udp_bind_retry_after_unix_ms = now_ms + static_cast<std::uint64_t>(delay_ms);
        (void)app.impl_->udp_bind_retry_backoff_total_.fetch_add(1, std::memory_order_relaxed);
    };

    if (state.udp_bind_retry_after_unix_ms > now_ms) {
        message = "bind retry backoff";
        (void)app.impl_->udp_bind_retry_reject_total_.fetch_add(1, std::memory_order_relaxed);
        return SERVER_BUSY;
    }

    if (state.udp_expires_unix_ms == 0 || state.udp_nonce == 0 || state.udp_token.empty()) {
        apply_retry_backoff();
        message = "bind ticket not issued";
        return UNAUTHORIZED;
    }

    if (req.expires_unix_ms < now_ms) {
        apply_retry_backoff();
        message = "ticket expired";
        return UNAUTHORIZED;
    }

    if (state.udp_expires_unix_ms < now_ms) {
        apply_retry_backoff();
        message = "ticket expired";
        return UNAUTHORIZED;
    }

    if (req.expires_unix_ms != state.udp_expires_unix_ms || req.nonce != state.udp_nonce) {
        apply_retry_backoff();
        message = "ticket mismatch";
        return UNAUTHORIZED;
    }

    const std::string expected = make_udp_bind_token(app, req.session_id, req.nonce, req.expires_unix_ms);
    if (!secure_equals(req.token, state.udp_token) || !secure_equals(req.token, expected)) {
        apply_retry_backoff();
        message = "invalid token";
        return UNAUTHORIZED;
    }

    if (state.udp_bound && state.udp_endpoint != endpoint) {
        apply_retry_backoff();
        message = "session already bound";
        return FORBIDDEN;
    }

    state.udp_bound = true;
    state.udp_endpoint = endpoint;
    state.udp_sequenced_metrics.reset();
    state.udp_bind_fail_attempts = 0;
    state.udp_bind_retry_after_unix_ms = 0;
    state.rudp_fallback_to_tcp = false;
    if (state.rudp_selected && !state.rudp_engine) {
        state.rudp_engine = std::make_unique<server::core::net::rudp::RudpEngine>(*app.impl_->rudp_config_);
    }

    if (state.udp_ticket_issued_unix_ms != 0 && now_ms >= state.udp_ticket_issued_unix_ms) {
        const auto bind_rtt_ms = now_ms - state.udp_ticket_issued_unix_ms;
        app.impl_->udp_rtt_ms_last_.store(bind_rtt_ms, std::memory_order_relaxed);
    }

    applied_ticket.session_id = req.session_id;
    applied_ticket.nonce = req.nonce;
    applied_ticket.expires_unix_ms = req.expires_unix_ms;
    applied_ticket.token = req.token;

    message = "bound";
    return 0;
}

void GatewayAppAccess::send_udp_datagram(GatewayApp& app,
                                         std::vector<std::uint8_t> frame,
                                         const boost::asio::ip::udp::endpoint& endpoint) {
    if (!app.impl_->udp_socket_) {
        return;
    }

    auto buffer = std::make_shared<std::vector<std::uint8_t>>(std::move(frame));
    trace_udp_bind_send(app, std::span<const std::uint8_t>(buffer->data(), buffer->size()), endpoint);
    auto handler = [&app, buffer, endpoint](const boost::system::error_code& ec, std::size_t) {
        if (ec) {
            (void)app.impl_->udp_send_error_total_.fetch_add(1, std::memory_order_relaxed);
            server::core::log::warn(
                "GatewayApp UDP send failed: endpoint="
                + endpoint.address().to_string() + ":" + std::to_string(endpoint.port())
                + " error=" + ec.message());
        }
    };

    app.impl_->udp_socket_->async_send_to(
        boost::asio::buffer(*buffer),
        endpoint,
        std::move(handler));
}

bool GatewayAppAccess::try_send_direct_client_frame(GatewayApp& app,
                                                    std::string_view session_id,
                                                    std::uint16_t msg_id,
                                                    std::span<const std::uint8_t> frame) {
    if (!is_direct_egress_msg(msg_id) || frame.empty()) {
        return false;
    }

    boost::asio::ip::udp::endpoint endpoint;
    DirectEgressDecision decision{};

    {
        std::lock_guard<std::mutex> lock(app.impl_->session_mutex_);
        const auto it = app.impl_->sessions_.find(std::string(session_id));
        if (it == app.impl_->sessions_.end()) {
            (void)app.impl_->direct_state_delta_tcp_fallback_total_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        auto& state = *it->second;

        const bool rudp_established =
            state.rudp_engine != nullptr
            && state.rudp_engine->state().lifecycle == server::core::net::rudp::LifecycleState::kEstablished;
        decision = evaluate_direct_egress(DirectEgressContext{
            .msg_id = msg_id,
            .udp_bound = state.udp_bound,
            .rudp_selected = state.rudp_selected,
            .rudp_fallback_to_tcp = state.rudp_fallback_to_tcp,
            .rudp_established = rudp_established,
        });
        endpoint = state.udp_endpoint;

        if (decision.route == DirectEgressRoute::kRudp) {
            std::vector<std::uint8_t> datagram;
            const auto channel = server::protocol::opcode_policy(msg_id).channel;
            if (!state.rudp_engine->queue_unreliable_payload(frame, channel, unix_time_ms(), datagram)) {
                state.rudp_fallback_to_tcp = true;
                (void)app.impl_->rudp_fallback_total_.fetch_add(1, std::memory_order_relaxed);
                (void)app.impl_->direct_state_delta_tcp_fallback_total_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            send_udp_datagram(app, std::move(datagram), endpoint);
            (void)app.impl_->rudp_direct_state_delta_total_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }

    if (decision.route == DirectEgressRoute::kUdp) {
        send_udp_datagram(app, std::vector<std::uint8_t>(frame.begin(), frame.end()), endpoint);
        (void)app.impl_->udp_direct_state_delta_total_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    (void)app.impl_->direct_state_delta_tcp_fallback_total_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void GatewayAppAccess::trace_udp_bind_send(const GatewayApp&,
                                           std::span<const std::uint8_t> frame,
                                           const boost::asio::ip::udp::endpoint& endpoint) {
    namespace proto = server::core::protocol;

    if (frame.size() < proto::k_header_bytes) {
        return;
    }

    proto::PacketHeader header{};
    proto::decode_header(frame.data(), header);
    if (header.msg_id != server::protocol::MSG_UDP_BIND_RES) {
        return;
    }

    std::string session_id;
    std::uint16_t code = 0;
    std::uint64_t nonce = 0;
    std::uint64_t expires_unix_ms = 0;
    std::string message;
    const auto payload = frame.subspan(proto::k_header_bytes);
    server::core::realtime::DirectBindResponse bind_response{};
    if (server::core::realtime::decode_direct_bind_response_payload(payload, bind_response)) {
        code = bind_response.code;
        session_id = bind_response.ticket.session_id;
        nonce = bind_response.ticket.nonce;
        expires_unix_ms = bind_response.ticket.expires_unix_ms;
        message = bind_response.message;
    }

    server::core::log::info(
        "GatewayApp UDP bind response send: endpoint=" + endpoint_key(endpoint)
        + " session=" + session_id
        + " nonce=" + std::to_string(nonce)
        + " expires_unix_ms=" + std::to_string(expires_unix_ms)
        + " code=" + std::to_string(code)
        + " message=" + message
        + " " + udp_frame_summary(frame));
}

void GatewayAppAccess::start_udp_listener(GatewayApp& app) {
    auto* impl_ = app.impl_.get();
    if (impl_->udp_listen_port_ == 0) {
        impl_->udp_enabled_.store(false, std::memory_order_relaxed);
        return;
    }

    if (impl_->udp_bind_secret_.empty()) {
        server::core::log::warn("GatewayApp UDP bind secret is empty; UDP disabled");
        impl_->udp_enabled_.store(false, std::memory_order_relaxed);
        return;
    }

    boost::system::error_code ec;
    auto address = boost::asio::ip::make_address(impl_->udp_listen_host_.empty() ? "0.0.0.0" : impl_->udp_listen_host_, ec);
    if (ec) {
        server::core::log::warn("GatewayApp failed to parse UDP listen address; UDP disabled");
        impl_->udp_enabled_.store(false, std::memory_order_relaxed);
        return;
    }

    auto socket = std::make_unique<boost::asio::ip::udp::socket>(impl_->io_);
    socket->open(boost::asio::ip::udp::v4(), ec);
    if (ec) {
        server::core::log::warn("GatewayApp failed to open UDP socket; UDP disabled");
        impl_->udp_enabled_.store(false, std::memory_order_relaxed);
        return;
    }

    socket->set_option(boost::asio::socket_base::reuse_address(true), ec);
    if (ec) {
        server::core::log::warn("GatewayApp failed to set UDP reuse_address; UDP disabled");
        impl_->udp_enabled_.store(false, std::memory_order_relaxed);
        return;
    }

    socket->bind(boost::asio::ip::udp::endpoint{address, impl_->udp_listen_port_}, ec);
    if (ec) {
        server::core::log::warn("GatewayApp failed to bind UDP socket; UDP disabled");
        impl_->udp_enabled_.store(false, std::memory_order_relaxed);
        return;
    }

    impl_->udp_socket_ = std::move(socket);
    impl_->udp_enabled_.store(true, std::memory_order_relaxed);
    GatewayAppAccess::do_udp_receive(app);
    server::core::log::info("GatewayApp UDP listening on " + address.to_string() + ":" + std::to_string(impl_->udp_listen_port_));
}

void GatewayAppAccess::stop_udp_listener(GatewayApp& app) {
    auto* impl_ = app.impl_.get();
    impl_->udp_enabled_.store(false, std::memory_order_relaxed);
    if (!impl_->udp_socket_) {
        return;
    }

    boost::system::error_code ec;
    impl_->udp_socket_->cancel(ec);
    impl_->udp_socket_->close(ec);
    impl_->udp_socket_.reset();
}

void GatewayAppAccess::do_udp_receive(GatewayApp& app) {
    auto* impl_ = app.impl_.get();
    if (!impl_->udp_socket_) {
        return;
    }

    impl_->udp_socket_->async_receive_from(
        boost::asio::buffer(impl_->udp_read_buffer_),
        impl_->udp_remote_endpoint_,
        [&app, impl_](const boost::system::error_code& ec, std::size_t bytes) {
            if (ec) {
                if (ec != boost::asio::error::operation_aborted) {
                    (void)impl_->udp_receive_error_total_.fetch_add(1, std::memory_order_relaxed);
                    GatewayAppAccess::do_udp_receive(app);
                }
                return;
            }

            if (bytes > 0) {
                (void)impl_->udp_packets_total_.fetch_add(1, std::memory_order_relaxed);
            }

            namespace proto = server::core::protocol;
            const auto now_ms = unix_time_ms();
            const auto incoming_datagram = std::span<const std::uint8_t>(impl_->udp_read_buffer_.data(), bytes);

            if (server::core::net::rudp::looks_like_rudp(incoming_datagram)) {
                (void)impl_->rudp_packets_total_.fetch_add(1, std::memory_order_relaxed);

                TransportSessionPtr bound_session;
                std::string bound_session_id;
                {
                    std::lock_guard<std::mutex> lock(impl_->session_mutex_);
                    for (const auto& [sid, state] : impl_->sessions_) {
                        if (state && state->udp_bound && state->udp_endpoint == impl_->udp_remote_endpoint_) {
                            bound_session = state->session;
                            bound_session_id = sid;
                            break;
                        }
                    }
                }

                if (!bound_session) {
                    (void)impl_->udp_bind_reject_total_.fetch_add(1, std::memory_order_relaxed);
                    (void)impl_->rudp_packets_reject_total_.fetch_add(1, std::memory_order_relaxed);
                    GatewayAppAccess::do_udp_receive(app);
                    return;
                }

                std::vector<std::vector<std::uint8_t>> egress_datagrams;
                std::vector<std::vector<std::uint8_t>> inner_frames;
                std::uint64_t retransmit_count = 0;
                bool fallback_required = false;
                bool invalid_inner = false;

                {
                    std::lock_guard<std::mutex> lock(impl_->session_mutex_);
                    auto it = impl_->sessions_.find(bound_session_id);
                    if (it == impl_->sessions_.end()
                        || !it->second
                        || !it->second->udp_bound
                        || it->second->udp_endpoint != impl_->udp_remote_endpoint_) {
                        fallback_required = true;
                    } else {
                        auto& state = *it->second;
                        if (!impl_->rudp_rollout_policy_.enabled
                            || !state.rudp_selected
                            || state.rudp_fallback_to_tcp
                            || !state.rudp_engine) {
                            state.rudp_fallback_to_tcp = true;
                            fallback_required = true;
                            server::core::runtime_metrics::record_rudp_fallback(
                                server::core::runtime_metrics::RudpFallbackReason::kDisabled);
                        } else {
                            auto process_result = state.rudp_engine->process_datagram(incoming_datagram, now_ms);
                            auto poll_result = state.rudp_engine->poll(now_ms);

                            if (!process_result.egress_datagrams.empty()) {
                                egress_datagrams = std::move(process_result.egress_datagrams);
                            }
                            if (!poll_result.egress_datagrams.empty()) {
                                egress_datagrams.reserve(egress_datagrams.size() + poll_result.egress_datagrams.size());
                                for (auto& frame : poll_result.egress_datagrams) {
                                    egress_datagrams.push_back(std::move(frame));
                                }
                            }
                            inner_frames = std::move(process_result.inner_frames);
                            retransmit_count = poll_result.retransmit_count;

                            if (process_result.fallback_required || poll_result.fallback_required) {
                                state.rudp_fallback_to_tcp = true;
                                fallback_required = true;
                            }
                        }
                    }
                }

                for (auto& frame : egress_datagrams) {
                    GatewayAppAccess::send_udp_datagram(app, std::move(frame), impl_->udp_remote_endpoint_);
                }

                if (retransmit_count > 0) {
                    (void)impl_->udp_retransmit_total_.fetch_add(retransmit_count, std::memory_order_relaxed);
                }

                if (!fallback_required) {
                    for (const auto& inner_frame : inner_frames) {
                        if (inner_frame.size() < proto::k_header_bytes) {
                            invalid_inner = true;
                            break;
                        }

                        proto::PacketHeader inner_header{};
                        proto::decode_header(inner_frame.data(), inner_header);
                        const auto inner_body_len = static_cast<std::size_t>(inner_header.length);
                        if (inner_body_len != (inner_frame.size() - proto::k_header_bytes)) {
                            invalid_inner = true;
                            break;
                        }

                        const bool is_game_opcode = !server::protocol::opcode_name(inner_header.msg_id).empty();
                        const bool is_core_opcode = !server::core::protocol::opcode_name(inner_header.msg_id).empty();
                        if (!is_game_opcode && !is_core_opcode) {
                            invalid_inner = true;
                            break;
                        }

                        if (!impl_->rudp_rollout_policy_.opcode_allowed(inner_header.msg_id)) {
                            invalid_inner = true;
                            break;
                        }

                        const auto policy = is_game_opcode
                            ? server::protocol::opcode_policy(inner_header.msg_id)
                            : server::core::protocol::opcode_policy(inner_header.msg_id);
                        if (!server::core::protocol::transport_allows(
                                policy.transport,
                                server::core::protocol::TransportKind::kUdp)) {
                            invalid_inner = true;
                            break;
                        }

                        bound_session->send(inner_frame.data(), inner_frame.size());
                        (void)impl_->udp_forward_total_.fetch_add(1, std::memory_order_relaxed);
                        (void)impl_->rudp_inner_forward_total_.fetch_add(1, std::memory_order_relaxed);
                        switch (policy.delivery) {
                            case server::core::protocol::DeliveryClass::kReliableOrdered:
                                (void)impl_->udp_forward_reliable_ordered_total_.fetch_add(1, std::memory_order_relaxed);
                                break;
                            case server::core::protocol::DeliveryClass::kReliable:
                                (void)impl_->udp_forward_reliable_total_.fetch_add(1, std::memory_order_relaxed);
                                break;
                            case server::core::protocol::DeliveryClass::kUnreliableSequenced:
                                (void)impl_->udp_forward_unreliable_sequenced_total_.fetch_add(1, std::memory_order_relaxed);
                                break;
                        }
                    }
                }

                if (invalid_inner) {
                    fallback_required = true;
                    server::core::runtime_metrics::record_rudp_fallback(
                        server::core::runtime_metrics::RudpFallbackReason::kProtocolError);
                    std::lock_guard<std::mutex> lock(impl_->session_mutex_);
                    auto it = impl_->sessions_.find(bound_session_id);
                    if (it != impl_->sessions_.end() && it->second) {
                        it->second->rudp_fallback_to_tcp = true;
                    }
                }

                if (fallback_required) {
                    (void)impl_->rudp_packets_reject_total_.fetch_add(1, std::memory_order_relaxed);
                    (void)impl_->rudp_fallback_total_.fetch_add(1, std::memory_order_relaxed);
                }

                GatewayAppAccess::do_udp_receive(app);
                return;
            }

            if (bytes < proto::k_header_bytes) {
                (void)impl_->udp_receive_error_total_.fetch_add(1, std::memory_order_relaxed);
                GatewayAppAccess::do_udp_receive(app);
                return;
            }

            proto::PacketHeader header{};
            proto::decode_header(impl_->udp_read_buffer_.data(), header);
            const auto body_len = static_cast<std::size_t>(header.length);
            if (body_len != (bytes - proto::k_header_bytes)) {
                (void)impl_->udp_receive_error_total_.fetch_add(1, std::memory_order_relaxed);
                GatewayAppAccess::do_udp_receive(app);
                return;
            }

            const auto payload = std::span<const std::uint8_t>(
                impl_->udp_read_buffer_.data() + proto::k_header_bytes,
                body_len
            );

            if (header.msg_id == server::protocol::MSG_UDP_BIND_REQ) {
                const auto remote_key = endpoint_key(impl_->udp_remote_endpoint_);
                const auto block_state = impl_->udp_bind_abuse_guard_.block_state(remote_key, now_ms);
                server::core::log::info(
                    "GatewayApp UDP bind request recv: endpoint=" + remote_key
                    + " blocked=" + std::string(block_state.blocked ? "1" : "0")
                    + " retry_after_ms=" + std::to_string(block_state.retry_after_ms)
                    + " " + udp_frame_summary(incoming_datagram));
                if (block_state.blocked) {
                    (void)impl_->udp_bind_rate_limit_reject_total_.fetch_add(1, std::memory_order_relaxed);
                    (void)impl_->udp_bind_reject_total_.fetch_add(1, std::memory_order_relaxed);
                    auto frame = GatewayAppAccess::make_udp_bind_res_frame(
                        app,
                        server::core::protocol::errc::SERVER_BUSY,
                        std::string_view{},
                        0,
                        0,
                        std::string_view{},
                        "bind temporarily blocked",
                        header.seq
                    );
                    GatewayAppAccess::send_udp_datagram(app, std::move(frame), impl_->udp_remote_endpoint_);
                    GatewayAppAccess::do_udp_receive(app);
                    return;
                }

                auto record_bind_failure = [&]() {
                    if (impl_->udp_bind_abuse_guard_.record_failure(remote_key, now_ms)) {
                        (void)impl_->udp_bind_block_total_.fetch_add(1, std::memory_order_relaxed);
                    }
                };

                ParsedUdpBindRequest req{};
                if (!GatewayAppAccess::parse_udp_bind_req(app, payload, req)) {
                    (void)impl_->udp_bind_reject_total_.fetch_add(1, std::memory_order_relaxed);
                    record_bind_failure();
                    server::core::log::warn(
                        "GatewayApp UDP bind request parse failed: endpoint=" + remote_key
                        + " " + udp_frame_summary(incoming_datagram));
                    auto frame = GatewayAppAccess::make_udp_bind_res_frame(
                        app,
                        server::core::protocol::errc::INVALID_PAYLOAD,
                        std::string_view{},
                        0,
                        0,
                        std::string_view{},
                        "invalid bind payload",
                        header.seq
                    );
                    GatewayAppAccess::send_udp_datagram(app, std::move(frame), impl_->udp_remote_endpoint_);
                    GatewayAppAccess::do_udp_receive(app);
                    return;
                }

                server::core::log::info(
                    "GatewayApp UDP bind request parsed: session=" + req.session_id
                    + " endpoint=" + remote_key
                    + " nonce=" + std::to_string(req.nonce)
                    + " expires_unix_ms=" + std::to_string(req.expires_unix_ms)
                    + " token_bytes=" + std::to_string(req.token.size())
                    + " seq=" + std::to_string(header.seq));

                UdpBindTicket ticket{};
                std::string message;
                const auto code =
                    GatewayAppAccess::apply_udp_bind_request(app, req, impl_->udp_remote_endpoint_, ticket, message);
                if (code == 0) {
                    (void)impl_->udp_bind_success_total_.fetch_add(1, std::memory_order_relaxed);
                    impl_->udp_bind_abuse_guard_.record_success(remote_key);
                    server::core::log::info(
                        "GatewayApp UDP bind success: session=" + ticket.session_id
                        + " endpoint=" + remote_key
                        + " nonce=" + std::to_string(ticket.nonce)
                        + " expires_unix_ms=" + std::to_string(ticket.expires_unix_ms)
                        + " seq=" + std::to_string(header.seq));
                } else {
                    (void)impl_->udp_bind_reject_total_.fetch_add(1, std::memory_order_relaxed);
                    record_bind_failure();
                    server::core::log::warn(
                        "GatewayApp UDP bind reject: session=" + req.session_id
                        + " endpoint=" + remote_key
                        + " nonce=" + std::to_string(req.nonce)
                        + " expires_unix_ms=" + std::to_string(req.expires_unix_ms)
                        + " seq=" + std::to_string(header.seq)
                        + " code=" + std::to_string(code)
                        + " message=" + message);
                }

                auto frame = (code == 0)
                    ? GatewayAppAccess::make_udp_bind_res_frame(app, code, ticket, message, header.seq)
                    : GatewayAppAccess::make_udp_bind_res_frame(app,
                                                                code,
                                                                req.session_id,
                                                                req.nonce,
                                                                req.expires_unix_ms,
                                                                req.token,
                                                                message,
                                                                header.seq);
                GatewayAppAccess::send_udp_datagram(app, std::move(frame), impl_->udp_remote_endpoint_);
                GatewayAppAccess::do_udp_receive(app);
                return;
            }

            TransportSessionPtr bound_session;
            std::string bound_session_id;
            {
                std::lock_guard<std::mutex> lock(impl_->session_mutex_);
                for (auto& [sid, state] : impl_->sessions_) {
                    if (state && state->udp_bound && state->udp_endpoint == impl_->udp_remote_endpoint_) {
                        bound_session = state->session;
                        bound_session_id = sid;
                        break;
                    }
                }
            }

            if (!bound_session) {
                (void)impl_->udp_bind_reject_total_.fetch_add(1, std::memory_order_relaxed);
                auto frame = GatewayAppAccess::make_udp_bind_res_frame(
                    app,
                    server::core::protocol::errc::UNAUTHORIZED,
                    std::string_view{},
                    0,
                    0,
                    std::string_view{},
                    "udp session not bound",
                    header.seq
                );
                GatewayAppAccess::send_udp_datagram(app, std::move(frame), impl_->udp_remote_endpoint_);
                GatewayAppAccess::do_udp_receive(app);
                return;
            }

            const bool is_game_opcode = !server::protocol::opcode_name(header.msg_id).empty();
            const bool is_core_opcode = !server::core::protocol::opcode_name(header.msg_id).empty();
            if (!is_game_opcode && !is_core_opcode) {
                (void)impl_->udp_bind_reject_total_.fetch_add(1, std::memory_order_relaxed);
                auto frame = GatewayAppAccess::make_udp_bind_res_frame(
                    app,
                    server::core::protocol::errc::UNKNOWN_MSG_ID,
                    std::string_view{},
                    0,
                    0,
                    std::string_view{},
                    "unknown udp msg_id",
                    header.seq
                );
                GatewayAppAccess::send_udp_datagram(app, std::move(frame), impl_->udp_remote_endpoint_);
                GatewayAppAccess::do_udp_receive(app);
                return;
            }

            if (!impl_->udp_opcode_allowlist_.contains(header.msg_id)) {
                (void)impl_->udp_bind_reject_total_.fetch_add(1, std::memory_order_relaxed);
                (void)impl_->udp_opcode_allowlist_reject_total_.fetch_add(1, std::memory_order_relaxed);
                auto frame = GatewayAppAccess::make_udp_bind_res_frame(
                    app,
                    server::core::protocol::errc::FORBIDDEN,
                    std::string_view{},
                    0,
                    0,
                    std::string_view{},
                    "opcode not in udp allowlist",
                    header.seq
                );
                GatewayAppAccess::send_udp_datagram(app, std::move(frame), impl_->udp_remote_endpoint_);
                GatewayAppAccess::do_udp_receive(app);
                return;
            }

            const auto policy = is_game_opcode
                ? server::protocol::opcode_policy(header.msg_id)
                : server::core::protocol::opcode_policy(header.msg_id);
            if (!server::core::protocol::transport_allows(policy.transport, server::core::protocol::TransportKind::kUdp)) {
                (void)impl_->udp_bind_reject_total_.fetch_add(1, std::memory_order_relaxed);
                auto frame = GatewayAppAccess::make_udp_bind_res_frame(
                    app,
                    server::core::protocol::errc::FORBIDDEN,
                    std::string_view{},
                    0,
                    0,
                    std::string_view{},
                    "opcode not allowed on udp",
                    header.seq
                );
                GatewayAppAccess::send_udp_datagram(app, std::move(frame), impl_->udp_remote_endpoint_);
                GatewayAppAccess::do_udp_receive(app);
                return;
            }

            if (policy.delivery == server::core::protocol::DeliveryClass::kUnreliableSequenced) {
                gateway::UdpSequencedMetrics::UpdateResult update{};
                {
                    std::lock_guard<std::mutex> lock(impl_->session_mutex_);
                    auto it = impl_->sessions_.find(bound_session_id);
                    if (it != impl_->sessions_.end()
                        && it->second
                        && it->second->udp_bound
                        && it->second->udp_endpoint == impl_->udp_remote_endpoint_) {
                        update = it->second->udp_sequenced_metrics.on_packet(header.seq, now_ms);
                    }
                }

                if (!update.accepted) {
                    (void)impl_->udp_replay_drop_total_.fetch_add(1, std::memory_order_relaxed);
                    (void)impl_->udp_bind_reject_total_.fetch_add(1, std::memory_order_relaxed);

                    if (update.duplicate) {
                        (void)impl_->udp_duplicate_drop_total_.fetch_add(1, std::memory_order_relaxed);
                        (void)impl_->udp_retransmit_total_.fetch_add(1, std::memory_order_relaxed);
                    } else if (update.reordered) {
                        (void)impl_->udp_reorder_drop_total_.fetch_add(1, std::memory_order_relaxed);
                    }

                    auto frame = GatewayAppAccess::make_udp_bind_res_frame(
                        app,
                        server::core::protocol::errc::FORBIDDEN,
                        std::string_view{},
                        0,
                        0,
                        std::string_view{},
                        "stale sequenced udp packet",
                        header.seq
                    );
                    GatewayAppAccess::send_udp_datagram(app, std::move(frame), impl_->udp_remote_endpoint_);
                    GatewayAppAccess::do_udp_receive(app);
                    return;
                }

                if (update.estimated_lost_packets > 0) {
                    (void)impl_->udp_loss_estimated_total_.fetch_add(update.estimated_lost_packets, std::memory_order_relaxed);
                }
                if (update.jitter_ms > 0) {
                    impl_->udp_jitter_ms_last_.store(update.jitter_ms, std::memory_order_relaxed);
                }
            }

            bound_session->send(impl_->udp_read_buffer_.data(), bytes);
            (void)impl_->udp_forward_total_.fetch_add(1, std::memory_order_relaxed);
            switch (policy.delivery) {
                case server::core::protocol::DeliveryClass::kReliableOrdered:
                    (void)impl_->udp_forward_reliable_ordered_total_.fetch_add(1, std::memory_order_relaxed);
                    break;
                case server::core::protocol::DeliveryClass::kReliable:
                    (void)impl_->udp_forward_reliable_total_.fetch_add(1, std::memory_order_relaxed);
                    break;
                case server::core::protocol::DeliveryClass::kUnreliableSequenced:
                    (void)impl_->udp_forward_unreliable_sequenced_total_.fetch_add(1, std::memory_order_relaxed);
                    break;
            }
            GatewayAppAccess::do_udp_receive(app);
        });
}

} // namespace gateway
