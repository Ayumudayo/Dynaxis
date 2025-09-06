#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <cstring>
#include <span>

#include <boost/asio.hpp>
#include <clocale>
#if defined(_WIN32)
#  include <windows.h>
#endif

#include "server/core/protocol/frame.hpp"
#include "server/core/protocol.hpp"

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
namespace proto = server::core::protocol;

static void send_frame_simple(asio::ip::tcp::socket& sock, std::uint16_t msg_id, std::uint16_t flags, const std::vector<std::uint8_t>& payload, std::uint32_t& tx_seq) {
    // 헤더 인코딩
    proto::FrameHeader h{};
    h.length = static_cast<std::uint16_t>(payload.size());
    h.msg_id = msg_id;
    h.flags  = flags;
    h.seq    = tx_seq++;
    auto now64 = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    h.utc_ts_ms32 = static_cast<std::uint32_t>(now64 & 0xFFFFFFFFu);

    std::vector<std::uint8_t> buf;
    buf.resize(proto::k_header_bytes + payload.size());
    proto::encode_header(h, buf.data());
    if (!payload.empty()) std::memcpy(buf.data() + proto::k_header_bytes, payload.data(), payload.size());
    asio::write(sock, asio::buffer(buf));
}

static bool recv_one(asio::ip::tcp::socket& sock) {
    std::vector<std::uint8_t> hdr;
    hdr.resize(proto::k_header_bytes);
    asio::read(sock, asio::buffer(hdr));
    proto::FrameHeader h{};
    proto::decode_header(hdr.data(), h);
    std::vector<std::uint8_t> body;
    body.resize(h.length);
    if (h.length) asio::read(sock, asio::buffer(body));

    std::cout << "<recv> msg_id=0x" << std::hex << h.msg_id << std::dec
              << " len=" << h.length << " seq=" << h.seq
              << " ts_ms32=" << h.utc_ts_ms32 << std::endl;

    if (h.msg_id == proto::MSG_CHAT_BROADCAST) {
        std::span<const std::uint8_t> sp(body.data(), body.size());
        std::string room, sender, text;
        if (!proto::read_lp_utf8(sp, room)) room = "";
        if (!proto::read_lp_utf8(sp, sender)) sender = "";
        if (!proto::read_lp_utf8(sp, text)) text = std::string(reinterpret_cast<const char*>(body.data()), body.size());
        std::cout << "[" << room << "] " << sender << ": " << text << std::endl;
    }
    return true;
}

int main(int argc, char** argv) {
    try {
#if defined(_WIN32)
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        std::setlocale(LC_ALL, ".UTF-8");
#endif
        if (argc < 3) {
            std::cerr << "사용법: dev_chat_cli <host> <port>" << std::endl;
            return 1;
        }
        std::string host = argv[1];
        unsigned short port = static_cast<unsigned short>(std::stoi(argv[2]));

        asio::io_context io;
        tcp::resolver resolver(io);
        auto endpoints = resolver.resolve(host, std::to_string(port));
        tcp::socket sock(io);
        asio::connect(sock, endpoints);
        sock.set_option(tcp::no_delay(true));
        std::cout << "연결됨: " << host << ":" << port << std::endl;

        // 서버의 HELLO 수신
        recv_one(sock);

        std::uint32_t seq = 1;
        // PING 한 번
        {
            std::vector<std::uint8_t> payload; // 빈 페이로드
            send_frame_simple(sock, proto::MSG_PING, 0, payload, seq);
            recv_one(sock);
        }

        std::cout << "메시지를 입력하세요. 빈 줄로 종료." << std::endl;
        std::string line;
        while (true) {
            std::getline(std::cin, line);
            if (!std::cin.good() || line.empty()) break;
            std::vector<std::uint8_t> payload;
            proto::write_lp_utf8(payload, "default"); // room
            proto::write_lp_utf8(payload, line);       // text
            send_frame_simple(sock, proto::MSG_CHAT_SEND, 0, payload, seq);
            // 서버가 즉시 브로드캐스트 에코를 보냄
            recv_one(sock);
        }
        std::cout << "종료" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "오류: " << ex.what() << std::endl;
        return 1;
    }
}
