#include "admin/admin_http_server.hpp"

#include "server/core/util/log.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/buffers_iterator.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <optional>
#include <sstream>

namespace dynaxis::admin {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
namespace corelog = server::core::log;

constexpr std::size_t kMaxHttpHeaderBytes = 16 * 1024;
constexpr std::size_t kMaxHttpRequestLineBytes = 2048;
constexpr std::size_t kMaxHttpHeaderCount = 64;
constexpr std::size_t kMaxHttpTargetBytes = 2048;
constexpr std::size_t kMaxHttpBodyBytes = 64 * 1024;
constexpr std::size_t kMaxConcurrentConnections = 64;
constexpr std::size_t kHeaderTimeoutMs = 5000;
constexpr std::size_t kBodyTimeoutMs = 5000;

enum class TimedReadResult {
    kOk,
    kTimeout,
    kTooLarge,
    kEof,
    kError
};

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

bool streambuf_contains(const asio::streambuf& buffer, std::string_view delimiter) {
    const auto data = buffer.data();
    const std::string merged(asio::buffers_begin(data), asio::buffers_end(data));
    return merged.find(delimiter) != std::string::npos;
}

std::optional<std::size_t> parse_content_length(std::string_view raw) {
    if (raw.empty()) {
        return static_cast<std::size_t>(0);
    }
    char* end_ptr = nullptr;
    const unsigned long long parsed = std::strtoull(std::string(raw).c_str(), &end_ptr, 10);
    if (end_ptr == nullptr || *end_ptr != '\0') {
        return std::nullopt;
    }
    if (parsed > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(parsed);
}

TimedReadResult read_headers_with_timeout(tcp::socket& socket, asio::streambuf& request_buf, std::size_t timeout_ms) {
    const auto started_at = std::chrono::steady_clock::now();
    std::array<char, 1024> chunk{};

    while (true) {
        if (streambuf_contains(request_buf, "\r\n\r\n")) {
            return TimedReadResult::kOk;
        }
        if (request_buf.size() > kMaxHttpHeaderBytes) {
            return TimedReadResult::kTooLarge;
        }
        const auto elapsed_ms = static_cast<std::size_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at).count());
        if (elapsed_ms >= timeout_ms) {
            return TimedReadResult::kTimeout;
        }
        boost::system::error_code ec;
        const auto available = socket.available(ec);
        if (ec) {
            return TimedReadResult::kError;
        }
        if (available == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        const auto to_read = std::min<std::size_t>(available, chunk.size());
        const auto n = socket.read_some(asio::buffer(chunk.data(), to_read), ec);
        if (ec) {
            return (ec == asio::error::eof) ? TimedReadResult::kEof : TimedReadResult::kError;
        }
        if (n > 0) {
            const auto out = request_buf.prepare(n);
            asio::buffer_copy(out, asio::buffer(chunk.data(), n));
            request_buf.commit(n);
        }
    }
}

TimedReadResult read_body_with_timeout(tcp::socket& socket, std::string& body, std::size_t remain, std::size_t timeout_ms) {
    const auto started_at = std::chrono::steady_clock::now();
    std::array<char, 1024> chunk{};

    while (remain > 0) {
        const auto elapsed_ms = static_cast<std::size_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at).count());
        if (elapsed_ms >= timeout_ms) {
            return TimedReadResult::kTimeout;
        }
        boost::system::error_code ec;
        const auto available = socket.available(ec);
        if (ec) {
            return TimedReadResult::kError;
        }
        if (available == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        const auto to_read = std::min<std::size_t>(remain, std::min<std::size_t>(available, chunk.size()));
        const auto n = socket.read_some(asio::buffer(chunk.data(), to_read), ec);
        if (ec) {
            return (ec == asio::error::eof) ? TimedReadResult::kEof : TimedReadResult::kError;
        }
        if (n > 0) {
            body.append(chunk.data(), n);
            remain -= n;
        }
    }
    return TimedReadResult::kOk;
}

void close_socket_gracefully(const std::shared_ptr<tcp::socket>& sock) {
    if (!sock) {
        return;
    }
    boost::system::error_code ignore;
    sock->shutdown(tcp::socket::shutdown_both, ignore);
    sock->close(ignore);
}

void send_simple_response(const std::shared_ptr<tcp::socket>& sock, std::string status, std::string body) {
    const std::string hdr = "HTTP/1.1 " + status +
                            "\r\nContent-Type: text/plain; charset=utf-8\r\n"
                            "X-Content-Type-Options: nosniff\r\n"
                            "X-Frame-Options: DENY\r\n"
                            "Content-Length: " +
                            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
    const std::vector<asio::const_buffer> bufs{asio::buffer(hdr), asio::buffer(body)};
    boost::system::error_code ignore;
    asio::write(*sock, bufs, ignore);
    close_socket_gracefully(sock);
}

std::atomic<std::size_t>& active_connections() {
    static std::atomic<std::size_t> value{0};
    return value;
}

} // namespace

AdminHttpServer::AdminHttpServer(unsigned short port, RouteHandler handler)
    : port_(port), bound_port_(port), handler_(std::move(handler)) {}

AdminHttpServer::~AdminHttpServer() {
    stop();
}

void AdminHttpServer::start() {
    if (io_thread_) {
        return;
    }

    try {
        stopped_.store(false);
        io_context_ = std::make_shared<asio::io_context>();
        const tcp::endpoint ep(tcp::v4(), port_);
        acceptor_ = std::make_shared<tcp::acceptor>(*io_context_);
        acceptor_->open(ep.protocol());
        acceptor_->set_option(asio::socket_base::reuse_address(true));
        acceptor_->bind(ep);
        acceptor_->listen();
        bound_port_.store(acceptor_->local_endpoint().port());

        do_accept();
        io_thread_ = std::make_unique<std::thread>([this]() {
            try {
                io_context_->run();
            } catch (const std::exception& ex) {
                corelog::error(std::string("AdminHttpServer run error: ") + ex.what());
            }
        });
    } catch (const std::exception& ex) {
        corelog::error(std::string("AdminHttpServer start error: ") + ex.what());
        stop();
    }
}

void AdminHttpServer::stop() {
    if (stopped_.exchange(true)) {
        return;
    }
    if (acceptor_) {
        boost::system::error_code ec;
        acceptor_->close(ec);
    }
    if (io_context_) {
        io_context_->stop();
    }
    if (io_thread_ && io_thread_->joinable()) {
        io_thread_->join();
    }
    io_thread_.reset();
    acceptor_.reset();
    io_context_.reset();
}

unsigned short AdminHttpServer::bound_port() const noexcept {
    return bound_port_.load();
}

void AdminHttpServer::do_accept() {
    if (!acceptor_ || stopped_.load()) {
        return;
    }

    auto sock = std::make_shared<tcp::socket>(*io_context_);
    acceptor_->async_accept(*sock, [this, sock](const boost::system::error_code& ec) {
        if (!ec) {
            const std::size_t current = active_connections().fetch_add(1, std::memory_order_relaxed) + 1;
            if (current > kMaxConcurrentConnections) {
                active_connections().fetch_sub(1, std::memory_order_relaxed);
                send_simple_response(sock, "503 Service Unavailable", "Too many concurrent admin HTTP connections\r\n");
                if (!stopped_.load()) {
                    do_accept();
                }
                return;
            }

            asio::post(io_context_->get_executor(), [this, sock]() {
                struct ActiveGuard {
                    ~ActiveGuard() { active_connections().fetch_sub(1, std::memory_order_relaxed); }
                } guard;

                try {
                    asio::streambuf req_buf(kMaxHttpHeaderBytes);
                    const auto hdr_res = read_headers_with_timeout(*sock, req_buf, kHeaderTimeoutMs);
                    if (hdr_res == TimedReadResult::kTooLarge) {
                        send_simple_response(sock, "431 Request Header Fields Too Large", "Header Too Large\r\n");
                        return;
                    }
                    if (hdr_res != TimedReadResult::kOk) {
                        close_socket_gracefully(sock);
                        return;
                    }

                    std::istream req_stream(&req_buf);
                    std::string request_line;
                    std::getline(req_stream, request_line);
                    if (!request_line.empty() && request_line.back() == '\r') {
                        request_line.pop_back();
                    }
                    if (request_line.empty() || request_line.size() > kMaxHttpRequestLineBytes) {
                        send_simple_response(sock, "400 Bad Request", "Bad Request\r\n");
                        return;
                    }

                    AdminHttpRequest request;
                    {
                        std::istringstream ls(request_line);
                        std::string version;
                        ls >> request.method >> request.target >> version;
                        if (request.method.empty() || request.target.empty() || version.empty()) {
                            send_simple_response(sock, "400 Bad Request", "Bad Request\r\n");
                            return;
                        }
                    }
                    if (request.target.size() > kMaxHttpTargetBytes) {
                        send_simple_response(sock, "414 URI Too Long", "URI Too Long\r\n");
                        return;
                    }

                    std::string header_line;
                    std::size_t header_count = 0;
                    while (std::getline(req_stream, header_line)) {
                        if (!header_line.empty() && header_line.back() == '\r') {
                            header_line.pop_back();
                        }
                        if (header_line.empty()) {
                            break;
                        }
                        if (++header_count > kMaxHttpHeaderCount) {
                            send_simple_response(sock, "431 Request Header Fields Too Large", "Header Count Limit\r\n");
                            return;
                        }
                        const auto colon = header_line.find(':');
                        if (colon != std::string::npos) {
                            const std::string key = to_lower_ascii(trim_ascii(std::string_view(header_line).substr(0, colon)));
                            const std::string val = trim_ascii(std::string_view(header_line).substr(colon + 1));
                            if (!key.empty()) {
                                request.headers[key] = val;
                            }
                        }
                    }

                    std::size_t content_length = 0;
                    if (const auto it = request.headers.find("content-length"); it != request.headers.end()) {
                        const auto parsed_len = parse_content_length(it->second);
                        if (!parsed_len) {
                            send_simple_response(sock, "400 Bad Request", "Invalid Content-Length\r\n");
                            return;
                        }
                        content_length = *parsed_len;
                    }
                    if (content_length > kMaxHttpBodyBytes) {
                        send_simple_response(sock, "413 Payload Too Large", "Payload Too Large\r\n");
                        return;
                    }
                    if (content_length > 0) {
                        std::string buffered((std::istreambuf_iterator<char>(req_stream)), std::istreambuf_iterator<char>());
                        if (buffered.size() > content_length) {
                            buffered.resize(content_length);
                        }
                        request.body.append(buffered);
                        if (request.body.size() < content_length) {
                            const auto body_res =
                                read_body_with_timeout(*sock, request.body, content_length - request.body.size(), kBodyTimeoutMs);
                            if (body_res != TimedReadResult::kOk) {
                                close_socket_gracefully(sock);
                                return;
                            }
                        }
                    }

                    boost::system::error_code ep_ec;
                    const auto remote = sock->remote_endpoint(ep_ec);
                    request.source_ip = ep_ec ? std::string("unknown") : remote.address().to_string();

                    AdminHttpResponse resp = handler_ ? handler_(request) : AdminHttpResponse{"500 Internal Server Error", "text/plain", "No handler"};
                    const std::string status = resp.status.empty() ? std::string("200 OK") : resp.status;
                    const std::string content_type =
                        resp.content_type.empty() ? std::string("application/octet-stream") : resp.content_type;

                    const std::string hdr =
                        "HTTP/1.1 " + status +
                        "\r\nContent-Type: " + content_type +
                        "\r\nX-Content-Type-Options: nosniff\r\n"
                        "X-Frame-Options: DENY\r\n"
                        "Referrer-Policy: no-referrer\r\n" +
                        resp.extra_headers +
                        "Content-Length: " + std::to_string(resp.body.size()) +
                        "\r\nConnection: close\r\n\r\n";

                    std::vector<asio::const_buffer> bufs{asio::buffer(hdr)};
                    if (!resp.body.empty() && request.method != "HEAD") {
                        bufs.emplace_back(asio::buffer(resp.body));
                    }
                    boost::system::error_code write_ec;
                    asio::write(*sock, bufs, write_ec);
                    close_socket_gracefully(sock);
                } catch (const std::exception& ex) {
                    corelog::error(std::string("AdminHttpServer request error: ") + ex.what());
                    close_socket_gracefully(sock);
                }
            });
        }
        if (!stopped_.load()) {
            do_accept();
        }
    });
}

} // namespace dynaxis::admin
