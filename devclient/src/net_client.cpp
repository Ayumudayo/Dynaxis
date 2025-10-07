// UTF-8, 한국어 주석
#include "client/net_client.hpp"
#include "server/core/protocol/opcodes.hpp"
#include "server/core/protocol/protocol_flags.hpp"
#include "server/core/protocol/frame.hpp"
#include "server/wire/codec.hpp"
#include "wire.pb.h"
#include <cstring>
#include <chrono>

using namespace std::chrono;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
namespace proto = server::core::protocol;

NetClient::NetClient() {}
NetClient::~NetClient() { close(); }

void NetClient::send_frame_simple(tcp::socket& sock, std::uint16_t msg_id, std::uint16_t flags, const std::vector<std::uint8_t>& payload, std::uint32_t& tx_seq, std::mutex* send_mu) {
    proto::FrameHeader h{}; h.length = static_cast<std::uint16_t>(payload.size()); h.msg_id = msg_id; h.flags = flags; h.seq = tx_seq++;
    auto now64 = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count(); h.utc_ts_ms32 = static_cast<std::uint32_t>(now64 & 0xFFFFFFFFu);
    std::vector<std::uint8_t> buf; buf.resize(proto::k_header_bytes + payload.size()); proto::encode_header(h, buf.data()); if (!payload.empty()) std::memcpy(buf.data() + proto::k_header_bytes, payload.data(), payload.size());
    if (send_mu) { std::scoped_lock lk(*send_mu); asio::write(sock, asio::buffer(buf)); } else { asio::write(sock, asio::buffer(buf)); }
}

bool NetClient::connect(const std::string& host, unsigned short port) {
    try {
        auto endpoints = resolver_.resolve(host, std::to_string(port));
        tcp::socket newsock(io_);
        asio::connect(newsock, endpoints);
        newsock.set_option(tcp::no_delay(true));
        sock_ = std::move(newsock);
        connected_.store(true); running_.store(true);
        // HELLO 1회 수신
        std::vector<std::uint8_t> hdr(proto::k_header_bytes); asio::read(sock_, asio::buffer(hdr)); proto::FrameHeader hh{}; proto::decode_header(hdr.data(), hh);
        std::vector<std::uint8_t> body(hh.length); if (hh.length) asio::read(sock_, asio::buffer(body));
        if (hh.msg_id == proto::MSG_HELLO && on_hello_) {
            std::uint16_t caps = 0; if (body.size() >= 12) { caps = proto::read_be16(body.data() + 4); } on_hello_(caps);
        }
        start_threads();
        return true;
    } catch (...) { connected_.store(false); running_.store(false); return false; }
}

void NetClient::start_threads() {
    rx_thread_ = std::thread([this]{ recv_loop(); });
    ping_thread_ = std::thread([this]{ try { while (running_.load()) { std::this_thread::sleep_for(std::chrono::seconds(8)); if (!running_.load()) break; std::vector<std::uint8_t> empty; send_frame_simple(sock_, proto::MSG_PING, 0, empty, seq_, &send_mu_); } } catch (...) {} });
}

void NetClient::close() {
    running_.store(false); try { sock_.close(); } catch (...) {}
    if (rx_thread_.joinable()) rx_thread_.join(); if (ping_thread_.joinable()) ping_thread_.join(); connected_.store(false);
}

void NetClient::recv_loop() {
    try {
        while (running_.load()) {
            std::vector<std::uint8_t> hdr(proto::k_header_bytes); asio::read(sock_, asio::buffer(hdr)); proto::FrameHeader hh{}; proto::decode_header(hdr.data(), hh);
            std::vector<std::uint8_t> body(hh.length); if (hh.length) asio::read(sock_, asio::buffer(body));
            std::span<const std::uint8_t> in(body.data(), body.size());
            if (hh.msg_id == proto::MSG_PING) {
                std::vector<std::uint8_t> empty; send_frame_simple(sock_, proto::MSG_PONG, 0, empty, seq_, &send_mu_);
            } else if (hh.msg_id == proto::MSG_PONG) {
            } else if (hh.msg_id == proto::MSG_ERR) {
                std::uint16_t code=0,len=0; std::string msg; if (in.size()>=4){ code=proto::read_be16(in.data()); len=proto::read_be16(in.data()+2); in=in.subspan(4); if (in.size()>=len) msg.assign(reinterpret_cast<const char*>(in.data()), len); }
                if (on_err_) on_err_(code, msg);
            } else if (hh.msg_id == proto::MSG_LOGIN_RES) {
                server::wire::v1::LoginRes pb;
                if (server::wire::codec::Decode(body.data(), body.size(), pb)) {
                    if (on_login_) on_login_(pb.effective_user(), pb.session_id());
                } else {
                    // fallback legacy
                    std::span<const std::uint8_t> in2(body.data(), body.size()); if (in2.size()>=1) in2 = in2.subspan(1); std::string m; proto::read_lp_utf8(in2,m); std::string effective; if (in2.size()>=2){ auto tmp=in2; std::string t; if (proto::read_lp_utf8(tmp,t)){ effective=std::move(t); in2=tmp; } } std::uint32_t sid=0; if (in2.size()>=4) sid=proto::read_be32(in2.data()); if (on_login_) on_login_(effective, sid);
                }
            } else if (hh.msg_id == proto::MSG_CHAT_BROADCAST) {
                server::wire::v1::ChatBroadcast pb;
                if (server::wire::codec::Decode(body.data(), body.size(), pb)) {
                    if (on_bcast_) on_bcast_(pb.room(), pb.sender(), pb.text(), hh.flags, pb.sender_sid());
                } else {
                    // fallback legacy
                    std::string room, sender, text; if (!proto::read_lp_utf8(in, room) || !proto::read_lp_utf8(in, sender) || !proto::read_lp_utf8(in, text)) continue; std::uint32_t sender_sid=0; if (in.size()>=4){ sender_sid=proto::read_be32(in.data()); in=in.subspan(4);} if (on_bcast_) on_bcast_(room, sender, text, hh.flags, sender_sid);
                }
            } else if (hh.msg_id == proto::MSG_ROOM_USERS) {
                server::wire::v1::RoomUsers pb;
                if (server::wire::codec::Decode(body.data(), body.size(), pb)) {
                    std::vector<std::string> list(pb.users().begin(), pb.users().end());
                    if (on_room_users_) on_room_users_(pb.room(), std::move(list));
                } else {
                    std::string room; if (!proto::read_lp_utf8(in, room)) continue; if (in.size()<2) continue; std::uint16_t n=proto::read_be16(in.data()); in=in.subspan(2); std::vector<std::string> list; list.reserve(n); for (std::uint16_t i=0;i<n;++i){ std::string u; if (!proto::read_lp_utf8(in,u)){ list.clear(); break;} list.push_back(std::move(u)); } if (on_room_users_) on_room_users_(room, std::move(list));
                }
            } else if (hh.msg_id == proto::MSG_WHISPER_BROADCAST) {
                server::wire::v1::WhisperNotice pb;
                if (server::wire::codec::Decode(body.data(), body.size(), pb)) {
                    if (on_whisper_) on_whisper_(pb.sender(), pb.recipient(), pb.text(), pb.outgoing());
                }
            } else if (hh.msg_id == proto::MSG_WHISPER_RES) {
                server::wire::v1::WhisperResult pb;
                if (server::wire::codec::Decode(body.data(), body.size(), pb)) {
                    if (on_whisper_result_) on_whisper_result_(pb.ok(), pb.reason());
                }
            } else if (hh.msg_id == proto::MSG_STATE_SNAPSHOT) {
                server::wire::v1::StateSnapshot pb;
                if (server::wire::codec::Decode(body.data(), body.size(), pb)) {
                    std::vector<std::string> rooms; rooms.reserve(pb.rooms_size());
                    std::vector<bool> locks; locks.reserve(pb.rooms_size());
                    for (const auto& ri : pb.rooms()) { rooms.emplace_back(ri.name()); locks.push_back(ri.locked()); }
                    std::vector<std::string> users(pb.users().begin(), pb.users().end());
                    if (on_snapshot_) on_snapshot_(pb.current_room(), std::move(rooms), std::move(users), std::move(locks));
                } else {
                    std::string current; if (!proto::read_lp_utf8(in,current)) continue; if (in.size()<2) continue; std::uint16_t rc=proto::read_be16(in.data()); in=in.subspan(2); std::vector<std::string> rooms; rooms.reserve(rc); for (std::uint16_t i=0;i<rc;++i){ std::string r; if(!proto::read_lp_utf8(in,r)) { rooms.clear(); break; } if (in.size()<2){ rooms.clear(); break;} in=in.subspan(2); rooms.push_back(std::move(r)); }
                    if (in.size()<2) continue; std::uint16_t uc=proto::read_be16(in.data()); in=in.subspan(2); std::vector<std::string> users; users.reserve(uc); for (std::uint16_t i=0;i<uc;++i){ std::string u; if(!proto::read_lp_utf8(in,u)){ users.clear(); break; } users.push_back(std::move(u)); }
                    std::vector<bool> locks(rooms.size(), false);
                    if (on_snapshot_) on_snapshot_(current, std::move(rooms), std::move(users), std::move(locks));
                }
            }
        }
    } catch (...) { connected_.store(false); running_.store(false); }
}

void NetClient::send_login(const std::string& user, const std::string& token) { std::vector<std::uint8_t> p; proto::write_lp_utf8(p,user); proto::write_lp_utf8(p,token); send_frame_simple(sock_, proto::MSG_LOGIN_REQ, 0, p, seq_, &send_mu_); }
void NetClient::send_join(const std::string& room, const std::string& password) { std::vector<std::uint8_t> p; proto::write_lp_utf8(p,room); if (!password.empty()) proto::write_lp_utf8(p,password); send_frame_simple(sock_, proto::MSG_JOIN_ROOM, 0, p, seq_, &send_mu_); }
void NetClient::send_leave(const std::string& room) { std::vector<std::uint8_t> p; proto::write_lp_utf8(p,room); send_frame_simple(sock_, proto::MSG_LEAVE_ROOM, 0, p, seq_, &send_mu_); }
void NetClient::send_chat(const std::string& room, const std::string& text) { std::vector<std::uint8_t> p; proto::write_lp_utf8(p,room); proto::write_lp_utf8(p,text); send_frame_simple(sock_, proto::MSG_CHAT_SEND, 0, p, seq_, &send_mu_); }
void NetClient::send_refresh(const std::string& current_room) { std::vector<std::uint8_t> p; proto::write_lp_utf8(p,current_room); proto::write_lp_utf8(p,std::string("/refresh")); send_frame_simple(sock_, proto::MSG_CHAT_SEND, 0, p, seq_, &send_mu_); }
void NetClient::send_who(const std::string& room) { std::vector<std::uint8_t> p; proto::write_lp_utf8(p,room); proto::write_lp_utf8(p,std::string("/who ")+room); send_frame_simple(sock_, proto::MSG_CHAT_SEND, 0, p, seq_, &send_mu_); }
void NetClient::send_rooms(const std::string& current_room) { std::vector<std::uint8_t> p; proto::write_lp_utf8(p,current_room); proto::write_lp_utf8(p,std::string("/rooms")); send_frame_simple(sock_, proto::MSG_CHAT_SEND, 0, p, seq_, &send_mu_); }
void NetClient::send_whisper(const std::string& user, const std::string& text) { std::vector<std::uint8_t> p; proto::write_lp_utf8(p,user); proto::write_lp_utf8(p,text); send_frame_simple(sock_, proto::MSG_WHISPER_REQ, 0, p, seq_, &send_mu_); }
