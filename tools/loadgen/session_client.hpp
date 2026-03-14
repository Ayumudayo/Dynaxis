#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>

#include "server/core/net/rudp/rudp_engine.hpp"

namespace loadgen {

struct TransportStats {
    std::uint64_t connect_failures{0};
    std::uint64_t read_timeouts{0};
    std::uint64_t disconnects{0};
    std::uint64_t unsupported_operations{0};
    std::uint64_t bind_ticket_timeouts{0};
    std::uint64_t udp_bind_attempts{0};
    std::uint64_t udp_bind_successes{0};
    std::uint64_t udp_bind_failures{0};
    std::uint64_t rudp_attach_attempts{0};
    std::uint64_t rudp_attach_successes{0};
    std::uint64_t rudp_attach_fallbacks{0};
};

inline void accumulate_transport_stats(TransportStats& target, const TransportStats& source) noexcept {
    target.connect_failures += source.connect_failures;
    target.read_timeouts += source.read_timeouts;
    target.disconnects += source.disconnects;
    target.unsupported_operations += source.unsupported_operations;
    target.bind_ticket_timeouts += source.bind_ticket_timeouts;
    target.udp_bind_attempts += source.udp_bind_attempts;
    target.udp_bind_successes += source.udp_bind_successes;
    target.udp_bind_failures += source.udp_bind_failures;
    target.rudp_attach_attempts += source.rudp_attach_attempts;
    target.rudp_attach_successes += source.rudp_attach_successes;
    target.rudp_attach_fallbacks += source.rudp_attach_fallbacks;
}

struct ClientOptions {
    std::uint32_t connect_timeout_ms{5000};
    std::uint32_t read_timeout_ms{5000};
    std::uint16_t udp_port{0};
    bool trace_udp_attach{false};
};

struct LoginResult {
    std::string effective_user;
    std::uint32_t session_id{0};
    bool is_admin{false};
    std::string logical_session_id;
    std::string resume_token;
    std::uint64_t resume_expires_unix_ms{0};
    bool resumed{false};
    std::string world_id;
};

struct SnapshotResult {
    std::string current_room;
};

struct UdpBindTicket {
    std::uint16_t code{0};
    std::string session_id;
    std::uint64_t nonce{0};
    std::uint64_t expires_unix_ms{0};
    std::string token;
    std::string message;
};

enum class TransportKind {
    kTcp,
    kUdp,
    kRudp,
};

class SessionClient {
public:
    virtual ~SessionClient() = default;

    SessionClient(const SessionClient&) = delete;
    SessionClient& operator=(const SessionClient&) = delete;
    SessionClient(SessionClient&&) = delete;
    SessionClient& operator=(SessionClient&&) = delete;

    virtual TransportKind transport_kind() const noexcept = 0;
    virtual bool connect(const std::string& host, unsigned short port) = 0;
    virtual bool login(const std::string& user, const std::string& token, LoginResult* result = nullptr) = 0;
    virtual bool join(const std::string& room, const std::string& password, SnapshotResult* result = nullptr) = 0;
    virtual bool send_chat_and_wait_echo(const std::string& room, const std::string& text) = 0;
    virtual bool send_ping_and_wait_pong() = 0;
    virtual void close() = 0;

    virtual bool is_connected() const noexcept = 0;
    virtual bool authentication_completed() const noexcept = 0;
    virtual std::string last_error() const = 0;
    virtual TransportStats transport_stats() const noexcept = 0;

protected:
    SessionClient() = default;
};

class TcpSessionClient final : public SessionClient {
public:
    explicit TcpSessionClient(ClientOptions options);
    ~TcpSessionClient() override;

    TransportKind transport_kind() const noexcept override { return TransportKind::kTcp; }
    bool connect(const std::string& host, unsigned short port) override;
    bool login(const std::string& user, const std::string& token, LoginResult* result = nullptr) override;
    bool wait_for_udp_bind_ticket(UdpBindTicket* result = nullptr);
    bool join(const std::string& room, const std::string& password, SnapshotResult* result = nullptr) override;
    bool send_chat_and_wait_echo(const std::string& room, const std::string& text) override;
    bool send_ping_and_wait_pong() override;
    void close() override;

    bool is_connected() const noexcept override { return connected_.load(); }
    bool authentication_completed() const noexcept override { return authenticated_.load(); }
    std::string last_error() const override;
    TransportStats transport_stats() const noexcept override;

private:
    enum class EventType {
        kHello,
        kLoginRes,
        kSnapshot,
        kPong,
        kSelfChat,
        kUdpBindTicket,
        kError,
    };

    struct Event {
        EventType type{EventType::kError};
        std::uint16_t capabilities{0};
        std::uint16_t flags{0};
        LoginResult login;
        UdpBindTicket bind_ticket;
        SnapshotResult snapshot;
        std::string room;
        std::string sender;
        std::string text;
        std::uint16_t error_code{0};
        std::string error_message;
    };

    using Tcp = boost::asio::ip::tcp;
    using Strand = boost::asio::strand<boost::asio::io_context::executor_type>;
    using WorkGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;

    void reset_state();
    void start_read_header();
    void start_read_body(std::uint16_t payload_size, std::uint16_t msg_id, std::uint16_t flags);
    void handle_frame(std::uint16_t msg_id, std::uint16_t flags, const std::vector<std::uint8_t>& payload);
    void enqueue_packet(std::uint16_t msg_id, std::uint16_t flags, std::vector<std::uint8_t> payload = {});
    void drain_send_queue();
    void handle_disconnect(const boost::system::error_code& ec, const char* context);
    void push_event(Event event);
    bool wait_for_event(std::chrono::milliseconds timeout,
                        const std::function<bool(const Event&)>& predicate,
                        const char* wait_label,
                        Event* out = nullptr);
    void set_last_error_locked(std::string message);

    ClientOptions options_;
    boost::asio::io_context io_;
    Tcp::resolver resolver_{io_};
    Tcp::socket socket_{io_};
    Strand strand_{io_.get_executor()};
    std::unique_ptr<WorkGuard> work_guard_;
    std::thread io_thread_;

    std::deque<std::shared_ptr<std::vector<std::uint8_t>>> write_queue_;
    std::vector<std::uint8_t> read_header_;
    std::vector<std::uint8_t> read_body_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> closing_{false};
    std::atomic<bool> authenticated_{false};
    std::uint32_t seq_{1};

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Event> events_;
    std::string last_error_;
    std::string current_user_;
    TransportStats transport_;
};

class UdpSessionClient final : public SessionClient {
public:
    explicit UdpSessionClient(ClientOptions options);
    ~UdpSessionClient() override;

    TransportKind transport_kind() const noexcept override { return TransportKind::kUdp; }
    bool connect(const std::string& host, unsigned short port) override;
    bool login(const std::string& user, const std::string& token, LoginResult* result = nullptr) override;
    bool join(const std::string& room, const std::string& password, SnapshotResult* result = nullptr) override;
    bool send_chat_and_wait_echo(const std::string& room, const std::string& text) override;
    bool send_ping_and_wait_pong() override;
    void close() override;

    bool is_connected() const noexcept override;
    bool authentication_completed() const noexcept override;
    std::string last_error() const override;
    TransportStats transport_stats() const noexcept override;

private:
    using Udp = boost::asio::ip::udp;

    bool perform_udp_bind();
    bool send_udp_bind_request(const UdpBindTicket& ticket);
    bool wait_for_udp_bind_response(std::uint32_t expected_seq, UdpBindTicket& ticket);
    bool unsupported_operation(const char* operation);
    void set_last_error(std::string message);

    ClientOptions options_;
    TcpSessionClient tcp_client_;
    boost::asio::io_context io_;
    Udp::resolver resolver_{io_};
    Udp::socket socket_{io_};
    Udp::endpoint remote_endpoint_{};
    std::string host_;
    std::uint16_t tcp_port_{0};
    std::uint16_t udp_port_{0};
    std::uint32_t udp_seq_{1};
    bool udp_bound_{false};

    mutable std::mutex mu_;
    std::string last_error_;
    TransportStats transport_;
};

class RudpSessionClient final : public SessionClient {
public:
    explicit RudpSessionClient(ClientOptions options);
    ~RudpSessionClient() override;

    TransportKind transport_kind() const noexcept override { return TransportKind::kRudp; }
    bool connect(const std::string& host, unsigned short port) override;
    bool login(const std::string& user, const std::string& token, LoginResult* result = nullptr) override;
    bool join(const std::string& room, const std::string& password, SnapshotResult* result = nullptr) override;
    bool send_chat_and_wait_echo(const std::string& room, const std::string& text) override;
    bool send_ping_and_wait_pong() override;
    void close() override;

    bool is_connected() const noexcept override;
    bool authentication_completed() const noexcept override;
    std::string last_error() const override;
    TransportStats transport_stats() const noexcept override;

private:
    using Udp = boost::asio::ip::udp;

    bool perform_udp_bind();
    bool send_udp_bind_request(const UdpBindTicket& ticket);
    bool wait_for_udp_bind_response(std::uint32_t expected_seq, UdpBindTicket& ticket);
    bool attempt_rudp_attach();
    bool wait_for_rudp_attach_result();
    bool wait_for_datagram(std::vector<std::uint8_t>& datagram,
                           std::chrono::milliseconds timeout,
                           std::string& error_message);
    bool unsupported_operation(const char* operation);
    void set_last_error(std::string message);

    ClientOptions options_;
    TcpSessionClient tcp_client_;
    boost::asio::io_context io_;
    Udp::resolver resolver_{io_};
    Udp::socket socket_{io_};
    Udp::endpoint remote_endpoint_{};
    std::string host_;
    std::uint16_t tcp_port_{0};
    std::uint16_t udp_port_{0};
    std::uint32_t udp_seq_{1};
    bool udp_bound_{false};
    server::core::net::rudp::RudpEngine rudp_engine_;

    mutable std::mutex mu_;
    std::string last_error_;
    TransportStats transport_;
};

std::unique_ptr<SessionClient> make_session_client(TransportKind transport, ClientOptions options);

}  // namespace loadgen
