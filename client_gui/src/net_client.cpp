#include "client/net_client.hpp"

#include "server/core/protocol/packet.hpp"
#include "server/core/protocol/protocol_flags.hpp"
#include "server/core/protocol/system_opcodes.hpp"
#include "server/core/protocol/version.hpp"
#include "server/protocol/game_opcodes.hpp"
#include "server/wire/codec.hpp"
#include "wire.pb.h"

#include <boost/system/error_code.hpp>

#include <chrono>
#include <cstring>

using namespace std::chrono;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
namespace proto = server::core::protocol;
namespace game_proto = server::protocol;

/**
 * @brief 클라이언트 TCP 송수신/프레임 파싱/콜백 디스패치 구현입니다.
 *
 * strand 기반 직렬화로 송수신 경합을 방지하고,
 * opcode별 핸들링을 한 경로로 모아 UI 계층이 단순한 이벤트 소비자 역할만 하도록 합니다.
 */
NetClient::NetClient() : socket_(io_), strand_(io_.get_executor()) {
    read_header_.resize(proto::k_header_bytes);
}

NetClient::~NetClient() {
    close();
}

bool NetClient::connect(const std::string& host, unsigned short port) {
    close();
    if (on_state_changed_) {
        on_state_changed_(TransportState::CONNECTING);
    }

    boost::system::error_code ec;
    io_.restart();

    auto endpoints = resolver_.resolve(host, std::to_string(port), ec);
    if (ec) {
        if (on_state_changed_) {
            on_state_changed_(TransportState::DISCONNECTED);
        }
        return false;
    }

    asio::connect(socket_, endpoints, ec);
    if (ec) {
        if (on_state_changed_) {
            on_state_changed_(TransportState::DISCONNECTED);
        }
        return false;
    }

    socket_.set_option(tcp::no_delay(true), ec);
    running_.store(true);
    connected_.store(true);
    seq_ = 1;
    ping_in_flight_ = false;

    work_guard_ = std::make_unique<WorkGuard>(asio::make_work_guard(io_));
    ping_timer_ = std::make_unique<Timer>(io_);

    start_read_header();
    schedule_ping();
    if (auto_ping_enabled_.load()) {
        send_ping();
    }

    io_thread_ = std::thread([this] { io_.run(); });
    if (on_state_changed_) {
        on_state_changed_(TransportState::CONNECTED);
    }
    return true;
}

void NetClient::close() {
    running_.store(false);
    const bool was_connected = connected_.exchange(false);

    if (ping_timer_) {
        ping_timer_->cancel();
        ping_timer_.reset();
    }

    boost::system::error_code ec;
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
    io_.restart();
    write_queue_.clear();
    ping_in_flight_ = false;

    if (was_connected && on_state_changed_) {
        on_state_changed_(TransportState::DISCONNECTED);
    }
}

void NetClient::start_read_header() {
    asio::async_read(
        socket_,
        asio::buffer(read_header_),
        boost::asio::bind_executor(
            strand_,
            [this](const boost::system::error_code& ec, std::size_t) {
                if (ec) {
                    handle_disconnect(ec, "async_read(header)");
                    return;
                }
                proto::PacketHeader header{};
                proto::decode_header(read_header_.data(), header);

                read_body_.assign(header.length, 0);
                start_read_body(header);
            }));
}

void NetClient::start_read_body(const proto::PacketHeader& header) {
    if (read_body_.empty()) {
        handle_packet(header, std::span<const std::uint8_t>{});
        start_read_header();
        return;
    }

    asio::async_read(
        socket_,
        asio::buffer(read_body_),
        boost::asio::bind_executor(
            strand_,
            [this, header](const boost::system::error_code& ec, std::size_t) {
                if (ec) {
                    handle_disconnect(ec, "async_read(body)");
                    return;
                }
                handle_packet(header, std::span<const std::uint8_t>(read_body_.data(), read_body_.size()));
                start_read_header();
            }));
}

void NetClient::schedule_ping() {
    if (!ping_timer_) return;
    ping_timer_->expires_after(std::chrono::seconds(10));
    ping_timer_->async_wait(boost::asio::bind_executor(
        strand_,
        [this](const boost::system::error_code& ec) {
            if (ec || !running_.load()) {
                return;
            }
            if (auto_ping_enabled_.load()) {
                send_ping_on_strand();
            }
            schedule_ping();
        }));
}

void NetClient::send_ping() {
    asio::post(strand_, [this]() {
        send_ping_on_strand();
    });
}

void NetClient::set_auto_ping_enabled(bool enabled) {
    auto_ping_enabled_.store(enabled);
}

void NetClient::send_ping_on_strand() {
    if (!connected_.load()) {
        return;
    }
    last_ping_sent_ = steady_clock::now();
    ping_in_flight_ = true;
    enqueue_packet_on_strand(proto::MSG_PING, 0);
}

void NetClient::enqueue_packet(std::uint16_t msg_id, std::uint16_t flags, std::vector<std::uint8_t> payload) {
    asio::post(strand_, [this, msg_id, flags, payload = std::move(payload)]() mutable {
        enqueue_packet_on_strand(msg_id, flags, std::move(payload));
    });
}

void NetClient::enqueue_packet_on_strand(std::uint16_t msg_id, std::uint16_t flags, std::vector<std::uint8_t> payload) {
    if (!connected_.load()) {
        return;
    }

    proto::PacketHeader header{};
    header.length = static_cast<std::uint16_t>(payload.size());
    header.msg_id = msg_id;
    header.flags = flags;
    header.seq = seq_++;
    const auto now64 = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    header.utc_ts_ms32 = static_cast<std::uint32_t>(now64 & 0xFFFFFFFFu);

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
}

void NetClient::drain_send_queue() {
    if (write_queue_.empty()) return;
    auto buffer = write_queue_.front();
    asio::async_write(
        socket_,
        asio::buffer(*buffer),
        boost::asio::bind_executor(
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

void NetClient::handle_packet(const proto::PacketHeader& hh, std::span<const std::uint8_t> in) {
    if (hh.msg_id == proto::MSG_PING) {
        enqueue_packet_on_strand(proto::MSG_PONG, 0);
        return;
    }

    if (hh.msg_id == proto::MSG_PONG) {
        if (ping_in_flight_) {
            ping_in_flight_ = false;
            const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - last_ping_sent_).count();
            const auto rtt_ms = static_cast<std::uint32_t>(elapsed < 0 ? 0 : elapsed);
            if (on_pong_) {
                on_pong_(rtt_ms);
            }
        }
        return;
    }

    if (hh.msg_id == proto::MSG_ERR) {
        std::uint16_t code = 0, len = 0;
        std::string msg;
        if (in.size() >= 4) {
            code = proto::read_be16(in.data());
            len = proto::read_be16(in.data() + 2);
            in = in.subspan(4);
            if (in.size() >= len) {
                msg.assign(reinterpret_cast<const char*>(in.data()), len);
            }
        }
        if (on_err_) on_err_(code, msg);
        return;
    }

    if (hh.msg_id == game_proto::MSG_LOGIN_RES) {
        server::wire::v1::LoginRes pb;
        if (server::wire::codec::Decode(in.data(), in.size(), pb)) {
            if (on_session_info_) {
                on_session_info_({pb.principal_id(), pb.auth_session_id(), pb.display_name(),
                    pb.logical_session_id(), pb.resume_token(), pb.resume_expires_unix_ms(), pb.resumed()});
            }
            if (on_login_) on_login_(pb.effective_user(), pb.session_id(), pb.is_admin());
        } else {
            auto cur = in;
            if (!cur.empty()) cur = cur.subspan(1);
            std::string m;
            proto::read_lp_utf8(cur, m);
            std::string effective;
            if (!cur.empty()) {
                auto tmp = cur;
                std::string t;
                if (proto::read_lp_utf8(tmp, t)) {
                    effective = std::move(t);
                    cur = tmp;
                }
            }
            std::uint32_t sid = 0;
            if (cur.size() >= 4) sid = proto::read_be32(cur.data());
            if (on_login_) on_login_(effective, sid, false);
        }
        return;
    }

    if (hh.msg_id == game_proto::MSG_CHAT_BROADCAST) {
        server::wire::v1::ChatBroadcast pb;
        if (server::wire::codec::Decode(in.data(), in.size(), pb)) {
            if (on_bcast_) {
                on_bcast_(pb.room(), pb.sender(), pb.text(), hh.flags, pb.sender_sid(), pb.message_id(), pb.room_id(), pb.ts_ms());
            }
        }
        return;
    }

    if (hh.msg_id == game_proto::MSG_ROOM_USERS) {
        std::string room;
        proto::read_lp_utf8(in, room);
        std::vector<std::string> list;
        while (!in.empty()) {
            std::string entry;
            if (!proto::read_lp_utf8(in, entry)) break;
            list.emplace_back(std::move(entry));
        }
        if (on_room_users_) on_room_users_(std::move(room), std::move(list));
        return;
    }

    if (hh.msg_id == game_proto::MSG_STATE_SNAPSHOT) {
        server::wire::v1::StateSnapshot pb;
        if (server::wire::codec::Decode(in.data(), in.size(), pb)) {
            std::vector<std::string> rooms;
            std::vector<bool> locked;
            rooms.reserve(pb.rooms_size());
            locked.reserve(pb.rooms_size());
            for (const auto& r : pb.rooms()) {
                rooms.push_back(r.name());
                locked.push_back(r.locked());
            }
            std::vector<std::string> users;
            users.reserve(pb.users_size());
            for (const auto& u : pb.users()) {
                users.push_back(u);
            }
            std::vector<SnapshotMessage> messages;
            messages.reserve(pb.messages_size());
            for (const auto& m : pb.messages()) {
                messages.push_back({m.sender(), m.text(), m.ts_ms(), m.id()});
            }
            if (on_snapshot_) {
                on_snapshot_(pb.current_room(), std::move(rooms), std::move(users), std::move(locked), std::move(messages), pb.your_name(), pb.watermark(), pb.room_id(), pb.history_available());
            }
        }
        return;
    }

    if (hh.msg_id == game_proto::MSG_WHISPER_BROADCAST) {
        std::string sender; std::string recipient; std::string text;
        bool outgoing = false;
        proto::read_lp_utf8(in, sender);
        proto::read_lp_utf8(in, recipient);
        proto::read_lp_utf8(in, text);
        if (!in.empty()) outgoing = in.front() != 0;
        if (on_whisper_) on_whisper_(std::move(sender), std::move(recipient), std::move(text), outgoing);
        return;
    }

    if (hh.msg_id == game_proto::MSG_WHISPER_RES) {
        bool ok = false;
        std::string reason;
        if (!in.empty()) {
            ok = in.front() != 0;
            in = in.subspan(1);
        }
        proto::read_lp_utf8(in, reason);
        if (on_whisper_result_) on_whisper_result_(ok, std::move(reason));
        return;
    }

    if (hh.msg_id == proto::MSG_HELLO) {
        std::uint16_t caps = 0;
        if (in.size() >= 12) {
            caps = proto::read_be16(in.data() + 4);
        }
        if (on_hello_) on_hello_(caps);
        return;
    }

    if (hh.msg_id == game_proto::MSG_REFRESH_NOTIFY) {
        if (on_refresh_notify_) on_refresh_notify_();
        return;
    }
}

void NetClient::handle_disconnect(const boost::system::error_code& ec, const char* context) {
    if (!running_.exchange(false)) {
        return;
    }
    connected_.store(false);
    ping_in_flight_ = false;

    boost::system::error_code ignore;
    socket_.cancel(ignore);
    socket_.close(ignore);
    if (ping_timer_) {
        ping_timer_->cancel();
    }
    write_queue_.clear();

    if (on_state_changed_) {
        on_state_changed_(TransportState::DISCONNECTED);
    }

    if (on_disconnected_) {
        std::string reason = ec ? ec.message() : "connection closed";
        if (context) {
            reason = std::string(context) + ": " + reason;
        }
        on_disconnected_(std::move(reason));
    }
}

void NetClient::send_login(const std::string& user, const std::string& token) {
    std::vector<std::uint8_t> p;
    proto::write_lp_utf8(p, user);
    proto::write_lp_utf8(p, token);

    const auto version_offset = p.size();
    p.resize(version_offset + 4);
    proto::write_be16(proto::kProtocolVersionMajor, p.data() + version_offset);
    proto::write_be16(proto::kProtocolVersionMinor, p.data() + version_offset + 2);

    enqueue_packet(game_proto::MSG_LOGIN_REQ, 0, std::move(p));
}

void NetClient::send_join(const std::string& room, const std::string& password) {
    std::vector<std::uint8_t> p;
    proto::write_lp_utf8(p, room);
    proto::write_lp_utf8(p, password);
    enqueue_packet(game_proto::MSG_JOIN_ROOM, 0, std::move(p));
}

void NetClient::send_leave(const std::string& room) {
    std::vector<std::uint8_t> p;
    proto::write_lp_utf8(p, room);
    enqueue_packet(game_proto::MSG_LEAVE_ROOM, 0, std::move(p));
}

void NetClient::send_chat(const std::string& room, const std::string& text) {
    std::vector<std::uint8_t> p;
    proto::write_lp_utf8(p, room);
    proto::write_lp_utf8(p, text);
    enqueue_packet(game_proto::MSG_CHAT_SEND, 0, std::move(p));
}

void NetClient::send_refresh(const std::string& current_room) {
    std::vector<std::uint8_t> p;
    proto::write_lp_utf8(p, current_room);
    enqueue_packet(game_proto::MSG_REFRESH_REQ, 0, std::move(p));
}

void NetClient::send_who(const std::string& room) {
    std::vector<std::uint8_t> p;
    proto::write_lp_utf8(p, room);
    enqueue_packet(game_proto::MSG_ROOM_USERS_REQ, 0, std::move(p));
}

void NetClient::send_rooms(const std::string& current_room) {
    std::vector<std::uint8_t> p;
    proto::write_lp_utf8(p, current_room);
    enqueue_packet(game_proto::MSG_ROOMS_REQ, 0, std::move(p));
}

void NetClient::send_whisper(const std::string& user, const std::string& text) {
    std::vector<std::uint8_t> p;
    proto::write_lp_utf8(p, user);
    proto::write_lp_utf8(p, text);
    enqueue_packet(game_proto::MSG_WHISPER_REQ, 0, std::move(p));
}
