#include <iostream>
#include <thread>
#include <vector>
#include <string>
#include <span>
#include <csignal>
#include <cstdlib> // getenv, strtoul
#include <cstring> // strcmp

#include <boost/asio.hpp>
#include <boost/asio/streambuf.hpp>
#include <clocale>
#if defined(_WIN32)
#  include <windows.h>
#endif

#include "server/app/bootstrap.hpp"
#include "server/app/router.hpp"

#include "server/core/net/acceptor.hpp"
#include "server/core/net/dispatcher.hpp"
#include "server/core/net/session.hpp"
#include "server/core/protocol/opcodes.hpp"
#include "server/core/protocol/protocol_flags.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/core/util/log.hpp"
#include "server/core/config/options.hpp"
#include "server/core/state/shared_state.hpp"
#include "server/core/concurrent/job_queue.hpp"
#include "server/core/concurrent/thread_manager.hpp"
#include "server/core/memory/memory_pool.hpp"
#include "server/chat/chat_service.hpp"
// Protobuf (수신 payload ts_ms 파싱용)
#include "wire.pb.h"
// 저장소 DI: Postgres 커넥션 풀 팩토리
#include "server/storage/postgres/connection_pool.hpp"
#include "server/core/storage/connection_pool.hpp"
// 캐시/팬아웃: Redis 클라이언트(스켈레톤)
#include "server/storage/redis/client.hpp"
// .env 로더
#include "server/core/config/dotenv.hpp"

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
namespace core = server::core;
namespace protocol = server::core::protocol;
namespace corelog = server::core::log;

namespace server::app {

// 간단 메트릭 카운터/게이지(HTTP /metrics 노출용)
static std::atomic<std::uint64_t> g_subscribe_total{0};
static std::atomic<std::uint64_t> g_self_echo_drop_total{0};
static std::atomic<long long>     g_subscribe_last_lag_ms{0};

int run_server(int argc, char** argv) {
    try {
#if defined(_WIN32)
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        std::setlocale(LC_ALL, ".UTF-8");
#endif
        // .env가 있으면 이를 우선(override=true), 없으면 OS 환경변수 사용
        if (server::core::config::load_dotenv(".env", true)) {
            corelog::info("Loaded .env (override existing env = true)");
        } else {
            corelog::info(".env not found — using OS environment variables");
        }

        unsigned short port = 5000;
        if (argc >= 2) {
            port = static_cast<unsigned short>(std::stoi(argv[1]));
        }

        asio::io_context io;
        core::JobQueue job_queue;
        core::ThreadManager workers(job_queue);
        core::BufferManager buffer_manager(2048, 1024); // 2KB buffers, 1024 count

        core::Dispatcher dispatcher;
        auto options = std::make_shared<core::SessionOptions>();
        options->read_timeout_ms = 60'000;
        options->heartbeat_interval_ms = 10'000;
        auto state = std::make_shared<core::SharedState>();

        // DB 커넥션 풀 구성(환경 변수 기반)
        std::shared_ptr<core::storage::IConnectionPool> db_pool;
        {
            const char* uri = std::getenv("DB_URI");
            if (uri && *uri) {
                corelog::info(std::string("DB_URI 감지: ") + uri);
                core::storage::PoolOptions popts{};
                if (const char* v = std::getenv("DB_POOL_MIN")) popts.min_size = static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
                if (const char* v = std::getenv("DB_POOL_MAX")) popts.max_size = static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
                if (const char* v = std::getenv("DB_CONN_TIMEOUT_MS")) popts.connect_timeout_ms = static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10));
                if (const char* v = std::getenv("DB_QUERY_TIMEOUT_MS")) popts.query_timeout_ms = static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10));
                if (const char* v = std::getenv("DB_PREPARE_STATEMENTS")) popts.prepare_statements = (std::strcmp(v, "0") != 0);

                db_pool = server::storage::postgres::make_connection_pool(uri, popts);
                if (!db_pool || !db_pool->health_check()) {
                    corelog::error("DB 헬스체크 실패 — DB_URI를 확인하세요.");
                    return 2;
                }
                corelog::info("DB 커넥션 풀 초기화 완료.");
            } else {
                corelog::warn("DB_URI 미설정 — DB 연동 비활성(후속 단계에서 필요)");
            }
        }

        // Redis 클라이언트 구성(환경 변수 기반)
        std::shared_ptr<server::storage::redis::IRedisClient> redis;
        if (const char* ruri = std::getenv("REDIS_URI"); ruri && *ruri) {
            corelog::info(std::string("REDIS_URI 감지: ") + ruri);
            server::storage::redis::Options ropts{};
            if (const char* v = std::getenv("REDIS_POOL_MAX")) ropts.pool_max = static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
            if (const char* v = std::getenv("REDIS_USE_STREAMS")) ropts.use_streams = (std::strcmp(v, "0") != 0);
            redis = server::storage::redis::make_redis_client(ruri, ropts);
            // 최소 복원: PRESENCE_CLEAN_ON_START != 0 이면 presence:room:* 정리(개발/단일 게이트웨이 전제)
            if (redis) {
                if (const char* clean = std::getenv("PRESENCE_CLEAN_ON_START"); clean && std::strcmp(clean, "0") != 0) {
                    std::string prefix;
                    if (const char* p = std::getenv("REDIS_CHANNEL_PREFIX"); p && *p) prefix = p;
                    std::string pattern = prefix + std::string("presence:room:*");
                    corelog::warn(std::string("Presence cleanup on start: ") + pattern);
                    (void)redis->scan_del(pattern);
                }
            }
            if (!redis || !redis->health_check()) {
                corelog::error("Redis 헬스체크 실패 — REDIS_URI를 확인하세요.");
            } else {
                corelog::info("Redis 클라이언트 초기화 완료.");
            }
        } else {
            corelog::warn("REDIS_URI 미설정 — Redis 연동 비활성(후속 단계에서 필요)");
        }

        server::app::chat::ChatService chat(io, job_queue, db_pool, redis);
        // TODO: ChatService에 저장소 주입(후속 단계)

        register_routes(dispatcher, chat);

        tcp::endpoint ep(tcp::v4(), port);
        auto acceptor = std::make_shared<core::Acceptor>(io, ep, dispatcher, buffer_manager, options, state,
            [&chat](std::shared_ptr<core::Session> sess){
                sess->set_on_close([&chat](std::shared_ptr<core::Session> s){ chat.on_session_close(s); });
            });
        acceptor->start();
        corelog::info("server_app 시작: 0.0.0.0:" + std::to_string(port));

        // 워커 스레드 풀 시작
        unsigned int num_worker_threads = std::max(1u, std::thread::hardware_concurrency());
        workers.Start(num_worker_threads);
        corelog::info(std::to_string(num_worker_threads) + "개의 워커 스레드를 시작합니다.");

        // I/O 스레드 풀 시작
        unsigned int num_io_threads = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> io_threads;
        io_threads.reserve(num_io_threads);
        for (unsigned int i = 0; i < num_io_threads; ++i) {
            io_threads.emplace_back([&io]() { 
                try {
                    io.run();
                } catch (const std::exception& e) {
                    corelog::error(std::string("I/O 스레드 예외: ") + e.what());
                }
            });
        }
        corelog::info(std::to_string(num_io_threads) + "개의 I/O 스레드를 시작합니다.");

        // 정상 종료(Ctrl+C) 처리
        // Pub/Sub 분산 브로드캐스트 구독(옵션)
        if (redis) {
            if (const char* use = std::getenv("USE_REDIS_PUBSUB"); use && std::strcmp(use, "0") != 0) {
                std::string prefix; if (const char* p = std::getenv("REDIS_CHANNEL_PREFIX")) if (*p) prefix = p;
                std::string pattern = prefix + std::string("fanout:room:*");
                std::string gwid = [](){ const char* g = std::getenv("GATEWAY_ID"); if (g && *g) return std::string(g); return std::string("gw-default"); }();
                redis->start_psubscribe(pattern, [&chat, gwid](const std::string& channel, const std::string& message){
                    if (message.rfind("gw=", 0) == 0) {
                        auto nl = message.find('\n'); if (nl == std::string::npos) return;
                        std::string from = message.substr(3, nl - 3);
                        if (from == gwid) { // self-echo 차단
                            auto d = ++g_self_echo_drop_total;
                            corelog::info(std::string("metric=self_echo_drop_total value=") + std::to_string(d));
                            return;
                        }
                        std::string payload = message.substr(nl + 1);
                        std::string room = channel; auto pos = room.rfind(':'); if (pos != std::string::npos) room = room.substr(pos + 1);
                        std::vector<std::uint8_t> body(payload.begin(), payload.end());
                        // subscribe lag 측정(ts_ms가 있으면 now - ts_ms)
                        try {
                            server::wire::v1::ChatBroadcast pb;
                            if (pb.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
                                auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch()).count();
                                if (pb.ts_ms() > 0 && now_ms >= static_cast<long long>(pb.ts_ms())) {
                                    auto lag = static_cast<long long>(now_ms - static_cast<long long>(pb.ts_ms()));
                                    g_subscribe_last_lag_ms.store(lag, std::memory_order_relaxed);
                                    corelog::info(std::string("metric=subscribe_lag_ms value=") + std::to_string(lag) + " room=" + room);
                                }
                            }
                        } catch (...) {}
                        chat.broadcast_room(room, body, nullptr);
                        auto n = ++g_subscribe_total;
                        corelog::info(std::string("metric=subscribe_total value=") + std::to_string(n) + " room=" + room);
                    }
                });
                corelog::info(std::string("Subscribed Redis pattern: ") + pattern);
            }
        }

        // Metrics HTTP 엔드포인트(METRICS_PORT 설정 시 활성화)
        std::unique_ptr<std::thread> metrics_thread;
        std::shared_ptr<asio::io_context> metrics_io;
        std::shared_ptr<tcp::acceptor> metrics_acc;
        unsigned short metrics_port = 0;
        if (const char* mp = std::getenv("METRICS_PORT"); mp && *mp) {
            unsigned long v = std::strtoul(mp, nullptr, 10); if (v > 0 && v < 65536) metrics_port = static_cast<unsigned short>(v);
        }
        if (metrics_port != 0) {
            metrics_io = std::make_shared<asio::io_context>();
            metrics_acc = std::make_shared<tcp::acceptor>(*metrics_io, tcp::endpoint(tcp::v4(), metrics_port));
            auto do_accept = std::make_shared<std::function<void()>>();
            *do_accept = [metrics_io, metrics_acc, do_accept]() {
                auto sock = std::make_shared<tcp::socket>(*metrics_io);
                metrics_acc->async_accept(*sock, [metrics_io, metrics_acc, sock, do_accept](const boost::system::error_code& ec){
                    if (!ec) {
                        asio::post(metrics_io->get_executor(), [sock]() {
                            try {
                                asio::streambuf request_buf;
                                boost::system::error_code read_ec;
                                asio::read_until(*sock, request_buf, "\r\n\r\n", read_ec);
                                if (read_ec && read_ec != asio::error::eof) {
                                    boost::system::error_code ec;
                                    sock->close(ec);
                                    return;
                                }

                                std::string body;
                                body.reserve(256);
                                body += "# TYPE chat_subscribe_total counter\nchat_subscribe_total ";
                                body += std::to_string(g_subscribe_total.load());
                                body += "\n# TYPE chat_self_echo_drop_total counter\nchat_self_echo_drop_total ";
                                body += std::to_string(g_self_echo_drop_total.load());
                                body += "\n# TYPE chat_subscribe_last_lag_ms gauge\nchat_subscribe_last_lag_ms ";
                                body += std::to_string(g_subscribe_last_lag_ms.load());
                                body += "\n";
                                std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: text/plain; version=0.0.4\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                                std::vector<asio::const_buffer> bufs;
                                bufs.emplace_back(asio::buffer(hdr));
                                bufs.emplace_back(asio::buffer(body));
                                asio::write(*sock, bufs);
                                boost::system::error_code ec;
                                sock->shutdown(tcp::socket::shutdown_both, ec);
                                if (ec) ec.clear();
                                sock->close(ec);
                            } catch (...) {}
                        });
                    }
                    // 다음 accept
                    asio::post(metrics_io->get_executor(), *do_accept);
                });
            };
            (*do_accept)();
            metrics_thread = std::make_unique<std::thread>([metrics_io](){
                try { metrics_io->run(); } catch (...) {}
            });
            corelog::info(std::string("Metrics listening on :") + std::to_string(metrics_port));
        }
        asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&](const boost::system::error_code&, int) {
            corelog::info("서버 종료 신호 수신...");
            // Redis Pub/Sub 구독 중지(있다면)
            try { if (redis) { redis->stop_psubscribe(); } } catch (...) {}
            if (metrics_io) { try { metrics_io->stop(); } catch (...) {} }
            acceptor->stop();
            io.stop();
            workers.Stop();
        });

        for (auto& t : io_threads) {
            t.join();
        }

        if (metrics_thread && metrics_thread->joinable()) metrics_thread->join();
        return 0;
    } catch (const std::exception& ex) {
        corelog::error(std::string("server_app 예외: ") + ex.what());
        return 1;
    }
}

} // namespace server::app

