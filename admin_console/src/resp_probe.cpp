#include "admin/resp_probe.hpp"

#include "admin/postgres_probe.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <vector>

namespace dynaxis::admin {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

struct ParsedRedisEndpoint {
    std::string host;
    unsigned short port{6379};
    std::string password;
};

ParsedRedisEndpoint parse_redis_endpoint(const std::string& uri,
                                         const std::string& fallback_host,
                                         unsigned short fallback_port) {
    ParsedRedisEndpoint out;
    out.host = fallback_host.empty() ? std::string("127.0.0.1") : fallback_host;
    out.port = (fallback_port > 0) ? fallback_port : 6379;

    if (uri.empty()) {
        return out;
    }

    std::string_view rest(uri);
    const std::size_t scheme = rest.find("://");
    if (scheme != std::string_view::npos) {
        rest.remove_prefix(scheme + 3);
    }
    const std::size_t slash = rest.find('/');
    if (slash != std::string_view::npos) {
        rest = rest.substr(0, slash);
    }

    const std::size_t at = rest.rfind('@');
    if (at != std::string_view::npos) {
        const std::string_view auth = rest.substr(0, at);
        rest.remove_prefix(at + 1);
        const std::size_t colon = auth.find(':');
        if (colon != std::string_view::npos) {
            out.password = std::string(auth.substr(colon + 1));
        } else {
            out.password = std::string(auth);
        }
    }

    if (!rest.empty()) {
        if (rest.front() == '[') {
            const std::size_t close_b = rest.find(']');
            if (close_b != std::string_view::npos) {
                out.host = std::string(rest.substr(1, close_b - 1));
                if (close_b + 2 <= rest.size() && rest[close_b + 1] == ':') {
                    const long p = std::strtol(std::string(rest.substr(close_b + 2)).c_str(), nullptr, 10);
                    if (p > 0 && p <= 65535) {
                        out.port = static_cast<unsigned short>(p);
                    }
                }
            }
        } else {
            const std::size_t colon = rest.rfind(':');
            if (colon != std::string_view::npos) {
                out.host = std::string(rest.substr(0, colon));
                const long p = std::strtol(std::string(rest.substr(colon + 1)).c_str(), nullptr, 10);
                if (p > 0 && p <= 65535) {
                    out.port = static_cast<unsigned short>(p);
                }
            } else {
                out.host = std::string(rest);
            }
        }
    }

    if (out.host.empty()) {
        out.host = fallback_host.empty() ? std::string("127.0.0.1") : fallback_host;
    }
    return out;
}

void append_resp_command(std::string& out, const std::vector<std::string_view>& args) {
    out += "*" + std::to_string(args.size()) + "\r\n";
    for (std::string_view arg : args) {
        out += "$" + std::to_string(arg.size()) + "\r\n";
        out.append(arg.data(), arg.size());
        out += "\r\n";
    }
}

std::size_t count_resp_lines(std::string_view buf) {
    std::size_t lines = 0;
    std::size_t pos = 0;
    while (pos < buf.size()) {
        const std::size_t end = buf.find("\r\n", pos);
        if (end == std::string_view::npos) {
            break;
        }
        ++lines;
        pos = end + 2;
    }
    return lines;
}

} // namespace

RedisProbeResult probe_redis_resp(const std::string& redis_uri,
                                  const std::string& fallback_host,
                                  unsigned short fallback_port,
                                  const std::string& dlq_stream_key,
                                  std::uint32_t deadline_ms) {
    RedisProbeResult result;
    const ParsedRedisEndpoint endpoint = parse_redis_endpoint(redis_uri, fallback_host, fallback_port);
    const std::string safe_dlq_key = dlq_stream_key.empty() ? std::string("session_events_dlq") : dlq_stream_key;
    const auto started_at = std::chrono::steady_clock::now();

    try {
        asio::io_context ioc;
        tcp::resolver resolver(ioc);
        tcp::socket socket(ioc);
        asio::steady_timer timer(ioc);

        std::string payload;
        std::size_t expected_replies = 2;
        if (!endpoint.password.empty()) {
            append_resp_command(payload, {"AUTH", endpoint.password});
            expected_replies = 3;
        }
        append_resp_command(payload, {"PING"});
        append_resp_command(payload, {"XLEN", safe_dlq_key});

        bool finished = false;
        bool timed_out = false;
        std::string raw_reply;
        std::array<char, 1024> scratch{};

        auto finish = [&]() {
            if (finished) {
                return;
            }
            finished = true;
            boost::system::error_code ignore_ec;
            timer.cancel();
            resolver.cancel();
            socket.shutdown(tcp::socket::shutdown_both, ignore_ec);
            socket.close(ignore_ec);
        };

        std::function<void()> start_read;
        start_read = [&]() {
            if (finished) {
                return;
            }
            socket.async_read_some(
                asio::buffer(scratch),
                [&](const boost::system::error_code& ec, std::size_t n) {
                    if (finished) {
                        return;
                    }
                    if (n > 0) {
                        if (raw_reply.size() + n > 8192) {
                            result.error = "RESP reply exceeds size cap";
                            finish();
                            return;
                        }
                        raw_reply.append(scratch.data(), n);
                        if (count_resp_lines(raw_reply) >= expected_replies) {
                            finish();
                            return;
                        }
                    }
                    if (ec) {
                        if (ec != asio::error::eof && result.error.empty()) {
                            result.error = sanitize_connection_error("redis read failed: " + ec.message());
                        }
                        finish();
                        return;
                    }
                    start_read();
                });
        };

        timer.expires_after(std::chrono::milliseconds(std::max<std::uint32_t>(deadline_ms, 50)));
        timer.async_wait([&](const boost::system::error_code& ec) {
            if (!ec && !finished) {
                timed_out = true;
                result.error = "redis probe timeout";
                finish();
            }
        });

        resolver.async_resolve(
            endpoint.host,
            std::to_string(endpoint.port),
            [&](const boost::system::error_code& ec, const tcp::resolver::results_type& endpoints) {
                if (finished) {
                    return;
                }
                if (ec) {
                    result.error = sanitize_connection_error("redis resolve failed: " + ec.message());
                    finish();
                    return;
                }
                asio::async_connect(
                    socket,
                    endpoints,
                    [&](const boost::system::error_code& connect_ec, const tcp::endpoint&) {
                        if (finished) {
                            return;
                        }
                        if (connect_ec) {
                            result.error = sanitize_connection_error("redis connect failed: " + connect_ec.message());
                            finish();
                            return;
                        }
                        asio::async_write(
                            socket,
                            asio::buffer(payload),
                            [&](const boost::system::error_code& write_ec, std::size_t) {
                                if (finished) {
                                    return;
                                }
                                if (write_ec) {
                                    result.error = sanitize_connection_error("redis write failed: " + write_ec.message());
                                    finish();
                                    return;
                                }
                                start_read();
                            });
                    });
            });

        ioc.run();

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at);
        result.latency_ms = static_cast<std::uint32_t>(std::max<long long>(0, elapsed.count()));

        if (!result.error.empty()) {
            return result;
        }
        if (timed_out) {
            result.error = "redis probe timeout";
            return result;
        }

        std::vector<std::string_view> lines;
        std::size_t pos = 0;
        while (pos < raw_reply.size()) {
            const std::size_t end = raw_reply.find("\r\n", pos);
            if (end == std::string::npos) {
                break;
            }
            lines.emplace_back(raw_reply.data() + pos, end - pos);
            pos = end + 2;
        }
        if (lines.size() < expected_replies) {
            result.error = "incomplete RESP reply";
            return result;
        }

        const std::size_t ping_idx = endpoint.password.empty() ? 0u : 1u;
        if (!endpoint.password.empty() && lines[0].rfind("+OK", 0) != 0) {
            result.error = "redis AUTH failed";
            return result;
        }
        if (lines[ping_idx].rfind("+PONG", 0) != 0) {
            result.error = "unexpected redis PING reply";
            return result;
        }

        const std::string_view xlen_line = lines[ping_idx + 1];
        if (!xlen_line.empty() && xlen_line.front() == ':') {
            char* end_ptr = nullptr;
            const long long len = std::strtoll(std::string(xlen_line.substr(1)).c_str(), &end_ptr, 10);
            if (end_ptr != nullptr && *end_ptr == '\0' && len >= 0) {
                result.dlq_length = len;
            }
        }
        result.healthy = true;
        return result;
    } catch (const std::exception& ex) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at);
        result.latency_ms = static_cast<std::uint32_t>(std::max<long long>(0, elapsed.count()));
        result.error = sanitize_connection_error(ex.what());
        return result;
    }
}

} // namespace dynaxis::admin
