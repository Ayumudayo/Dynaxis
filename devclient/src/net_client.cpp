// UTF-8, 한글 주석
#include "client/net_client.hpp"

#include "server/core/protocol/frame.hpp"
#include "server/core/protocol/system_opcodes.hpp"
#include "server/protocol/game_opcodes.hpp"
#include "server/core/protocol/protocol_flags.hpp"
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

// -----------------------------------------------------------------------------
// NetClient 생성자
// -----------------------------------------------------------------------------
// 단일 io_context 위에서 strand를 사용하여 모든 I/O 핸들러의 순차적 실행을 보장합니다.
// 이는 멀티스레드 환경에서도 동기화 문제 없이 소켓 I/O를 처리할 수 있게 해줍니다.
NetClient::NetClient() : socket_(io_), strand_(io_.get_executor()) {
    read_header_.resize(proto::k_header_bytes);
}

NetClient::~NetClient() {
    close();
}

// -----------------------------------------------------------------------------
// 서버 연결
// -----------------------------------------------------------------------------
// 지정된 호스트와 포트로 TCP 연결을 시도합니다.
// 연결 성공 시 I/O 스레드를 시작하고, 헤더 읽기 및 핑 타이머를 가동합니다.
bool NetClient::connect(const std::string& host, unsigned short port) {
    close(); // 기존 연결이 있다면 정리
    boost::system::error_code ec;
    io_.restart(); // io_context 재사용을 위해 리셋

    // 호스트 이름 해석 (DNS Lookup)
    auto endpoints = resolver_.resolve(host, std::to_string(port), ec);
    if (ec) return false;

    // 연결 시도
    asio::connect(socket_, endpoints, ec);
    if (ec) return false;

    // Nagle 알고리즘 비활성화 (지연 시간 최소화)
    socket_.set_option(tcp::no_delay(true));
    running_.store(true);
    connected_.store(true);
    seq_ = 1;

    // io_context가 작업이 없어도 종료되지 않도록 WorkGuard 생성
    work_guard_ = std::make_unique<WorkGuard>(asio::make_work_guard(io_));
    ping_timer_ = std::make_unique<Timer>(io_);

    start_read_header(); // 연결 직후 서버로부터의 메시지 수신 대기
    schedule_ping();     // 주기적인 핑 전송 예약

    // 별도 스레드에서 io_context 실행 (비동기 I/O 처리)
    io_thread_ = std::thread([this] { io_.run(); });
    return true;
}

// -----------------------------------------------------------------------------
// 연결 종료
// -----------------------------------------------------------------------------
// 소켓을 닫고, 타이머와 I/O 스레드를 정리합니다.
void NetClient::close() {
    // 연결 상태 플래그를 false로 설정하여 추가적인 요청 전송을 막습니다.
    running_.store(false);
    connected_.store(false);

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
}

// -----------------------------------------------------------------------------
// 헤더 읽기 시작
// -----------------------------------------------------------------------------
// 고정 길이 헤더(4바이트 등)를 비동기로 읽습니다.
// 헤더를 다 읽으면 콜백에서 파싱 후 본문(Body) 읽기를 시작합니다.
// 이 과정은 재귀적으로 호출되어 지속적인 수신 루프를 형성합니다.
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
                proto::FrameHeader header{};
                proto::decode_header(read_header_.data(), header);
                
                // 헤더에 명시된 길이만큼 본문 버퍼 할당
                read_body_.assign(header.length, 0);
                start_read_body(header);
            }));
}

// -----------------------------------------------------------------------------
// 본문 읽기 시작
// -----------------------------------------------------------------------------
// 헤더에서 파싱한 길이만큼 본문 데이터를 비동기로 읽습니다.
void NetClient::start_read_body(const proto::FrameHeader& header) {
    if (read_body_.empty()) {
        // 페이로드가 없는 메시지(예: PING)는 바로 처리하고 다음 헤더 읽기로 넘어갑니다.
        handle_frame(header, std::span<const std::uint8_t>{});
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
                // 메시지 처리 핸들러 호출
                handle_frame(header, std::span<const std::uint8_t>(read_body_.data(), read_body_.size()));
                // 다음 메시지 헤더 읽기 시작
                start_read_header();
            }));
}

// -----------------------------------------------------------------------------
// 핑 스케줄링
// -----------------------------------------------------------------------------
// 서버와의 연결 유지를 위해 주기적으로 PING 메시지를 보냅니다.
void NetClient::schedule_ping() {
    if (!ping_timer_) return;
    // 8초마다 PING 전송 (서버 타임아웃 방지)
    ping_timer_->expires_after(std::chrono::seconds(8));
    ping_timer_->async_wait(boost::asio::bind_executor(
        strand_,
        [this](const boost::system::error_code& ec) {
            if (ec || !running_.load()) {
                return;
            }
            enqueue_frame(proto::MSG_PING, 0);
            schedule_ping(); // 다음 핑 예약
        }));
}

// -----------------------------------------------------------------------------
// 프레임 전송 큐에 추가
// -----------------------------------------------------------------------------
// 메시지를 직렬화하여 전송 큐에 추가하고, 전송 중이 아니라면 전송을 시작합니다.
// strand를 사용하여 스레드 안전성을 보장합니다.
// 즉, 여러 스레드에서 동시에 send를 호출해도 큐에 순서대로 쌓이고 하나씩 전송됩니다.
void NetClient::enqueue_frame(std::uint16_t msg_id, std::uint16_t flags, std::vector<std::uint8_t> payload) {
    asio::post(strand_, [this, msg_id, flags, payload = std::move(payload)]() mutable {
        if (!connected_.load()) {
            return;
        }

        proto::FrameHeader header{};
        header.length = static_cast<std::uint16_t>(payload.size());
        header.msg_id = msg_id;
        header.flags = flags;
        header.seq = seq_++;
        auto now64 = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
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
    });
}

// -----------------------------------------------------------------------------
// 전송 큐 처리
// -----------------------------------------------------------------------------
// 큐에 있는 메시지를 하나씩 순차적으로 비동기 전송합니다.
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
                    drain_send_queue(); // 남은 메시지가 있다면 계속 전송
                }
            }));
}

// -----------------------------------------------------------------------------
// 수신 프레임 처리
// -----------------------------------------------------------------------------
// 수신된 메시지의 ID(Opcode)에 따라 적절한 처리를 수행합니다.
void NetClient::handle_frame(const proto::FrameHeader& hh, std::span<const std::uint8_t> in) {
    if (hh.msg_id == proto::MSG_PING) {
        // 서버에서 PING이 오면 PONG으로 즉시 응답 (RTT 측정 및 연결 확인용)
        enqueue_frame(proto::MSG_PONG, 0);
        return;
    }

    if (hh.msg_id == proto::MSG_PONG) {
        return; // PONG은 별도 처리 없음
    }

    // 에러 메시지 처리
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

    // 로그인 응답 처리
    if (hh.msg_id == game_proto::MSG_LOGIN_RES) {
        server::wire::v1::LoginRes pb;
        if (server::wire::codec::Decode(in.data(), in.size(), pb)) {
            if (on_login_) on_login_(pb.effective_user(), pb.session_id());
        } else {
            // Protobuf 디코딩 실패 시 레거시 포맷 파싱 시도 (하위 호환성)
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
            if (on_login_) on_login_(effective, sid);
        }
        return;
    }

    // 채팅 브로드캐스트 처리
    if (hh.msg_id == game_proto::MSG_CHAT_BROADCAST) {
        server::wire::v1::ChatBroadcast pb;
        if (server::wire::codec::Decode(in.data(), in.size(), pb)) {
            if (on_bcast_) {
                on_bcast_(pb.room(), pb.sender(), pb.text(), hh.flags, pb.sender_sid());
            }
        }
        return;
    }

    // 방 사용자 목록 처리
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

    // 상태 스냅샷 처리 (로그인/조인 직후 상태 동기화)
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
                messages.push_back({m.sender(), m.text(), m.ts_ms()});
            }
            if (on_snapshot_) {
                on_snapshot_(pb.current_room(), std::move(rooms), std::move(users), std::move(locked), std::move(messages), pb.your_name());
            }
        }
        return;
    }

    // 귓속말 수신 처리
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

    // 귓속말 결과 처리 (성공/실패)
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

    // 서버 HELLO 메시지 처리 (기능 협상 등)
    if (hh.msg_id == proto::MSG_HELLO) {
        std::uint16_t caps = 0;
        if (in.size() >= 12) {
            caps = proto::read_be16(in.data() + 4);
        }
        if (on_hello_) on_hello_(caps);
        return;
    }

    // 상태 갱신 알림 처리
    if (hh.msg_id == game_proto::MSG_REFRESH_NOTIFY) {
        if (on_refresh_notify_) on_refresh_notify_();
        return;
    }
}

// -----------------------------------------------------------------------------
// 연결 끊김 처리
// -----------------------------------------------------------------------------
void NetClient::handle_disconnect(const boost::system::error_code& ec, const char* context) {
    if (!running_.exchange(false)) {
        return;
    }
    connected_.store(false);

    // 모든 I/O 자원을 닫고 콜백에 reason을 전달한다.
    boost::system::error_code ignore;
    socket_.cancel(ignore);
    socket_.close(ignore);
    if (ping_timer_) {
        ping_timer_->cancel();
    }
    write_queue_.clear();

    if (on_disconnected_) {
        std::string reason = ec ? ec.message() : "connection closed";
        if (context) {
            reason = std::string(context) + ": " + reason;
        }
        on_disconnected_(std::move(reason));
    }
}

// -----------------------------------------------------------------------------
// 각종 요청 전송 메서드들
// -----------------------------------------------------------------------------

void NetClient::send_login(const std::string& user, const std::string& token) {
    std::vector<std::uint8_t> p;
    proto::write_lp_utf8(p, user);
    proto::write_lp_utf8(p, token);
    enqueue_frame(game_proto::MSG_LOGIN_REQ, 0, std::move(p));
}

void NetClient::send_join(const std::string& room, const std::string& password) {
    std::vector<std::uint8_t> p;
    proto::write_lp_utf8(p, room);
    if (!password.empty()) proto::write_lp_utf8(p, password);
    enqueue_frame(game_proto::MSG_JOIN_ROOM, 0, std::move(p));
}

void NetClient::send_leave(const std::string& room) {
    std::vector<std::uint8_t> p;
    proto::write_lp_utf8(p, room);
    enqueue_frame(game_proto::MSG_LEAVE_ROOM, 0, std::move(p));
}

void NetClient::send_chat(const std::string& room, const std::string& text) {
    std::vector<std::uint8_t> p;
    proto::write_lp_utf8(p, room);
    proto::write_lp_utf8(p, text);
    enqueue_frame(game_proto::MSG_CHAT_SEND, 0, std::move(p));
}

void NetClient::send_refresh(const std::string& current_room) {
    std::vector<std::uint8_t> p;
    proto::write_lp_utf8(p, current_room);
    enqueue_frame(game_proto::MSG_REFRESH_REQ, 0, std::move(p));
}

void NetClient::send_who(const std::string& room) {
    std::vector<std::uint8_t> p;
    proto::write_lp_utf8(p, room);
    enqueue_frame(game_proto::MSG_ROOM_USERS_REQ, 0, std::move(p));
}

void NetClient::send_rooms(const std::string& current_room) {
    std::vector<std::uint8_t> p;
    proto::write_lp_utf8(p, current_room);
    enqueue_frame(game_proto::MSG_ROOMS_REQ, 0, std::move(p));
}

void NetClient::send_whisper(const std::string& user, const std::string& text) {
    std::vector<std::uint8_t> p;
    proto::write_lp_utf8(p, user);
    proto::write_lp_utf8(p, text);
    enqueue_frame(game_proto::MSG_WHISPER_REQ, 0, std::move(p));
}
