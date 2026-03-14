#include "session_client.hpp"

#include "server/core/protocol/packet.hpp"
#include "server/core/protocol/protocol_flags.hpp"
#include "server/core/protocol/system_opcodes.hpp"
#include "server/core/protocol/version.hpp"
#include "server/protocol/game_opcodes.hpp"
#include "server/wire/codec.hpp"
#include "wire.pb.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using udp = asio::ip::udp;
namespace proto = server::core::protocol;
namespace game_proto = server::protocol;
namespace rudp = server::core::net::rudp;

namespace loadgen {

namespace {

struct AsyncConnectResult {
    std::promise<boost::system::error_code> promise;
    std::atomic<bool> fulfilled{false};
};

std::atomic<std::uint32_t> g_next_rudp_connection_id{1};

void complete_async_result(const std::shared_ptr<AsyncConnectResult>& result,
                           const boost::system::error_code& ec) {
    bool expected = false;
    if (result->fulfilled.compare_exchange_strong(expected, true)) {
        result->promise.set_value(ec);
    }
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

std::uint32_t unix_time_ms32() {
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    return static_cast<std::uint32_t>(now_ms & 0xFFFFFFFFu);
}

std::uint64_t unix_time_ms64() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string bind_ticket_error_message(const UdpBindTicket& ticket) {
    std::string message = ticket.message.empty() ? "udp bind rejected" : ticket.message;
    return "udp bind rejected (code=" + std::to_string(ticket.code) + "): " + message;
}

std::string opcode_label(std::uint16_t msg_id) {
    if (const auto game_name = game_proto::opcode_name(msg_id); !game_name.empty()) {
        return std::string(game_name);
    }
    if (const auto core_name = proto::opcode_name(msg_id); !core_name.empty()) {
        return std::string(core_name);
    }
    return "unknown";
}

std::string bytes_prefix_hex(std::span<const std::uint8_t> bytes, std::size_t max_bytes = 16) {
    static constexpr char kHex[] = "0123456789abcdef";
    const auto count = std::min(bytes.size(), max_bytes);
    std::string out;
    out.reserve(count * 2 + (bytes.size() > count ? 3 : 0));
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(kHex[(bytes[i] >> 4) & 0x0Fu]);
        out.push_back(kHex[bytes[i] & 0x0Fu]);
    }
    if (bytes.size() > count) {
        out += "...";
    }
    return out;
}

std::string frame_summary(std::span<const std::uint8_t> datagram) {
    std::ostringstream oss;
    oss << "bytes=" << datagram.size();
    if (datagram.size() < proto::k_header_bytes) {
        oss << " short prefix=" << bytes_prefix_hex(datagram);
        return oss.str();
    }

    proto::PacketHeader header{};
    proto::decode_header(datagram.data(), header);
    oss << " msg_id=" << header.msg_id
        << "(" << opcode_label(header.msg_id) << ")"
        << " seq=" << header.seq
        << " payload_len=" << header.length
        << " prefix=" << bytes_prefix_hex(datagram);
    return oss.str();
}

void trace_udp_attach(const ClientOptions& options,
                      std::string_view transport,
                      std::string_view stage,
                      const std::string& message) {
    if (!options.trace_udp_attach) {
        return;
    }
    std::cerr << "[loadgen][" << std::string(transport) << "][udp-trace] "
              << std::string(stage) << ' ' << message << '\n';
}

std::string endpoint_to_string(const udp::endpoint& endpoint) {
    if (endpoint.address().is_unspecified() && endpoint.port() == 0) {
        return "unbound";
    }
    return endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
}

std::string socket_endpoint_summary(udp::socket& socket, const udp::endpoint& remote_endpoint) {
    boost::system::error_code ec;
    const auto local_endpoint = socket.local_endpoint(ec);
    const std::string local = ec ? std::string("unknown") : endpoint_to_string(local_endpoint);
    return "local=" + local + " remote=" + endpoint_to_string(remote_endpoint);
}

bool parse_udp_bind_ticket_payload(std::span<const std::uint8_t> payload, UdpBindTicket& out) {
    std::span<const std::uint8_t> in = payload;
    if (in.size() < 2) {
        return false;
    }

    out = {};
    out.code = proto::read_be16(in.data());
    in = in.subspan(2);

    if (!proto::read_lp_utf8(in, out.session_id)) {
        return false;
    }
    if (!read_be64(in, out.nonce)) {
        return false;
    }
    if (!read_be64(in, out.expires_unix_ms)) {
        return false;
    }
    if (!proto::read_lp_utf8(in, out.token)) {
        return false;
    }
    if (!proto::read_lp_utf8(in, out.message)) {
        return false;
    }
    return in.empty();
}

std::vector<std::uint8_t> make_frame(std::uint16_t msg_id,
                                     std::uint32_t seq,
                                     std::span<const std::uint8_t> payload) {
    proto::PacketHeader header{};
    header.length = static_cast<std::uint16_t>(payload.size());
    header.msg_id = msg_id;
    header.flags = 0;
    header.seq = seq;
    header.utc_ts_ms32 = unix_time_ms32();

    std::vector<std::uint8_t> frame(proto::k_header_bytes + payload.size());
    proto::encode_header(header, frame.data());
    if (!payload.empty()) {
        std::memcpy(frame.data() + proto::k_header_bytes, payload.data(), payload.size());
    }
    return frame;
}

std::string unsupported_operation_message(std::string_view transport, std::string_view operation) {
    return "transport '" + std::string(transport)
        + "' currently only supports login_only attach validation; "
        + std::string(operation)
        + " requires tcp";
}

bool open_udp_socket(udp::resolver& resolver,
                     udp::socket& socket,
                     udp::endpoint& remote_endpoint,
                     const std::string& host,
                     std::uint16_t port,
                     std::string& error_message) {
    boost::system::error_code ec;

    if (socket.is_open()) {
        socket.close(ec);
        ec.clear();
    }

    auto endpoints = resolver.resolve(host, std::to_string(port), ec);
    if (ec || endpoints.begin() == endpoints.end()) {
        error_message = "udp resolve failed: " + ec.message();
        return false;
    }

    remote_endpoint = endpoints.begin()->endpoint();

    socket.open(remote_endpoint.protocol(), ec);
    if (ec) {
        error_message = "udp socket open failed: " + ec.message();
        return false;
    }

    socket.bind(udp::endpoint(remote_endpoint.protocol(), 0), ec);
    if (ec) {
        error_message = "udp bind failed: " + ec.message();
        socket.close(ec);
        return false;
    }

    socket.non_blocking(true, ec);
    if (ec) {
        error_message = "udp non_blocking failed: " + ec.message();
        socket.close(ec);
        return false;
    }

    return true;
}

bool wait_for_udp_datagram(udp::socket& socket,
                           udp::endpoint& sender_endpoint,
                           std::vector<std::uint8_t>& datagram,
                           std::chrono::milliseconds timeout,
                           std::string& error_message) {
    std::array<std::uint8_t, 2048> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    for (;;) {
        boost::system::error_code ec;
        const auto received = socket.receive_from(asio::buffer(buffer), sender_endpoint, 0, ec);
        if (!ec) {
            datagram.assign(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(received));
            return true;
        }

        if (ec != asio::error::would_block && ec != asio::error::try_again) {
            error_message = "udp receive failed: " + ec.message();
            return false;
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            error_message = "timeout waiting for udp response";
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

rudp::RudpConfig make_loadgen_rudp_config(const ClientOptions& options) {
    rudp::RudpConfig config{};
    config.handshake_timeout_ms = std::max<std::uint32_t>(250, options.read_timeout_ms);
    return config;
}

}  // namespace

TcpSessionClient::TcpSessionClient(ClientOptions options)
    : options_(options) {
    read_header_.resize(proto::k_header_bytes);
}

TcpSessionClient::~TcpSessionClient() {
    close();
}

bool TcpSessionClient::connect(const std::string& host, unsigned short port) {
    close();

    reset_state();
    work_guard_ = std::make_unique<WorkGuard>(asio::make_work_guard(io_));
    const auto connect_result = std::make_shared<AsyncConnectResult>();
    auto future = connect_result->promise.get_future();

    resolver_.async_resolve(
        host,
        std::to_string(port),
        [this, connect_result](const boost::system::error_code& ec, tcp::resolver::results_type results) {
            if (ec) {
                complete_async_result(connect_result, ec);
                return;
            }

            asio::async_connect(
                socket_,
                results,
                asio::bind_executor(
                    strand_,
                    [this, connect_result](const boost::system::error_code& connect_ec, const tcp::endpoint&) {
                        if (connect_ec) {
                            complete_async_result(connect_result, connect_ec);
                            return;
                        }

                        boost::system::error_code option_ec;
                        socket_.set_option(tcp::no_delay(true), option_ec);
                        if (option_ec) {
                            complete_async_result(connect_result, option_ec);
                            return;
                        }

                        connected_.store(true);
                        start_read_header();
                        complete_async_result(connect_result, {});
                    }));
        });

    io_thread_ = std::thread([this] { io_.run(); });

    if (future.wait_for(std::chrono::milliseconds(options_.connect_timeout_ms)) != std::future_status::ready) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++transport_.connect_failures;
            set_last_error_locked("connect timeout");
        }
        close();
        return false;
    }

    const auto ec = future.get();
    if (ec) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++transport_.connect_failures;
            set_last_error_locked(std::string("connect failed: ") + ec.message());
        }
        close();
        return false;
    }

    return true;
}

bool TcpSessionClient::login(const std::string& user, const std::string& token, LoginResult* result) {
    std::vector<std::uint8_t> payload;
    proto::write_lp_utf8(payload, user);
    proto::write_lp_utf8(payload, token);

    const auto version_offset = payload.size();
    payload.resize(version_offset + 4);
    proto::write_be16(proto::kProtocolVersionMajor, payload.data() + version_offset);
    proto::write_be16(proto::kProtocolVersionMinor, payload.data() + version_offset + 2);

    enqueue_packet(game_proto::MSG_LOGIN_REQ, 0, std::move(payload));

    Event login_event;
    if (!wait_for_event(std::chrono::milliseconds(options_.read_timeout_ms),
                        [](const Event& event) { return event.type == EventType::kLoginRes; },
                        "login response",
                        &login_event)) {
        return false;
    }

    if (result != nullptr) {
        *result = login_event.login;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        current_user_ = login_event.login.effective_user.empty() ? user : login_event.login.effective_user;
    }
    authenticated_.store(true);
    return true;
}

bool TcpSessionClient::wait_for_udp_bind_ticket(UdpBindTicket* result) {
    Event ticket_event;
    if (!wait_for_event(std::chrono::milliseconds(options_.read_timeout_ms),
                        [](const Event& event) { return event.type == EventType::kUdpBindTicket; },
                        "udp bind ticket",
                        &ticket_event)) {
        std::lock_guard<std::mutex> lock(mu_);
        ++transport_.bind_ticket_timeouts;
        return false;
    }

    if (ticket_event.bind_ticket.code != 0) {
        std::lock_guard<std::mutex> lock(mu_);
        set_last_error_locked(bind_ticket_error_message(ticket_event.bind_ticket));
        return false;
    }

    if (result != nullptr) {
        *result = ticket_event.bind_ticket;
    }
    trace_udp_attach(
        options_,
        "tcp",
        "ticket",
        "session=" + ticket_event.bind_ticket.session_id
            + " nonce=" + std::to_string(ticket_event.bind_ticket.nonce)
            + " expires_unix_ms=" + std::to_string(ticket_event.bind_ticket.expires_unix_ms)
            + " token_bytes=" + std::to_string(ticket_event.bind_ticket.token.size())
            + " message=" + ticket_event.bind_ticket.message);
    return true;
}

bool TcpSessionClient::join(const std::string& room, const std::string& password, SnapshotResult* result) {
    std::vector<std::uint8_t> payload;
    proto::write_lp_utf8(payload, room);
    proto::write_lp_utf8(payload, password);
    enqueue_packet(game_proto::MSG_JOIN_ROOM, 0, std::move(payload));

    const std::string expected_room = room.empty() ? std::string("lobby") : room;
    std::string current_user;
    {
        std::lock_guard<std::mutex> lock(mu_);
        current_user = current_user_;
    }

    Event broadcast_event;
    if (!wait_for_event(std::chrono::milliseconds(options_.read_timeout_ms),
                        [&expected_room, &current_user](const Event& event) {
                            return event.type == EventType::kSelfChat
                                && event.room == expected_room
                                && event.sender == "(system)"
                                && event.text.find(current_user) != std::string::npos;
                        },
                        "join confirmation broadcast",
                        &broadcast_event)) {
        return false;
    }

    if (result != nullptr) {
        result->current_room = broadcast_event.room;
    }
    return true;
}

bool TcpSessionClient::send_chat_and_wait_echo(const std::string& room, const std::string& text) {
    std::vector<std::uint8_t> payload;
    proto::write_lp_utf8(payload, room);
    proto::write_lp_utf8(payload, text);
    enqueue_packet(game_proto::MSG_CHAT_SEND, 0, std::move(payload));

    return wait_for_event(
        std::chrono::milliseconds(options_.read_timeout_ms),
        [&room, &text](const Event& event) {
            return event.type == EventType::kSelfChat
                && (event.flags & proto::FLAG_SELF) != 0
                && event.room == room
                && event.text == text;
        },
        "chat echo");
}

bool TcpSessionClient::send_ping_and_wait_pong() {
    enqueue_packet(proto::MSG_PING, 0);
    return wait_for_event(std::chrono::milliseconds(options_.read_timeout_ms),
                          [](const Event& event) { return event.type == EventType::kPong; },
                          "pong");
}

void TcpSessionClient::close() {
    closing_.store(true);
    connected_.store(false);

    {
        std::lock_guard<std::mutex> lock(mu_);
        cv_.notify_all();
    }

    boost::system::error_code ec;
    resolver_.cancel();
    socket_.cancel(ec);
    socket_.close(ec);

    if (work_guard_) {
        work_guard_->reset();
        work_guard_.reset();
    }

    io_.stop();
    if (io_thread_.joinable()) {
        io_thread_.join();
    }
}

std::string TcpSessionClient::last_error() const {
    std::lock_guard<std::mutex> lock(mu_);
    return last_error_;
}

TransportStats TcpSessionClient::transport_stats() const noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    return transport_;
}

void TcpSessionClient::reset_state() {
    io_.restart();
    closing_.store(false);
    connected_.store(false);
    authenticated_.store(false);
    seq_ = 1;
    read_header_.assign(proto::k_header_bytes, 0);
    read_body_.clear();
    write_queue_.clear();

    std::lock_guard<std::mutex> lock(mu_);
    events_.clear();
    last_error_.clear();
    current_user_.clear();
    transport_ = {};
}

void TcpSessionClient::start_read_header() {
    asio::async_read(
        socket_,
        asio::buffer(read_header_),
        asio::bind_executor(
            strand_,
            [this](const boost::system::error_code& ec, std::size_t) {
                if (ec) {
                    handle_disconnect(ec, "async_read(header)");
                    return;
                }

                proto::PacketHeader header{};
                proto::decode_header(read_header_.data(), header);
                start_read_body(header.length, header.msg_id, header.flags);
            }));
}

void TcpSessionClient::start_read_body(std::uint16_t payload_size, std::uint16_t msg_id, std::uint16_t flags) {
    if (payload_size == 0) {
        handle_frame(msg_id, flags, {});
        start_read_header();
        return;
    }

    read_body_.assign(payload_size, 0);
    asio::async_read(
        socket_,
        asio::buffer(read_body_),
        asio::bind_executor(
            strand_,
            [this, msg_id, flags](const boost::system::error_code& ec, std::size_t) {
                if (ec) {
                    handle_disconnect(ec, "async_read(body)");
                    return;
                }

                handle_frame(msg_id, flags, read_body_);
                start_read_header();
            }));
}

void TcpSessionClient::handle_frame(std::uint16_t msg_id,
                                    std::uint16_t flags,
                                    const std::vector<std::uint8_t>& payload) {
    if (msg_id == proto::MSG_PING) {
        enqueue_packet(proto::MSG_PONG, 0);
        return;
    }

    if (msg_id == proto::MSG_HELLO) {
        Event event;
        event.type = EventType::kHello;
        if (payload.size() >= 12) {
            event.capabilities = proto::read_be16(payload.data() + 4);
        }
        push_event(std::move(event));
        return;
    }

    if (msg_id == proto::MSG_PONG) {
        Event event;
        event.type = EventType::kPong;
        push_event(std::move(event));
        return;
    }

    if (msg_id == proto::MSG_ERR) {
        Event event;
        event.type = EventType::kError;
        std::span<const std::uint8_t> span(payload.data(), payload.size());
        if (span.size() >= 4) {
            event.error_code = proto::read_be16(span.data());
            const auto len = proto::read_be16(span.data() + 2);
            if (span.size() >= 4 + len) {
                event.error_message.assign(reinterpret_cast<const char*>(span.data() + 4), len);
            }
        }
        if (event.error_message.empty()) {
            event.error_message = "server returned MSG_ERR";
        }
        push_event(std::move(event));
        return;
    }

    if (msg_id == game_proto::MSG_UDP_BIND_RES) {
        UdpBindTicket ticket;
        if (!parse_udp_bind_ticket_payload(std::span<const std::uint8_t>(payload.data(), payload.size()), ticket)) {
            Event event;
            event.type = EventType::kError;
            event.error_message = "invalid udp bind ticket payload";
            push_event(std::move(event));
            return;
        }

        Event event;
        event.type = EventType::kUdpBindTicket;
        event.bind_ticket = std::move(ticket);
        push_event(std::move(event));
        return;
    }

    if (msg_id == game_proto::MSG_LOGIN_RES) {
        server::wire::v1::LoginRes message;
        Event event;
        event.type = EventType::kLoginRes;
        if (server::wire::codec::Decode(payload.data(), payload.size(), message)) {
            event.login.effective_user = message.effective_user();
            event.login.session_id = message.session_id();
            event.login.is_admin = message.is_admin();
            event.login.logical_session_id = message.logical_session_id();
            event.login.resume_token = message.resume_token();
            event.login.resume_expires_unix_ms = message.resume_expires_unix_ms();
            event.login.resumed = message.resumed();
            event.login.world_id = message.world_id();
            push_event(std::move(event));
            return;
        }

        std::span<const std::uint8_t> span(payload.data(), payload.size());
        if (!span.empty()) {
            span = span.subspan(1);
        }
        std::string ignored_message;
        proto::read_lp_utf8(span, ignored_message);
        if (!span.empty()) {
            auto next = span;
            proto::read_lp_utf8(next, event.login.effective_user);
            span = next;
        }
        if (span.size() >= 4) {
            event.login.session_id = proto::read_be32(span.data());
        }
        push_event(std::move(event));
        return;
    }

    if (msg_id == game_proto::MSG_STATE_SNAPSHOT) {
        server::wire::v1::StateSnapshot snapshot;
        if (!server::wire::codec::Decode(payload.data(), payload.size(), snapshot)) {
            return;
        }
        Event event;
        event.type = EventType::kSnapshot;
        event.snapshot.current_room = snapshot.current_room();
        push_event(std::move(event));
        return;
    }

    if (msg_id == game_proto::MSG_CHAT_BROADCAST) {
        server::wire::v1::ChatBroadcast broadcast;
        if (!server::wire::codec::Decode(payload.data(), payload.size(), broadcast)) {
            return;
        }
        const bool is_self = (flags & proto::FLAG_SELF) != 0;
        const bool is_system = broadcast.sender() == "(system)";
        if (!is_self && !is_system) {
            return;
        }

        Event event;
        event.type = EventType::kSelfChat;
        event.flags = flags;
        event.room = broadcast.room();
        event.sender = broadcast.sender();
        event.text = broadcast.text();
        push_event(std::move(event));
    }
}

void TcpSessionClient::enqueue_packet(std::uint16_t msg_id,
                                      std::uint16_t flags,
                                      std::vector<std::uint8_t> payload) {
    asio::post(
        strand_,
        [this, msg_id, flags, payload = std::move(payload)]() mutable {
            if (!connected_.load()) {
                return;
            }

            proto::PacketHeader header{};
            header.length = static_cast<std::uint16_t>(payload.size());
            header.msg_id = msg_id;
            header.flags = flags;
            header.seq = seq_++;
            header.utc_ts_ms32 = unix_time_ms32();

            auto buffer = std::make_shared<std::vector<std::uint8_t>>();
            buffer->resize(proto::k_header_bytes + payload.size());
            proto::encode_header(header, buffer->data());
            if (!payload.empty()) {
                std::memcpy(buffer->data() + proto::k_header_bytes, payload.data(), payload.size());
            }

            const bool writing = !write_queue_.empty();
            write_queue_.push_back(buffer);
            if (!writing) {
                drain_send_queue();
            }
        });
}

void TcpSessionClient::drain_send_queue() {
    if (write_queue_.empty()) {
        return;
    }

    auto buffer = write_queue_.front();
    asio::async_write(
        socket_,
        asio::buffer(*buffer),
        asio::bind_executor(
            strand_,
            [this, buffer](const boost::system::error_code& ec, std::size_t) {
                if (ec) {
                    write_queue_.clear();
                    handle_disconnect(ec, "async_write");
                    return;
                }

                write_queue_.pop_front();
                if (!write_queue_.empty()) {
                    drain_send_queue();
                }
            }));
}

void TcpSessionClient::handle_disconnect(const boost::system::error_code& ec, const char* context) {
    if (closing_.load()) {
        return;
    }

    if (!connected_.exchange(false)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        ++transport_.disconnects;
        set_last_error_locked(std::string(context) + ": " + ec.message());
    }
    cv_.notify_all();
}

void TcpSessionClient::push_event(Event event) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        events_.push_back(std::move(event));
    }
    cv_.notify_all();
}

bool TcpSessionClient::wait_for_event(std::chrono::milliseconds timeout,
                                      const std::function<bool(const Event&)>& predicate,
                                      const char* wait_label,
                                      Event* out) {
    std::unique_lock<std::mutex> lock(mu_);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        auto match_it = std::find_if(events_.begin(), events_.end(), predicate);
        if (match_it != events_.end()) {
            if (out != nullptr) {
                *out = *match_it;
            }
            events_.erase(match_it);
            return true;
        }

        auto error_it = std::find_if(events_.begin(), events_.end(), [](const Event& event) {
            return event.type == EventType::kError;
        });
        if (error_it != events_.end()) {
            set_last_error_locked(std::string("server error: ") + error_it->error_message);
            events_.erase(error_it);
            return false;
        }

        if (!connected_.load() || closing_.load()) {
            if (last_error_.empty()) {
                set_last_error_locked("connection closed");
            }
            return false;
        }

        if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
            ++transport_.read_timeouts;
            set_last_error_locked(std::string("timeout waiting for ") + wait_label);
            return false;
        }
    }
}

void TcpSessionClient::set_last_error_locked(std::string message) {
    last_error_ = std::move(message);
}

UdpSessionClient::UdpSessionClient(ClientOptions options)
    : options_(options)
    , tcp_client_(options) {
}

UdpSessionClient::~UdpSessionClient() {
    close();
}

bool UdpSessionClient::connect(const std::string& host, unsigned short port) {
    close();

    host_ = host;
    tcp_port_ = port;
    udp_port_ = options_.udp_port == 0 ? port : options_.udp_port;

    if (!tcp_client_.connect(host, port)) {
        set_last_error(tcp_client_.last_error());
        return false;
    }

    std::string error_message;
    if (!open_udp_socket(resolver_, socket_, remote_endpoint_, host_, udp_port_, error_message)) {
        set_last_error(std::move(error_message));
        tcp_client_.close();
        return false;
    }

    trace_udp_attach(
        options_,
        "udp",
        "socket-open",
        "host=" + host_ + " tcp_port=" + std::to_string(tcp_port_)
            + " udp_port=" + std::to_string(udp_port_)
            + " " + socket_endpoint_summary(socket_, remote_endpoint_));

    udp_bound_ = false;
    udp_seq_ = 1;
    return true;
}

bool UdpSessionClient::login(const std::string& user, const std::string& token, LoginResult* result) {
    if (!tcp_client_.login(user, token, result)) {
        set_last_error(tcp_client_.last_error());
        return false;
    }

    return perform_udp_bind();
}

bool UdpSessionClient::join(const std::string&, const std::string&, SnapshotResult*) {
    return unsupported_operation("join");
}

bool UdpSessionClient::send_chat_and_wait_echo(const std::string&, const std::string&) {
    return unsupported_operation("chat");
}

bool UdpSessionClient::send_ping_and_wait_pong() {
    return unsupported_operation("ping");
}

void UdpSessionClient::close() {
    udp_bound_ = false;

    boost::system::error_code ec;
    resolver_.cancel();
    socket_.close(ec);
    tcp_client_.close();
}

bool UdpSessionClient::is_connected() const noexcept {
    return tcp_client_.is_connected();
}

bool UdpSessionClient::authentication_completed() const noexcept {
    return tcp_client_.authentication_completed();
}

std::string UdpSessionClient::last_error() const {
    std::lock_guard<std::mutex> lock(mu_);
    return last_error_.empty() ? tcp_client_.last_error() : last_error_;
}

TransportStats UdpSessionClient::transport_stats() const noexcept {
    auto stats = tcp_client_.transport_stats();
    std::lock_guard<std::mutex> lock(mu_);
    accumulate_transport_stats(stats, transport_);
    return stats;
}

bool UdpSessionClient::perform_udp_bind() {
    UdpBindTicket ticket;
    if (!tcp_client_.wait_for_udp_bind_ticket(&ticket)) {
        set_last_error(tcp_client_.last_error());
        return false;
    }
    return send_udp_bind_request(ticket);
}

bool UdpSessionClient::send_udp_bind_request(const UdpBindTicket& ticket) {
    std::vector<std::uint8_t> payload;
    proto::write_lp_utf8(payload, ticket.session_id);
    write_be64(ticket.nonce, payload);
    write_be64(ticket.expires_unix_ms, payload);
    proto::write_lp_utf8(payload, ticket.token);

    const auto seq = udp_seq_++;
    const auto frame = make_frame(game_proto::MSG_UDP_BIND_REQ, seq, payload);

    {
        std::lock_guard<std::mutex> lock(mu_);
        ++transport_.udp_bind_attempts;
    }

    boost::system::error_code ec;
    trace_udp_attach(
        options_,
        "udp",
        "bind-send",
        "session=" + ticket.session_id
            + " nonce=" + std::to_string(ticket.nonce)
            + " expires_unix_ms=" + std::to_string(ticket.expires_unix_ms)
            + " target=" + endpoint_to_string(remote_endpoint_)
            + " " + frame_summary(std::span<const std::uint8_t>(frame.data(), frame.size())));
    socket_.send_to(asio::buffer(frame), remote_endpoint_, 0, ec);
    if (ec) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++transport_.udp_bind_failures;
        }
        set_last_error("udp bind send failed: " + ec.message());
        return false;
    }

    UdpBindTicket response;
    if (!wait_for_udp_bind_response(seq, response)) {
        std::lock_guard<std::mutex> lock(mu_);
        ++transport_.udp_bind_failures;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        ++transport_.udp_bind_successes;
    }
    udp_bound_ = true;
    return true;
}

bool UdpSessionClient::wait_for_udp_bind_response(std::uint32_t expected_seq, UdpBindTicket& ticket) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.read_timeout_ms);

    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            set_last_error(
                "timeout waiting for udp bind response (" + socket_endpoint_summary(socket_, remote_endpoint_) + ")");
            return false;
        }

        std::vector<std::uint8_t> datagram;
        udp::endpoint sender_endpoint;
        std::string error_message;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (!wait_for_udp_datagram(socket_, sender_endpoint, datagram, remaining, error_message)) {
            set_last_error(std::move(error_message));
            return false;
        }

        trace_udp_attach(
            options_,
            "udp",
            "bind-recv",
            "sender=" + endpoint_to_string(sender_endpoint)
                + " expected_remote=" + endpoint_to_string(remote_endpoint_)
                + " remaining_ms=" + std::to_string(remaining.count())
                + " " + frame_summary(std::span<const std::uint8_t>(datagram.data(), datagram.size())));

        if (datagram.size() < proto::k_header_bytes) {
            trace_udp_attach(options_, "udp", "bind-ignore", "reason=short-datagram");
            continue;
        }

        proto::PacketHeader header{};
        proto::decode_header(datagram.data(), header);
        if (header.length != (datagram.size() - proto::k_header_bytes)) {
            trace_udp_attach(
                options_,
                "udp",
                "bind-ignore",
                "reason=length-mismatch header_len=" + std::to_string(header.length)
                    + " actual_payload=" + std::to_string(datagram.size() - proto::k_header_bytes));
            continue;
        }
        if (header.msg_id != game_proto::MSG_UDP_BIND_RES || header.seq != expected_seq) {
            trace_udp_attach(
                options_,
                "udp",
                "bind-ignore",
                "reason=unexpected-frame expected_msg_id=" + std::to_string(game_proto::MSG_UDP_BIND_RES)
                    + " expected_seq=" + std::to_string(expected_seq)
                    + " actual_msg_id=" + std::to_string(header.msg_id)
                    + " actual_seq=" + std::to_string(header.seq));
            continue;
        }

        const auto payload = std::span<const std::uint8_t>(
            datagram.data() + proto::k_header_bytes,
            datagram.size() - proto::k_header_bytes);
        if (!parse_udp_bind_ticket_payload(payload, ticket)) {
            set_last_error("invalid udp bind response payload");
            return false;
        }
        if (ticket.code != 0) {
            set_last_error(bind_ticket_error_message(ticket));
            return false;
        }
        trace_udp_attach(
            options_,
            "udp",
            "bind-accepted",
            "sender=" + endpoint_to_string(sender_endpoint)
                + " session=" + ticket.session_id
                + " nonce=" + std::to_string(ticket.nonce)
                + " expires_unix_ms=" + std::to_string(ticket.expires_unix_ms));
        return true;
    }
}

bool UdpSessionClient::unsupported_operation(const char* operation) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        ++transport_.unsupported_operations;
    }
    set_last_error(unsupported_operation_message("udp", operation));
    return false;
}

void UdpSessionClient::set_last_error(std::string message) {
    std::lock_guard<std::mutex> lock(mu_);
    last_error_ = std::move(message);
}

RudpSessionClient::RudpSessionClient(ClientOptions options)
    : options_(options)
    , tcp_client_(options)
    , rudp_engine_(make_loadgen_rudp_config(options)) {
}

RudpSessionClient::~RudpSessionClient() {
    close();
}

bool RudpSessionClient::connect(const std::string& host, unsigned short port) {
    close();

    host_ = host;
    tcp_port_ = port;
    udp_port_ = options_.udp_port == 0 ? port : options_.udp_port;

    if (!tcp_client_.connect(host, port)) {
        set_last_error(tcp_client_.last_error());
        return false;
    }

    std::string error_message;
    if (!open_udp_socket(resolver_, socket_, remote_endpoint_, host_, udp_port_, error_message)) {
        set_last_error(std::move(error_message));
        tcp_client_.close();
        return false;
    }

    trace_udp_attach(
        options_,
        "rudp",
        "socket-open",
        "host=" + host_ + " tcp_port=" + std::to_string(tcp_port_)
            + " udp_port=" + std::to_string(udp_port_)
            + " " + socket_endpoint_summary(socket_, remote_endpoint_));

    udp_bound_ = false;
    udp_seq_ = 1;
    rudp_engine_.reset();
    return true;
}

bool RudpSessionClient::login(const std::string& user, const std::string& token, LoginResult* result) {
    if (!tcp_client_.login(user, token, result)) {
        set_last_error(tcp_client_.last_error());
        return false;
    }
    if (!perform_udp_bind()) {
        return false;
    }
    return attempt_rudp_attach();
}

bool RudpSessionClient::join(const std::string&, const std::string&, SnapshotResult*) {
    return unsupported_operation("join");
}

bool RudpSessionClient::send_chat_and_wait_echo(const std::string&, const std::string&) {
    return unsupported_operation("chat");
}

bool RudpSessionClient::send_ping_and_wait_pong() {
    return unsupported_operation("ping");
}

void RudpSessionClient::close() {
    udp_bound_ = false;
    rudp_engine_.reset();

    boost::system::error_code ec;
    resolver_.cancel();
    socket_.close(ec);
    tcp_client_.close();
}

bool RudpSessionClient::is_connected() const noexcept {
    return tcp_client_.is_connected();
}

bool RudpSessionClient::authentication_completed() const noexcept {
    return tcp_client_.authentication_completed();
}

std::string RudpSessionClient::last_error() const {
    std::lock_guard<std::mutex> lock(mu_);
    return last_error_.empty() ? tcp_client_.last_error() : last_error_;
}

TransportStats RudpSessionClient::transport_stats() const noexcept {
    auto stats = tcp_client_.transport_stats();
    std::lock_guard<std::mutex> lock(mu_);
    accumulate_transport_stats(stats, transport_);
    return stats;
}

bool RudpSessionClient::perform_udp_bind() {
    UdpBindTicket ticket;
    if (!tcp_client_.wait_for_udp_bind_ticket(&ticket)) {
        set_last_error(tcp_client_.last_error());
        return false;
    }
    return send_udp_bind_request(ticket);
}

bool RudpSessionClient::send_udp_bind_request(const UdpBindTicket& ticket) {
    std::vector<std::uint8_t> payload;
    proto::write_lp_utf8(payload, ticket.session_id);
    write_be64(ticket.nonce, payload);
    write_be64(ticket.expires_unix_ms, payload);
    proto::write_lp_utf8(payload, ticket.token);

    const auto seq = udp_seq_++;
    const auto frame = make_frame(game_proto::MSG_UDP_BIND_REQ, seq, payload);

    {
        std::lock_guard<std::mutex> lock(mu_);
        ++transport_.udp_bind_attempts;
    }

    boost::system::error_code ec;
    trace_udp_attach(
        options_,
        "rudp",
        "bind-send",
        "session=" + ticket.session_id
            + " nonce=" + std::to_string(ticket.nonce)
            + " expires_unix_ms=" + std::to_string(ticket.expires_unix_ms)
            + " target=" + endpoint_to_string(remote_endpoint_)
            + " " + frame_summary(std::span<const std::uint8_t>(frame.data(), frame.size())));
    socket_.send_to(asio::buffer(frame), remote_endpoint_, 0, ec);
    if (ec) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++transport_.udp_bind_failures;
        }
        set_last_error("udp bind send failed: " + ec.message());
        return false;
    }

    UdpBindTicket response;
    if (!wait_for_udp_bind_response(seq, response)) {
        std::lock_guard<std::mutex> lock(mu_);
        ++transport_.udp_bind_failures;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        ++transport_.udp_bind_successes;
    }
    udp_bound_ = true;
    return true;
}

bool RudpSessionClient::wait_for_udp_bind_response(std::uint32_t expected_seq, UdpBindTicket& ticket) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.read_timeout_ms);

    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            set_last_error(
                "timeout waiting for udp bind response (" + socket_endpoint_summary(socket_, remote_endpoint_) + ")");
            return false;
        }

        std::vector<std::uint8_t> datagram;
        udp::endpoint sender_endpoint;
        std::string error_message;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (!wait_for_udp_datagram(socket_, sender_endpoint, datagram, remaining, error_message)) {
            set_last_error(std::move(error_message));
            return false;
        }

        trace_udp_attach(
            options_,
            "rudp",
            "bind-recv",
            "sender=" + endpoint_to_string(sender_endpoint)
                + " expected_remote=" + endpoint_to_string(remote_endpoint_)
                + " remaining_ms=" + std::to_string(remaining.count())
                + " " + frame_summary(std::span<const std::uint8_t>(datagram.data(), datagram.size())));

        if (datagram.size() < proto::k_header_bytes) {
            trace_udp_attach(options_, "rudp", "bind-ignore", "reason=short-datagram");
            continue;
        }

        proto::PacketHeader header{};
        proto::decode_header(datagram.data(), header);
        if (header.length != (datagram.size() - proto::k_header_bytes)) {
            trace_udp_attach(
                options_,
                "rudp",
                "bind-ignore",
                "reason=length-mismatch header_len=" + std::to_string(header.length)
                    + " actual_payload=" + std::to_string(datagram.size() - proto::k_header_bytes));
            continue;
        }
        if (header.msg_id != game_proto::MSG_UDP_BIND_RES || header.seq != expected_seq) {
            trace_udp_attach(
                options_,
                "rudp",
                "bind-ignore",
                "reason=unexpected-frame expected_msg_id=" + std::to_string(game_proto::MSG_UDP_BIND_RES)
                    + " expected_seq=" + std::to_string(expected_seq)
                    + " actual_msg_id=" + std::to_string(header.msg_id)
                    + " actual_seq=" + std::to_string(header.seq));
            continue;
        }

        const auto payload = std::span<const std::uint8_t>(
            datagram.data() + proto::k_header_bytes,
            datagram.size() - proto::k_header_bytes);
        if (!parse_udp_bind_ticket_payload(payload, ticket)) {
            set_last_error("invalid udp bind response payload");
            return false;
        }
        if (ticket.code != 0) {
            set_last_error(bind_ticket_error_message(ticket));
            return false;
        }
        trace_udp_attach(
            options_,
            "rudp",
            "bind-accepted",
            "sender=" + endpoint_to_string(sender_endpoint)
                + " session=" + ticket.session_id
                + " nonce=" + std::to_string(ticket.nonce)
                + " expires_unix_ms=" + std::to_string(ticket.expires_unix_ms));
        return true;
    }
}

bool RudpSessionClient::attempt_rudp_attach() {
    rudp_engine_.reset();

    {
        std::lock_guard<std::mutex> lock(mu_);
        ++transport_.rudp_attach_attempts;
    }

    const auto connection_id = g_next_rudp_connection_id.fetch_add(1, std::memory_order_relaxed);
    auto hello = rudp_engine_.make_hello(connection_id, unix_time_ms64());

    boost::system::error_code ec;
    trace_udp_attach(
        options_,
        "rudp",
        "hello-send",
        "connection_id=" + std::to_string(connection_id)
            + " target=" + endpoint_to_string(remote_endpoint_)
            + " " + frame_summary(std::span<const std::uint8_t>(hello.data(), hello.size())));
    socket_.send_to(asio::buffer(hello), remote_endpoint_, 0, ec);
    if (ec) {
        set_last_error("rudp hello send failed: " + ec.message());
        return false;
    }

    return wait_for_rudp_attach_result();
}

bool RudpSessionClient::wait_for_rudp_attach_result() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.read_timeout_ms);

    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            break;
        }

        std::vector<std::uint8_t> datagram;
        std::string error_message;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (!wait_for_datagram(datagram, remaining, error_message)) {
            if (error_message == "timeout waiting for udp response") {
                break;
            }
            set_last_error(std::move(error_message));
            return false;
        }

        trace_udp_attach(
            options_,
            "rudp",
            "hello-recv",
            frame_summary(std::span<const std::uint8_t>(datagram.data(), datagram.size())));

        if (!rudp::looks_like_rudp(datagram)) {
            trace_udp_attach(options_, "rudp", "hello-ignore", "reason=not-rudp-frame");
            continue;
        }

        const auto now_unix_ms = unix_time_ms64();
        auto process_result = rudp_engine_.process_datagram(datagram, now_unix_ms);
        for (auto& egress : process_result.egress_datagrams) {
            boost::system::error_code send_ec;
            socket_.send_to(asio::buffer(egress), remote_endpoint_, 0, send_ec);
            if (send_ec) {
                set_last_error("rudp response send failed: " + send_ec.message());
                return false;
            }
        }
        if (process_result.handshake_established) {
            std::lock_guard<std::mutex> lock(mu_);
            ++transport_.rudp_attach_successes;
            return true;
        }
        if (process_result.fallback_required) {
            std::lock_guard<std::mutex> lock(mu_);
            ++transport_.rudp_attach_fallbacks;
            return true;
        }

        auto poll_result = rudp_engine_.poll(now_unix_ms);
        for (auto& egress : poll_result.egress_datagrams) {
            boost::system::error_code send_ec;
            socket_.send_to(asio::buffer(egress), remote_endpoint_, 0, send_ec);
            if (send_ec) {
                set_last_error("rudp poll send failed: " + send_ec.message());
                return false;
            }
        }
        if (poll_result.fallback_required) {
            std::lock_guard<std::mutex> lock(mu_);
            ++transport_.rudp_attach_fallbacks;
            return true;
        }
    }

    const auto now_unix_ms = unix_time_ms64();
    auto poll_result = rudp_engine_.poll(now_unix_ms);
    if (poll_result.fallback_required) {
        std::lock_guard<std::mutex> lock(mu_);
        ++transport_.rudp_attach_fallbacks;
        return true;
    }

    std::lock_guard<std::mutex> lock(mu_);
    ++transport_.rudp_attach_fallbacks;
    return true;
}

bool RudpSessionClient::wait_for_datagram(std::vector<std::uint8_t>& datagram,
                                          std::chrono::milliseconds timeout,
                                          std::string& error_message) {
    udp::endpoint sender_endpoint;
    return wait_for_udp_datagram(
        socket_,
        sender_endpoint,
        datagram,
        timeout,
        error_message);
}

bool RudpSessionClient::unsupported_operation(const char* operation) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        ++transport_.unsupported_operations;
    }
    set_last_error(unsupported_operation_message("rudp", operation));
    return false;
}

void RudpSessionClient::set_last_error(std::string message) {
    std::lock_guard<std::mutex> lock(mu_);
    last_error_ = std::move(message);
}

std::unique_ptr<SessionClient> make_session_client(TransportKind transport, ClientOptions options) {
    switch (transport) {
    case TransportKind::kTcp:
        return std::make_unique<TcpSessionClient>(options);
    case TransportKind::kUdp:
        return std::make_unique<UdpSessionClient>(options);
    case TransportKind::kRudp:
        return std::make_unique<RudpSessionClient>(options);
    }
    throw std::runtime_error("unknown transport kind");
}

}  // namespace loadgen
