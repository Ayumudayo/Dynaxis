#include "http_client.hpp"

#include <charconv>
#include <utility>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

namespace admin_app {
namespace asio = boost::asio;
namespace http = boost::beast::http;
using tcp = asio::ip::tcp;

std::optional<HttpUrl> parse_http_url(std::string_view raw) {
    if (!raw.starts_with("http://")) { return std::nullopt; }
    for (const unsigned char ch : raw) {
        if (ch <= 0x20 || ch >= 0x7f || ch == '#' || ch == '@' || ch == '\\') {
            return std::nullopt;
        }
    }
    const auto rest = raw.substr(7);
    const auto separator = rest.find_first_of("/?");
    auto authority = rest.substr(0, separator);
    if (authority.empty()) { return std::nullopt; }
    HttpUrl url;
    if (separator != std::string_view::npos) {
        url.target = rest[separator] == '?' ? "/" + std::string(rest.substr(separator))
                                           : std::string(rest.substr(separator));
    }
    std::string_view port;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) { return std::nullopt; }
        url.host = authority.substr(1, close - 1);
        boost::system::error_code ec;
        asio::ip::make_address_v6(url.host, ec);
        if (ec) { return std::nullopt; }
        const auto suffix = authority.substr(close + 1);
        if (!suffix.empty()) {
            if (!suffix.starts_with(':')) { return std::nullopt; }
            port = suffix.substr(1);
            if (port.empty()) { return std::nullopt; }
        }
    } else {
        const auto colon = authority.find(':');
        url.host = authority.substr(0, colon);
        for (const unsigned char ch : url.host) {
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
                  || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_')) {
                return std::nullopt;
            }
        }
        if (colon != std::string_view::npos) {
            port = authority.substr(colon + 1);
            if (port.empty()) { return std::nullopt; }
        }
    }
    if (url.host.empty()) { return std::nullopt; }
    if (!port.empty()) {
        unsigned int value = 0;
        const auto parsed = std::from_chars(port.data(), port.data() + port.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() || value == 0 || value > 65535) {
            return std::nullopt;
        }
        url.port = static_cast<std::uint16_t>(value);
    }
    return url;
}

std::optional<HttpResponse> http_get_response(const HttpUrl& url, HttpProbeOptions options) {
    if (options.timeout.count() <= 0 || options.body_limit == 0) { return std::nullopt; }
    // HttpUrl을 직접 만든 호출자도 request-line/Host header에 control 문자를 넣을 수 없어야 한다.
    const auto authority = url.host.find(':') == std::string::npos ? url.host : "[" + url.host + "]";
    const auto validated = parse_http_url("http://" + authority + ":" + std::to_string(url.port) + url.target);
    if (url.target.empty() || url.target.front() != '/' || !validated
        || validated->host != url.host || validated->port != url.port || validated->target != url.target) {
        return std::nullopt;
    }
    try {
        asio::io_context io;
        tcp::resolver resolver(io);
        tcp::socket socket(io);
        asio::steady_timer deadline(io, options.timeout);
        boost::beast::flat_buffer buffer(32 * 1024);
        http::response_parser<http::string_body> parser;
        parser.body_limit(options.body_limit);
        parser.header_limit(16 * 1024);
        http::request<http::empty_body> request{http::verb::get, url.target, 11};
        request.set(http::field::host, authority + ":" + std::to_string(url.port));
        request.set(http::field::connection, "close");
        request.set(http::field::accept, "text/plain");
        std::optional<HttpResponse> result;
        bool finished = false;
        auto finish = [&] {
            if (finished) { return; }
            finished = true;
            deadline.cancel();
            resolver.cancel();
            boost::system::error_code ignored;
            socket.close(ignored);
        };
        deadline.async_wait([&](boost::system::error_code ec) {
            if (!ec) { finish(); }
        });
        resolver.async_resolve(url.host, std::to_string(url.port),
            [&](boost::system::error_code ec, tcp::resolver::results_type endpoints) {
                if (finished) { return; }
                if (ec) { finish(); return; }
                asio::async_connect(socket, endpoints, [&](boost::system::error_code ec, const tcp::endpoint&) {
                    if (finished) { return; }
                    if (ec) { finish(); return; }
                    http::async_write(socket, request, [&](boost::system::error_code ec, std::size_t) {
                        if (finished) { return; }
                        if (ec) { finish(); return; }
                        http::async_read(socket, buffer, parser, [&](boost::system::error_code ec, std::size_t) {
                            if (finished) { return; }
                            if (!ec) {
                                result = HttpResponse{parser.get().result_int(), std::move(parser.get().body())};
                            }
                            finish();
                        });
                    });
                });
            });
        io.run();
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> http_get_text(const HttpUrl& url, HttpProbeOptions options) {
    auto response = http_get_response(url, options);
    if (!response || response->status_code != 200) { return std::nullopt; }
    return std::move(response->body);
}

} // namespace admin_app
