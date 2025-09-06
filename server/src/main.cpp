#include <iostream>
#include <thread>
#include <vector>
#include <string>
#include <span>
#include <cstdint>
#include <algorithm>

#include <boost/asio.hpp>
#include <clocale>
#if defined(_WIN32)
#  include <windows.h>
#endif

#include "server/core/acceptor.hpp"
#include "server/core/dispatcher.hpp"
#include "server/core/session.hpp"
#include "server/core/protocol.hpp"
// 고정 헤더에 UTC/SEQ가 항상 포함되므로 별도 플래그 불필요
#include "server/core/util/log.hpp"
#include "server/core/options.hpp"
#include "server/core/shared_state.hpp"

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using server::core::Dispatcher;
using server::core::Acceptor;
using server::core::Session;
namespace protocol = server::core::protocol;
namespace corelog = server::core::log;

int main(int argc, char** argv) {
    try {
#if defined(_WIN32)
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        std::setlocale(LC_ALL, ".UTF-8");
#endif
        unsigned short port = 5000;
        if (argc >= 2) {
            port = static_cast<unsigned short>(std::stoi(argv[1]));
        }

        asio::io_context io;
        Dispatcher dispatcher;
        auto options = std::make_shared<server::core::SessionOptions>();
        auto state   = std::make_shared<server::core::SharedState>();

        // 핸들러 등록: PING -> PONG, CHAT_SEND -> CHAT_BROADCAST(자기 자신에게 에코)
        dispatcher.register_handler(protocol::MSG_PING,
            [](Session& s, std::span<const std::uint8_t> payload) {
                std::vector<std::uint8_t> body(payload.begin(), payload.end());
                s.async_send(protocol::MSG_PONG, body, 0);
            });

        dispatcher.register_handler(protocol::MSG_CHAT_SEND,
            [](Session& s, std::span<const std::uint8_t> payload) {
                // 룸 개념 없이 동일 세션에만 브로드캐스트처럼 에코
                std::vector<std::uint8_t> body(payload.begin(), payload.end());
                s.async_send(protocol::MSG_CHAT_BROADCAST, body, 0);
            });

        tcp::endpoint ep(tcp::v4(), port);
        auto acceptor = std::make_shared<Acceptor>(io, ep, dispatcher, options, state);
        acceptor->start();
        corelog::info("server_app 시작: 0.0.0.0:" + std::to_string(port));

        // 워커 스레드 풀
        unsigned int n = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> workers;
        workers.reserve(n);
        for (unsigned int i = 0; i < n; ++i) {
            workers.emplace_back([&io]() { io.run(); });
        }

        // 단순 대기(종료는 프로세스 종료로)
        for (auto& t : workers) t.join();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "server_app 예외: " << ex.what() << std::endl;
        return 1;
    }
}
