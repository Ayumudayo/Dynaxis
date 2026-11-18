#include "admin/bounded_http_probe.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <sstream>

namespace dynaxis::admin {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

std::string to_lower_copy(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (unsigned char c : sv) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

std::string trim_copy(std::string_view sv) {
    std::size_t a = 0;
    while (a < sv.size() && std::isspace(static_cast<unsigned char>(sv[a]))) {
        ++a;
    }
    std::size_t b = sv.size();
    while (b > a && std::isspace(static_cast<unsigned char>(sv[b - 1]))) {
        --b;
    }
    return std::string(sv.substr(a, b - a));
}

std::string decode_chunked_bounded(std::string_view raw, std::size_t max_bytes, bool& ok) {
    ok = true;
    std::string decoded;
    std::size_t pos = 0;
    while (pos < raw.size()) {
        const std::size_t line_end = raw.find("\r\n", pos);
        if (line_end == std::string_view::npos) {
            ok = false;
            return decoded;
        }
        std::string size_tok = trim_copy(raw.substr(pos, line_end - pos));
        const std::size_t semi = size_tok.find(';');
        if (semi != std::string::npos) {
            size_tok.resize(semi);
        }
        char* end_ptr = nullptr;
        const unsigned long chunk_sz = std::strtoul(size_tok.c_str(), &end_ptr, 16);
        if (end_ptr == size_tok.c_str() || *end_ptr != '\0') {
            ok = false;
            return decoded;
        }
        pos = line_end + 2;
        if (chunk_sz == 0) {
            return decoded;
        }
        if (pos + chunk_sz > raw.size()) {
            ok = false;
            return decoded;
        }
        if (decoded.size() + chunk_sz > max_bytes) {
            ok = false;
            return decoded;
        }
        decoded.append(raw.substr(pos, chunk_sz));
        pos += chunk_sz;
        if (pos + 2 > raw.size() || raw.substr(pos, 2) != "\r\n") {
            ok = false;
            return decoded;
        }
        pos += 2;
    }
    ok = false;
    return decoded;
}

} // namespace

BoundedHttpProbeResult bounded_http_get(const std::string& host,
                                        unsigned short port,
                                        const std::string& target,
                                        std::uint32_t deadline_ms,
                                        std::size_t max_header_bytes,
                                        std::size_t max_body_bytes) {
    BoundedHttpProbeResult result;
    if (host.empty() || port == 0) {
        result.error = "invalid target";
        return result;
    }

    const std::string safe_target = target.empty() ? std::string("/") : target;
    const auto started_at = std::chrono::steady_clock::now();

    try {
        asio::io_context ioc;
        tcp::resolver resolver(ioc);
        tcp::socket socket(ioc);
        asio::steady_timer timer(ioc);

        bool timed_out = false;
        bool finished = false;
        boost::system::error_code io_ec;
        std::string raw_response;
        std::size_t header_end_pos = std::string::npos;
        std::optional<std::size_t> content_length;
        bool is_chunked = false;
        std::array<char, 4096> scratch{};

        const std::size_t total_cap = max_header_bytes + max_body_bytes + 4;
        const std::string request =
            "GET " + safe_target + " HTTP/1.1\r\n"
            "Host: " + host + ":" + std::to_string(port) + "\r\n"
            "Connection: close\r\n"
            "User-Agent: dynaxis-admin-probe/1.0\r\n\r\n";

        auto finish = [&](const boost::system::error_code& ec) {
            if (finished) {
                return;
            }
            finished = true;
            io_ec = ec;
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
                [&](const boost::system::error_code& ec, std::size_t bytes_transferred) {
                    if (finished) {
                        return;
                    }
                    if (bytes_transferred > 0) {
                        if (raw_response.size() + bytes_transferred > total_cap) {
                            result.error = "response exceeds size cap";
                            finish(ec);
                            return;
                        }
                        raw_response.append(scratch.data(), bytes_transferred);

                        if (header_end_pos == std::string::npos) {
                            const std::size_t hdr_end = raw_response.find("\r\n\r\n");
                            if (hdr_end == std::string::npos) {
                                if (raw_response.size() > max_header_bytes) {
                                    result.error = "response headers exceed cap";
                                    finish({});
                                    return;
                                }
                            } else {
                                if (hdr_end > max_header_bytes) {
                                    result.error = "response headers exceed cap";
                                    finish({});
                                    return;
                                }
                                header_end_pos = hdr_end + 4;
                                const std::string_view hdr_view(raw_response.data(), hdr_end);
                                std::size_t line_pos = hdr_view.find("\r\n");
                                if (line_pos != std::string_view::npos) {
                                    line_pos += 2;
                                    while (line_pos < hdr_view.size()) {
                                        const std::size_t next_line = hdr_view.find("\r\n", line_pos);
                                        const std::string_view line =
                                            (next_line == std::string_view::npos)
                                                ? hdr_view.substr(line_pos)
                                                : hdr_view.substr(line_pos, next_line - line_pos);
                                        const std::size_t colon = line.find(':');
                                        if (colon != std::string_view::npos) {
                                            const std::string key = to_lower_copy(trim_copy(line.substr(0, colon)));
                                            const std::string val = trim_copy(line.substr(colon + 1));
                                            if (key == "content-length") {
                                                char* end_ptr = nullptr;
                                                const unsigned long long len = std::strtoull(val.c_str(), &end_ptr, 10);
                                                if (end_ptr != val.c_str() && *end_ptr == '\0') {
                                                    content_length = static_cast<std::size_t>(len);
                                                    if (*content_length > max_body_bytes) {
                                                        result.error = "response body exceeds cap";
                                                        finish({});
                                                        return;
                                                    }
                                                }
                                            } else if (key == "transfer-encoding" &&
                                                       to_lower_copy(val).find("chunked") != std::string::npos) {
                                                is_chunked = true;
                                            }
                                        }
                                        if (next_line == std::string_view::npos) {
                                            break;
                                        }
                                        line_pos = next_line + 2;
                                    }
                                }
                            }
                        }

                        if (header_end_pos != std::string::npos) {
                            const std::size_t body_bytes = raw_response.size() - header_end_pos;
                            if (body_bytes > max_body_bytes) {
                                result.error = "response body exceeds cap";
                                finish({});
                                return;
                            }
                            if (content_length.has_value() && body_bytes >= *content_length) {
                                finish({});
                                return;
                            }
                            if (is_chunked &&
                                raw_response.find("0\r\n\r\n", header_end_pos) != std::string::npos) {
                                finish({});
                                return;
                            }
                        }
                    }

                    if (ec == asio::error::eof) {
                        finish({});
                        return;
                    }
                    if (ec) {
                        finish(ec);
                        return;
                    }
                    start_read();
                });
        };

        timer.expires_after(std::chrono::milliseconds(std::max<std::uint32_t>(deadline_ms, 50)));
        timer.async_wait([&](const boost::system::error_code& ec) {
            if (!ec && !finished) {
                timed_out = true;
                result.error = "probe timeout";
                finish(asio::error::timed_out);
            }
        });

        resolver.async_resolve(
            host,
            std::to_string(port),
            [&](const boost::system::error_code& ec, const tcp::resolver::results_type& endpoints) {
                if (finished) {
                    return;
                }
                if (ec) {
                    result.error = "resolve failed: " + ec.message();
                    finish(ec);
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
                            result.error = "connect failed: " + connect_ec.message();
                            finish(connect_ec);
                            return;
                        }
                        asio::async_write(
                            socket,
                            asio::buffer(request),
                            [&](const boost::system::error_code& write_ec, std::size_t) {
                                if (finished) {
                                    return;
                                }
                                if (write_ec) {
                                    result.error = "write failed: " + write_ec.message();
                                    finish(write_ec);
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
            result.error = "probe timeout";
            return result;
        }
        if (io_ec && io_ec != asio::error::eof) {
            result.error = "read failed: " + io_ec.message();
            return result;
        }
        if (header_end_pos == std::string::npos) {
            result.error = "incomplete HTTP response";
            return result;
        }

        const std::string_view hdr_view(raw_response.data(), header_end_pos - 4);
        const std::size_t status_line_end = hdr_view.find("\r\n");
        const std::string_view status_line =
            (status_line_end == std::string_view::npos) ? hdr_view : hdr_view.substr(0, status_line_end);
        std::istringstream ss{std::string(status_line)};
        std::string http_ver;
        int code = 0;
        ss >> http_ver >> code;
        if (http_ver.rfind("HTTP/", 0) != 0 || code < 100 || code > 599) {
            result.error = "malformed HTTP status line";
            return result;
        }

        std::string_view body_view(raw_response.data() + header_end_pos, raw_response.size() - header_end_pos);
        if (content_length.has_value()) {
            if (body_view.size() < *content_length) {
                result.error = "truncated HTTP body";
                return result;
            }
            result.body.assign(body_view.data(), *content_length);
        } else if (is_chunked) {
            bool chunk_ok = false;
            result.body = decode_chunked_bounded(body_view, max_body_bytes, chunk_ok);
            if (!chunk_ok) {
                result.error = "invalid chunked HTTP body";
                return result;
            }
        } else {
            result.body.assign(body_view.data(), body_view.size());
        }

        result.status_code = code;
        result.ok = (code >= 200 && code < 400);
        return result;
    } catch (const std::exception&) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at);
        result.latency_ms = static_cast<std::uint32_t>(std::max<long long>(0, elapsed.count()));
        result.error = "http probe exception";
        return result;
    }
}

} // namespace dynaxis::admin
