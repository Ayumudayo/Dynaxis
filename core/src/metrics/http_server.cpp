#include "server/core/metrics/http_server.hpp"
#include "server/core/runtime_metrics.hpp"
#include "server/core/util/log.hpp"
#include <array>
#include <algorithm>
#include <boost/asio/buffers_iterator.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <optional>
#include <iostream>
#include <sstream>
#include <thread>
#include <unordered_set>

/**
 * @brief MetricsHttpServer의 HTTP 파싱/응답 구현입니다.
 *
 * 메트릭/상태 노출 트래픽을 앱 데이터 경로와 분리해,
 * 운영 진단 요청이 핵심 세션 처리 성능을 방해하지 않도록 설계되었습니다.
 * 이 파일은 웹 프레임워크를 들여오지 않고도 `/metrics`, `/healthz`, `/readyz`를
 * 일정한 동작으로 제공하기 위한 최소 HTTP 표면입니다.
 * 단순해 보이지만 크기 제한, timeout, 인증, allowlist를 같이 묶지 않으면
 * 운영용 엔드포인트가 오히려 가장 싼 공격면이 되기 쉽습니다.
 */
namespace server::core::metrics {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
namespace corelog = server::core::log;

constexpr std::size_t kMaxHttpHeaderBytes = 16 * 1024;
constexpr std::size_t kMaxHttpRequestLineBytes = 2048;
constexpr std::size_t kMaxHttpHeaderCount = 64;
constexpr std::size_t kMaxHttpTargetBytes = 2048;
constexpr std::size_t kMaxHttpBodyBytesDefault = 64 * 1024;
constexpr std::size_t kDefaultHeaderTimeoutMs = 5000;
constexpr std::size_t kDefaultBodyTimeoutMs = 5000;

std::size_t read_env_size(const char* key, std::size_t fallback, std::size_t min_value, std::size_t max_value) {
    if (const char* raw = std::getenv(key); raw && *raw) {
        try {
            const auto parsed = std::stoull(raw);
            if (parsed >= min_value && parsed <= max_value) {
                return static_cast<std::size_t>(parsed);
            }
        } catch (...) {
        }
    }
    return fallback;
}

std::string read_env_string(const char* key) {
    if (const char* raw = std::getenv(key); raw && *raw) {
        return std::string(raw);
    }
    return {};
}

std::string trim_ascii(std::string_view value);
std::string to_lower_ascii(std::string_view value);

std::unordered_set<std::string> split_csv_set(std::string_view raw) {
    std::unordered_set<std::string> out;
    std::size_t start = 0;
    while (start <= raw.size()) {
        std::size_t end = raw.find(',', start);
        if (end == std::string_view::npos) {
            end = raw.size();
        }
        const auto token = trim_ascii(raw.substr(start, end - start));
        if (!token.empty()) {
            out.insert(token);
        }
        if (end == raw.size()) {
            break;
        }
        start = end + 1;
    }
    return out;
}

struct HttpHardeningConfig {
    std::size_t max_connections{64};
    std::size_t max_body_bytes{kMaxHttpBodyBytesDefault};
    std::size_t header_timeout_ms{kDefaultHeaderTimeoutMs};
    std::size_t body_timeout_ms{kDefaultBodyTimeoutMs};
    std::string auth_token;
    std::unordered_set<std::string> ip_allowlist;
};

// 관측 포트는 인터넷-facing API가 아니더라도 임의 입력과 비정상 클라이언트를 그대로 받는다.
// 보호 장치를 한 구조체로 모아 읽어야 연결 수, 본문 크기, 인증 정책이 서로 다른 기본값으로 갈라지지 않는다.
HttpHardeningConfig hardening_config() {
    // 하드닝 값을 환경 변수로 읽는 이유는 서비스별 운영 제약이 다르기 때문입니다.
    // 코드를 다시 배포하지 않고도 관리망/사설망/개발 환경에 맞는 제한을 조정할 수 있습니다.
    HttpHardeningConfig c;
    c.max_connections = read_env_size("METRICS_HTTP_MAX_CONNECTIONS", 64, 1, 65535);
    c.max_body_bytes = read_env_size("METRICS_HTTP_MAX_BODY_BYTES", kMaxHttpBodyBytesDefault, 1, 1024 * 1024);
    c.header_timeout_ms = read_env_size("METRICS_HTTP_HEADER_TIMEOUT_MS", kDefaultHeaderTimeoutMs, 1, 600000);
    c.body_timeout_ms = read_env_size("METRICS_HTTP_BODY_TIMEOUT_MS", kDefaultBodyTimeoutMs, 1, 600000);
    c.auth_token = read_env_string("METRICS_HTTP_AUTH_TOKEN");
    c.ip_allowlist = split_csv_set(read_env_string("METRICS_HTTP_ALLOWLIST"));
    return c;
}

std::atomic<std::size_t>& active_connections() {
    static std::atomic<std::size_t> value{0};
    return value;
}

void update_active_connection_metric(std::size_t active) {
    // 현재 연결 수를 별도 런타임 메트릭으로 밀어 두면
    // 메트릭 서버 자체가 병목인지, 앱 본체가 병목인지를 분리해서 볼 수 있습니다.
    server::core::runtime_metrics::set_http_active_connections(active);
}

std::optional<std::size_t> parse_content_length(std::string_view raw) {
    if (raw.empty()) {
        return static_cast<std::size_t>(0);
    }
    try {
        std::size_t pos = 0;
        const auto parsed = std::stoull(std::string(raw), &pos, 10);
        if (pos != raw.size()) {
            return std::nullopt;
        }
        if (parsed > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

bool request_has_valid_token(const MetricsHttpServer::HttpRequest& request, std::string_view expected_token) {
    // 토큰이 비어 있으면 로컬 개발 기본값으로 열어 둔다.
    // 운영에서는 allowlist 또는 bearer/x-metrics-token 중 적어도 하나를 같이 두는 편이 안전하다.
    if (expected_token.empty()) {
        return true;
    }

    if (const auto it = request.headers.find("x-metrics-token"); it != request.headers.end()) {
        if (it->second == expected_token) {
            return true;
        }
    }

    if (const auto it = request.headers.find("authorization"); it != request.headers.end()) {
        const std::string value = trim_ascii(it->second);
        if (value.size() > 7 && to_lower_ascii(value.substr(0, 7)) == "bearer ") {
            return value.substr(7) == expected_token;
        }
    }

    return false;
}

enum class TimedReadResult {
    kOk,
    kTimeout,
    kTooLarge,
    kEof,
    kError,
};

bool streambuf_contains(const asio::streambuf& buffer, std::string_view delimiter) {
    const auto data = buffer.data();
    const auto begin = asio::buffers_begin(data);
    const auto end = asio::buffers_end(data);
    std::string merged(begin, end);
    return merged.find(delimiter) != std::string::npos;
}

TimedReadResult read_headers_with_timeout(tcp::socket& socket,
                                          asio::streambuf& request_buf,
                                          std::size_t timeout_ms) {
    const auto started_at = std::chrono::steady_clock::now();
    constexpr std::size_t kChunkBytes = 1024;

    while (true) {
        // 헤더 경계가 보일 때까지만 조금씩 읽습니다.
        // 한 번에 크게 읽으면 느린 클라이언트가 메모리와 스레드 시간을 불필요하게 점유합니다.
        // 끝 구분자와 총량 상한을 동시에 봐야 느린 전송(slowloris)과 무한 헤더 누적을 함께 방어할 수 있다.
        if (streambuf_contains(request_buf, "\r\n\r\n")) {
            return TimedReadResult::kOk;
        }
        if (request_buf.size() > kMaxHttpHeaderBytes) {
            return TimedReadResult::kTooLarge;
        }

        const auto elapsed_ms = static_cast<std::size_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started_at)
                .count());
        if (elapsed_ms >= timeout_ms) {
            return TimedReadResult::kTimeout;
        }

        boost::system::error_code available_ec;
        const auto available = socket.available(available_ec);
        if (available_ec) {
            return TimedReadResult::kError;
        }
        if (available == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        std::array<char, kChunkBytes> chunk{};
        const auto to_read = std::min<std::size_t>(available, chunk.size());
        boost::system::error_code read_ec;
        const auto read_bytes = socket.read_some(asio::buffer(chunk.data(), to_read), read_ec);
        if (read_ec) {
            if (read_ec == asio::error::eof) {
                return TimedReadResult::kEof;
            }
            return TimedReadResult::kError;
        }
        if (read_bytes == 0) {
            continue;
        }

        auto writable = request_buf.prepare(read_bytes);
        asio::buffer_copy(writable, asio::buffer(chunk.data(), read_bytes));
        request_buf.commit(read_bytes);
    }
}

TimedReadResult read_body_with_timeout(tcp::socket& socket,
                                       std::string& body,
                                       std::size_t remaining,
                                       std::size_t timeout_ms) {
    const auto started_at = std::chrono::steady_clock::now();
    constexpr std::size_t kChunkBytes = 2048;

    while (remaining > 0) {
        // body도 헤더와 같은 이유로 bounded read를 유지합니다.
        // metrics 경로는 큰 업로드를 기대하지 않으므로, 작고 예측 가능한 비용이 더 중요합니다.
        const auto elapsed_ms = static_cast<std::size_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started_at)
                .count());
        if (elapsed_ms >= timeout_ms) {
            return TimedReadResult::kTimeout;
        }

        boost::system::error_code available_ec;
        const auto available = socket.available(available_ec);
        if (available_ec) {
            return TimedReadResult::kError;
        }
        if (available == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        std::array<char, kChunkBytes> chunk{};
        const auto to_read = std::min<std::size_t>(remaining, std::min<std::size_t>(available, chunk.size()));
        boost::system::error_code read_ec;
        const auto read_bytes = socket.read_some(asio::buffer(chunk.data(), to_read), read_ec);
        if (read_ec) {
            if (read_ec == asio::error::eof) {
                return TimedReadResult::kEof;
            }
            return TimedReadResult::kError;
        }
        if (read_bytes == 0) {
            continue;
        }

        body.append(chunk.data(), read_bytes);
        remaining -= read_bytes;
    }

    return TimedReadResult::kOk;
}

void close_socket_gracefully(const std::shared_ptr<tcp::socket>& sock) {
    if (!sock) {
        return;
    }

    // 운영 엔드포인트는 실패 응답 후에도 FD를 깔끔히 회수해야 합니다.
    // 그렇지 않으면 제한을 잘 걸어 두고도 소켓 정리가 늦어 connection cap이 금방 찹니다.
    try {
        sock->shutdown(tcp::socket::shutdown_both);
    } catch (...) {
    }
    try {
        sock->close();
    } catch (...) {
    }
}

std::string trim_ascii(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }

    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

std::string to_lower_ascii(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

enum class BuiltInRoute {
    kNone,
    kMetrics,
    kLogs,
    kHealth,
    kReady,
};

BuiltInRoute classify_builtin_route(std::string_view target) {
    struct RouteEntry {
        std::string_view target;
        BuiltInRoute route;
    };

    static constexpr std::array<RouteEntry, 7> kRouteEntries{{
        {"/", BuiltInRoute::kMetrics},
        {"/metrics", BuiltInRoute::kMetrics},
        {"/logs", BuiltInRoute::kLogs},
        {"/healthz", BuiltInRoute::kHealth},
        {"/health", BuiltInRoute::kHealth},
        {"/readyz", BuiltInRoute::kReady},
        {"/ready", BuiltInRoute::kReady},
    }};

    // 허용 경로를 화이트리스트 방식으로 고정하면,
    // route callback을 붙이더라도 기본 운영 엔드포인트 의미가 흔들리지 않습니다.
    for (const auto& entry : kRouteEntries) {
        if (entry.target == target) {
            return entry.route;
        }
    }

    return BuiltInRoute::kNone;
}

MetricsHttpServer::MetricsHttpServer(unsigned short port,
                                     MetricsCallback metrics_callback,
                                     StatusCallback health_callback,
                                     StatusCallback ready_callback,
                                     LogsCallback logs_callback,
                                     StatusBodyCallback health_body_callback,
                                     StatusBodyCallback ready_body_callback,
                                     RouteCallback route_callback)
    : port_(port)
    , callback_(std::move(metrics_callback))
    , health_callback_(std::move(health_callback))
    , ready_callback_(std::move(ready_callback))
    , logs_callback_(std::move(logs_callback))
    , health_body_callback_(std::move(health_body_callback))
    , ready_body_callback_(std::move(ready_body_callback))
    , route_callback_(std::move(route_callback)) {
    io_context_ = std::make_shared<asio::io_context>();
    acceptor_ = std::make_shared<tcp::acceptor>(*io_context_, tcp::endpoint(tcp::v4(), port_));
}

MetricsHttpServer::~MetricsHttpServer() {
    stop();
}

void MetricsHttpServer::start() {
    corelog::info(std::string("MetricsHttpServer listening on :") + std::to_string(port_));
    do_accept();
    thread_ = std::make_unique<std::thread>([this]() {
        try {
            // 전용 스레드에서만 관측 HTTP를 돌려 앱 메인 IO와 경합을 최소화합니다.
            io_context_->run();
        } catch (const std::exception& e) {
            corelog::error(std::string("MetricsHttpServer exception: ") + e.what());
        }
    });
}

void MetricsHttpServer::stop() {
    if (stopped_.exchange(true)) {
        return;
    }
    // stop은 idempotent해야 종료 훅 체인에서 여러 번 호출돼도 안전합니다.
    if (io_context_) {
        io_context_->stop();
    }
    if (thread_ && thread_->joinable()) {
        thread_->join();
    }
}

void MetricsHttpServer::do_accept() {
    auto sock = std::make_shared<tcp::socket>(*io_context_);
    acceptor_->async_accept(*sock, [this, sock](const boost::system::error_code& ec) {
        if (!ec) {
            const auto& cfg = hardening_config();
            const std::size_t active_now = active_connections().fetch_add(1, std::memory_order_relaxed) + 1;
            update_active_connection_metric(active_now);
            if (active_now > cfg.max_connections) {
                // 관측 경로가 폭주하더라도 무제한 accept로 메모리를 잡아먹지 않게 즉시 거절합니다.
                // 앱 본체 보호가 우선이므로, metrics 요청 일부를 버리는 편이 낫습니다.
                server::core::runtime_metrics::record_http_connection_limit_reject();
                const std::string body = "Service Unavailable\r\n";
                const std::string hdr = "HTTP/1.1 503 Service Unavailable\r\n"
                    "Content-Type: text/plain; charset=utf-8\r\n"
                    "Content-Length: " + std::to_string(body.size()) + "\r\n"
                    "Connection: close\r\n\r\n";
                const std::vector<asio::const_buffer> bufs{asio::buffer(hdr), asio::buffer(body)};
                boost::system::error_code ignore_ec;
                asio::write(*sock, bufs, ignore_ec);
                close_socket_gracefully(sock);
                const std::size_t active_after = active_connections().fetch_sub(1, std::memory_order_relaxed) - 1;
                update_active_connection_metric(active_after);
                if (!stopped_) {
                    do_accept();
                }
                return;
            }

            asio::post(io_context_->get_executor(), [this, sock]() {
                struct ActiveConnectionGuard {
                    ~ActiveConnectionGuard() {
                        const std::size_t active_after = active_connections().fetch_sub(1, std::memory_order_relaxed) - 1;
                        update_active_connection_metric(active_after);
                    }
                } guard;

                try {
                    const auto cfg = hardening_config();
                    asio::streambuf request_buf(kMaxHttpHeaderBytes);
                    const auto header_read_result = read_headers_with_timeout(*sock, request_buf, cfg.header_timeout_ms);
                    if (header_read_result == TimedReadResult::kTimeout) {
                        // 느린 헤더 전송은 가장 흔한 자원 고갈 패턴이라 조용히 정리합니다.
                        server::core::runtime_metrics::record_http_header_timeout();
                        close_socket_gracefully(sock);
                        return;
                    }
                    if (header_read_result == TimedReadResult::kTooLarge) {
                        server::core::runtime_metrics::record_http_header_oversize();
                        const std::string body = "Request Header Fields Too Large\r\n";
                        const std::string hdr = "HTTP/1.1 431 Request Header Fields Too Large\r\n"
                            "Content-Type: text/plain; charset=utf-8\r\n"
                            "Content-Length: " + std::to_string(body.size()) + "\r\n"
                            "Connection: close\r\n\r\n";
                        const std::vector<asio::const_buffer> bufs{asio::buffer(hdr), asio::buffer(body)};
                        asio::write(*sock, bufs);
                        close_socket_gracefully(sock);
                        return;
                    }
                    if (header_read_result != TimedReadResult::kOk) {
                        // 파싱 불가 요청은 상세 정보를 더 읽지 않고 빨리 닫아 비용을 제한합니다.
                        server::core::runtime_metrics::record_http_bad_request();
                        close_socket_gracefully(sock);
                        return;
                    }

                    if (request_buf.size() > kMaxHttpHeaderBytes) {
                        server::core::runtime_metrics::record_http_header_oversize();
                        const std::string body = "Request Header Fields Too Large\r\n";
                        const std::string hdr = "HTTP/1.1 431 Request Header Fields Too Large\r\n"
                            "Content-Type: text/plain; charset=utf-8\r\n"
                            "Content-Length: " + std::to_string(body.size()) + "\r\n"
                            "Connection: close\r\n\r\n";
                        const std::vector<asio::const_buffer> bufs{asio::buffer(hdr), asio::buffer(body)};
                        asio::write(*sock, bufs);
                        close_socket_gracefully(sock);
                        return;
                    }

                    std::istream request_stream(&request_buf);
                    std::string request_line;
                    std::getline(request_stream, request_line);
                    if (!request_line.empty() && request_line.back() == '\r') request_line.pop_back();
                    if (request_line.empty() || request_line.size() > kMaxHttpRequestLineBytes) {
                        server::core::runtime_metrics::record_http_bad_request();
                        const std::string body = "Bad Request\r\n";
                        const std::string hdr = "HTTP/1.1 400 Bad Request\r\n"
                            "Content-Type: text/plain; charset=utf-8\r\n"
                            "Content-Length: " + std::to_string(body.size()) + "\r\n"
                            "Connection: close\r\n\r\n";
                        const std::vector<asio::const_buffer> bufs{asio::buffer(hdr), asio::buffer(body)};
                        asio::write(*sock, bufs);
                        close_socket_gracefully(sock);
                        return;
                    }

                    MetricsHttpServer::HttpRequest request;
                    {
                        std::istringstream line_stream(request_line);
                        std::string version;
                        line_stream >> request.method >> request.target >> version;
                        if (request.method.empty() || request.target.empty() || version.empty()) {
                            server::core::runtime_metrics::record_http_bad_request();
                            const std::string body = "Bad Request\r\n";
                            const std::string hdr = "HTTP/1.1 400 Bad Request\r\n"
                                "Content-Type: text/plain; charset=utf-8\r\n"
                                "Content-Length: " + std::to_string(body.size()) + "\r\n"
                                "Connection: close\r\n\r\n";
                            const std::vector<asio::const_buffer> bufs{asio::buffer(hdr), asio::buffer(body)};
                            asio::write(*sock, bufs);
                            close_socket_gracefully(sock);
                            return;
                        }
                    }
                    if (request.target.empty()) request.target = "/metrics";
                    if (request.target.size() > kMaxHttpTargetBytes) {
                        server::core::runtime_metrics::record_http_bad_request();
                        const std::string body = "URI Too Long\r\n";
                        const std::string hdr = "HTTP/1.1 414 URI Too Long\r\n"
                            "Content-Type: text/plain; charset=utf-8\r\n"
                            "Content-Length: " + std::to_string(body.size()) + "\r\n"
                            "Connection: close\r\n\r\n";
                        const std::vector<asio::const_buffer> bufs{asio::buffer(hdr), asio::buffer(body)};
                        asio::write(*sock, bufs);
                        close_socket_gracefully(sock);
                        return;
                    }

                    std::string header_line;
                    std::size_t header_count = 0;
                    while (std::getline(request_stream, header_line)) {
                        if (!header_line.empty() && header_line.back() == '\r') {
                            header_line.pop_back();
                        }
                        if (header_line.empty()) {
                            break;
                        }

                        ++header_count;
                        if (header_count > kMaxHttpHeaderCount) {
                            server::core::runtime_metrics::record_http_header_oversize();
                            const std::string body = "Request Header Fields Too Large\r\n";
                            const std::string hdr = "HTTP/1.1 431 Request Header Fields Too Large\r\n"
                                "Content-Type: text/plain; charset=utf-8\r\n"
                                "Content-Length: " + std::to_string(body.size()) + "\r\n"
                                "Connection: close\r\n\r\n";
                            const std::vector<asio::const_buffer> bufs{asio::buffer(hdr), asio::buffer(body)};
                            asio::write(*sock, bufs);
                            close_socket_gracefully(sock);
                            return;
                        }

                        const auto colon = header_line.find(':');
                        if (colon == std::string::npos) {
                            continue;
                        }

                        const std::string name = to_lower_ascii(trim_ascii(std::string_view(header_line).substr(0, colon)));
                        const std::string value = trim_ascii(std::string_view(header_line).substr(colon + 1));
                        if (!name.empty()) {
                            // 마지막 값을 채택하는 단순 정책으로 두어 파서 복잡성을 낮춥니다.
                            // metrics 표면은 RFC 완전성보다 예측 가능한 제한 동작이 더 중요합니다.
                            request.headers[name] = value;
                        }
                    }

                    std::size_t content_length = 0;
                    if (const auto it = request.headers.find("content-length"); it != request.headers.end()) {
                        const auto parsed = parse_content_length(trim_ascii(it->second));
                        if (!parsed) {
                            server::core::runtime_metrics::record_http_bad_request();
                            const std::string body = "Bad Request\r\n";
                            const std::string hdr = "HTTP/1.1 400 Bad Request\r\n"
                                "Content-Type: text/plain; charset=utf-8\r\n"
                                "Content-Length: " + std::to_string(body.size()) + "\r\n"
                                "Connection: close\r\n\r\n";
                            const std::vector<asio::const_buffer> bufs{asio::buffer(hdr), asio::buffer(body)};
                            asio::write(*sock, bufs);
                            close_socket_gracefully(sock);
                            return;
                        }
                        content_length = *parsed;
                    }

                    if (content_length > cfg.max_body_bytes) {
                        server::core::runtime_metrics::record_http_body_oversize();
                        const std::string body = "Payload Too Large\r\n";
                        const std::string hdr = "HTTP/1.1 413 Payload Too Large\r\n"
                            "Content-Type: text/plain; charset=utf-8\r\n"
                            "Content-Length: " + std::to_string(body.size()) + "\r\n"
                            "Connection: close\r\n\r\n";
                        const std::vector<asio::const_buffer> bufs{asio::buffer(hdr), asio::buffer(body)};
                        asio::write(*sock, bufs);
                        close_socket_gracefully(sock);
                        return;
                    }

                    if (content_length > 0) {
                        // 남은 body를 정확히 필요한 만큼만 읽어, keepalive를 지원하지 않아도
                        // oversized payload 때문에 추가 메모리를 잡아먹지 않게 합니다.
                        request.body.reserve(content_length);
                        std::string buffered((std::istreambuf_iterator<char>(request_stream)), std::istreambuf_iterator<char>());
                        if (buffered.size() > content_length) {
                            buffered.resize(content_length);
                        }
                        request.body.append(buffered);
                        if (request.body.size() < content_length) {
                            const std::size_t remain = content_length - request.body.size();
                            const auto body_read_result = read_body_with_timeout(*sock, request.body, remain, cfg.body_timeout_ms);
                            if (body_read_result == TimedReadResult::kTimeout) {
                                server::core::runtime_metrics::record_http_body_timeout();
                                close_socket_gracefully(sock);
                                return;
                            }
                            if (body_read_result != TimedReadResult::kOk) {
                                server::core::runtime_metrics::record_http_bad_request();
                                close_socket_gracefully(sock);
                                return;
                            }
                        }
                    }

                    request.source_ip = "unknown";
                    boost::system::error_code endpoint_ec;
                    const auto remote = sock->remote_endpoint(endpoint_ec);
                    if (!endpoint_ec) {
                        const auto ip = remote.address().to_string();
                        if (!ip.empty()) {
                            request.source_ip = ip;
                        }
                    }

                    if (!cfg.ip_allowlist.empty() && cfg.ip_allowlist.count(request.source_ip) == 0) {
                        // allowlist는 제일 싼 차단 조건이라 토큰 검증보다 먼저 적용합니다.
                        server::core::runtime_metrics::record_http_auth_reject();
                        const std::string body = "Forbidden\r\n";
                        const std::string hdr = "HTTP/1.1 403 Forbidden\r\n"
                            "Content-Type: text/plain; charset=utf-8\r\n"
                            "Content-Length: " + std::to_string(body.size()) + "\r\n"
                            "Connection: close\r\n\r\n";
                        const std::vector<asio::const_buffer> bufs{asio::buffer(hdr), asio::buffer(body)};
                        asio::write(*sock, bufs);
                        close_socket_gracefully(sock);
                        return;
                    }

                    if (!request_has_valid_token(request, cfg.auth_token)) {
                        // allowlist 뒤에 토큰을 두면 내부망과 외부망 정책을 조합하기 쉽습니다.
                        server::core::runtime_metrics::record_http_auth_reject();
                        const std::string body = "Unauthorized\r\n";
                        const std::string hdr = "HTTP/1.1 401 Unauthorized\r\n"
                            "Content-Type: text/plain; charset=utf-8\r\n"
                            "WWW-Authenticate: Bearer\r\n"
                            "Content-Length: " + std::to_string(body.size()) + "\r\n"
                            "Connection: close\r\n\r\n";
                        const std::vector<asio::const_buffer> bufs{asio::buffer(hdr), asio::buffer(body)};
                        asio::write(*sock, bufs);
                        close_socket_gracefully(sock);
                        return;
                    }

                    const std::string& target = request.target;
                    const bool is_get = request.method == "GET";
                    const bool is_head = request.method == "HEAD";
                    const BuiltInRoute built_in_route = classify_builtin_route(target);
                    const bool built_in_target = built_in_route != BuiltInRoute::kNone;

                    std::string body;
                    std::string status = "200 OK";
                    std::string content_type = "text/plain; version=0.0.4";
                    std::string extra_headers;

                    if (built_in_target && !is_get && !is_head) {
                        // 기본 운영 엔드포인트는 읽기 전용으로 고정합니다.
                        // 쓰기 메서드를 허용하면 프락시/캐시/보안 규칙 해석이 불필요하게 복잡해집니다.
                        server::core::runtime_metrics::record_http_bad_request();
                        status = "405 Method Not Allowed";
                        content_type = "text/plain; charset=utf-8";
                        body = "Method Not Allowed\r\n";
                        extra_headers = "Allow: GET, HEAD\r\n";
                    } else {
                        switch (built_in_route) {
                        case BuiltInRoute::kLogs:
                            if (logs_callback_) {
                                content_type = "text/plain; charset=utf-8";
                                body = logs_callback_();
                            } else {
                                status = "404 Not Found";
                                content_type = "text/plain; charset=utf-8";
                                body = "Not Found\r\n";
                            }
                            break;
                        case BuiltInRoute::kMetrics:
                            if (callback_) {
                                body = callback_();
                            } else {
                                // 콜백 누락을 500으로 돌리면 설정 실수와 런타임 오류를 구분하기 어려워진다.
                                // 빈 본문을 노출하면 서버는 살아 있지만 메트릭 소스가 비어 있음을 명확히 보여 준다.
                                body = "# No callback registered\n";
                            }
                            break;
                        case BuiltInRoute::kHealth: {
                            const bool ok = health_callback_ ? health_callback_() : true;
                            status = ok ? "200 OK" : "503 Service Unavailable";
                            content_type = "text/plain; charset=utf-8";
                            if (health_body_callback_) {
                                body = health_body_callback_(ok);
                            } else {
                                body = ok ? "ok\n" : "unhealthy\n";
                            }
                            break;
                        }
                        case BuiltInRoute::kReady: {
                            const bool ok = ready_callback_ ? ready_callback_() : true;
                            status = ok ? "200 OK" : "503 Service Unavailable";
                            content_type = "text/plain; charset=utf-8";
                            if (ready_body_callback_) {
                                body = ready_body_callback_(ok);
                            } else {
                                body = ok ? "ready\n" : "not ready\n";
                            }
                            break;
                        }
                        case BuiltInRoute::kNone: {
                            bool handled_custom = false;
                            if (route_callback_) {
                                // custom route는 기본 운영 표면과 충돌하지 않는 경우에만 실행됩니다.
                                if (auto custom = route_callback_(request)) {
                                    handled_custom = true;
                                    status = custom->status.empty() ? "200 OK" : std::move(custom->status);
                                    content_type = custom->content_type.empty()
                                        ? "text/plain; charset=utf-8"
                                        : std::move(custom->content_type);
                                    body = std::move(custom->body);
                                }
                            }
                            if (!handled_custom) {
                                status = "404 Not Found";
                                content_type = "text/plain; charset=utf-8";
                                body = "Not Found\r\n";
                            }
                            break;
                        }
                        }
                    }

                    std::string hdr = "HTTP/1.1 " + status + "\r\nContent-Type: " + content_type +
                        "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extra_headers + "Connection: close\r\n\r\n";
                    std::vector<asio::const_buffer> bufs;
                    bufs.emplace_back(asio::buffer(hdr));
                    if (!body.empty() && !is_head) {
                        // HEAD는 본문 계산은 공유하되 전송만 생략해, GET과 의미 차이가 벌어지지 않게 합니다.
                        bufs.emplace_back(asio::buffer(body));
                    }
                    asio::write(*sock, bufs);
                    close_socket_gracefully(sock);
                } catch (const std::exception& e) {
                    server::core::runtime_metrics::record_exception_recoverable();
                    corelog::error(std::string("MetricsHttpServer request handling exception: ") + e.what());
                    close_socket_gracefully(sock);
                } catch (...) {
                    server::core::runtime_metrics::record_exception_ignored();
                    corelog::error("MetricsHttpServer request handling unknown exception");
                    close_socket_gracefully(sock);
                }
            });
        }
        
        if (!stopped_) {
            do_accept();
        }
    });
}

} // namespace server::core::metrics
