#include <gtest/gtest.h>

#include "http_client.hpp"

#include <chrono>
#include <string>
#include <thread>

#include <boost/asio.hpp>

namespace {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

// 연결을 닫지 않고 유지해 client가 HTTP framing이나 deadline으로 조회를 끝내는지 검증한다.
class ProbePeer {
public:
    explicit ProbePeer(std::string response)
        : acceptor_(io_, {asio::ip::address_v4::loopback(), 0})
        , port_(acceptor_.local_endpoint().port())
        , socket_(io_)
        , thread_([this, response = std::move(response)] {
            acceptor_.async_accept(socket_, [this, response](boost::system::error_code ec) {
                if (ec) { return; }
                request_.reserve(1024);
                asio::async_read_until(socket_, asio::dynamic_buffer(request_, 16384), "\r\n\r\n",
                    [this, response](boost::system::error_code ec, std::size_t) {
                        if (ec || response.empty()) { return; }
                        response_ = response;
                        asio::async_write(socket_, asio::buffer(response_), [](auto, auto) {});
                    });
            });
            io_.run();
        }) {}

    ~ProbePeer() {
        // 취소된 I/O completion까지 처리한 뒤 buffer와 socket을 파괴한다.
        asio::post(io_, [this] {
            boost::system::error_code ignored;
            acceptor_.close(ignored);
            socket_.close(ignored);
            guard_.reset();
        });
        thread_.join();
    }

    admin_app::HttpUrl url() const {
        return {"127.0.0.1", port_, "/readyz"};
    }

private:
    asio::io_context io_;
    tcp::acceptor acceptor_;
    const std::uint16_t port_;
    tcp::socket socket_;
    std::string request_;
    std::string response_;
    asio::executor_work_guard<asio::io_context::executor_type> guard_{asio::make_work_guard(io_)};
    std::thread thread_;
};

} // namespace

TEST(AdminHttpClientTest, ValidatesAuthorityPortAndControlCharacters) {
    for (const auto* invalid : {
        "https://localhost/", "http:///metrics", "http://host:0/", "http://host:65536/",
        "http://host:80junk/", "http://host:/", "http://user@host/", "http://host/a#frag",
        "http://host/a\r\nInjected: yes", "http://[::1]:bad/", "http://::1/", "http://host\\x/"}) {
        EXPECT_FALSE(admin_app::parse_http_url(invalid)) << invalid;
    }
    const auto url = admin_app::parse_http_url("http://wb_worker:9090/metrics?q=1");
    ASSERT_TRUE(url);
    EXPECT_EQ(url->host, "wb_worker");
    EXPECT_EQ(url->port, 9090);
    EXPECT_EQ(url->target, "/metrics?q=1");
    const auto ipv6 = admin_app::parse_http_url("http://[::1]:8080/readyz");
    ASSERT_TRUE(ipv6);
    EXPECT_EQ(ipv6->host, "::1");
    const auto query = admin_app::parse_http_url("http://localhost?x=1");
    ASSERT_TRUE(query);
    EXPECT_EQ(query->target, "/?x=1");
}

TEST(AdminHttpClientTest, ReadsContentLengthWithoutWaitingForPeerClose) {
    ProbePeer peer("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nready");
    const auto result = admin_app::http_get_text(peer.url(), {std::chrono::seconds(2), 128});
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, "ready");
}

TEST(AdminHttpClientTest, DecodesChunkedBodyAndPreservesNotReadyStatus) {
    ProbePeer peer("HTTP/1.1 503 Service Unavailable\r\nTransfer-Encoding: chunked\r\n\r\n"
                   "3\r\nnot\r\n6\r\n ready\r\n0\r\n\r\n");
    const auto result = admin_app::http_get_response(peer.url(), {std::chrono::seconds(2), 128});
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status_code, 503u);
    EXPECT_EQ(result->body, "not ready");
}

TEST(AdminHttpClientTest, SilentPeerTimesOut) {
    ProbePeer peer("");
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(admin_app::http_get_response(peer.url(), {std::chrono::milliseconds(100), 128}));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, std::chrono::milliseconds(50));
    EXPECT_LT(elapsed, std::chrono::seconds(2));
}

TEST(AdminHttpClientTest, DeclaredOrChunkedOversizedBodyIsRejected) {
    {
        ProbePeer peer("HTTP/1.1 200 OK\r\nContent-Length: 10000000\r\n\r\n");
        EXPECT_FALSE(admin_app::http_get_response(peer.url(), {std::chrono::seconds(2), 8}));
    }
    {
        ProbePeer peer("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                       "a\r\n0123456789\r\n0\r\n\r\n");
        EXPECT_FALSE(admin_app::http_get_response(peer.url(), {std::chrono::seconds(2), 8}));
    }
}

TEST(AdminHttpClientTest, OversizedHeaderAndMalformedStatusAreRejected) {
    {
        ProbePeer peer("HTTP/1.1 200 OK\r\nX-Fill: " + std::string(17000, 'x') + "\r\nContent-Length: 0\r\n\r\n");
        EXPECT_FALSE(admin_app::http_get_response(peer.url()));
    }
    {
        ProbePeer peer("not-http 200 OK\r\nContent-Length: 0\r\n\r\n");
        EXPECT_FALSE(admin_app::http_get_response(peer.url()));
    }
}
