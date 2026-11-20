#include "admin/admin_router.hpp"
#include "admin/admin_types.hpp"
#include "admin/bounded_http_probe.hpp"
#include "admin/inventory_poller.hpp"
#include "admin/postgres_probe.hpp"
#include "admin/prometheus_parser.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using namespace dynaxis::admin;

class ScopedHttpMockServer {
public:
    struct HttpReply {
        std::string status{"200 OK"};
        std::string body;
    };
    using Handler = std::function<HttpReply(std::string_view target)>;

    explicit ScopedHttpMockServer(Handler handler)
        : handler_(std::move(handler)),
          acceptor_(ioc_, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)),
          port_(acceptor_.local_endpoint().port()) {
        do_accept();
        worker_ = std::thread([this]() { ioc_.run(); });
    }

    ~ScopedHttpMockServer() {
        stop();
    }

    unsigned short port() const noexcept {
        return port_;
    }

    void stop() {
        if (stopped_.exchange(true)) {
            return;
        }
        asio::post(ioc_, [this]() {
            boost::system::error_code ec;
            acceptor_.cancel(ec);
            acceptor_.close(ec);
        });
        if (worker_.joinable()) {
            worker_.join();
        }
        boost::system::error_code ec;
        acceptor_.close(ec);
    }

private:
    void do_accept() {
        if (stopped_.load()) {
            return;
        }
        auto socket = std::make_shared<tcp::socket>(ioc_);
        acceptor_.async_accept(*socket, [this, socket](const boost::system::error_code& ec) {
            if (!ec && !stopped_.load()) {
                handle_client(*socket);
            }
            if (!stopped_.load()) {
                do_accept();
            }
        });
    }

    void handle_client(tcp::socket& socket) {
        boost::system::error_code ec;
        try {
            asio::streambuf buf;
            asio::read_until(socket, buf, "\r\n\r\n");
            std::istream req_stream(&buf);
            std::string method;
            std::string target;
            req_stream >> method >> target;

            const HttpReply reply = handler_(target);
            std::ostringstream oss;
            oss << "HTTP/1.1 " << reply.status << "\r\n"
                << "Content-Type: text/plain; charset=utf-8\r\n"
                << "Content-Length: " << reply.body.size() << "\r\n"
                << "Connection: close\r\n\r\n"
                << reply.body;
            const std::string raw = oss.str();
            asio::write(socket, asio::buffer(raw), ec);
            socket.shutdown(tcp::socket::shutdown_both, ec);
            socket.close(ec);
        } catch (...) {
            boost::system::error_code close_ec;
            socket.close(close_ec);
        }
    }

    Handler handler_;
    asio::io_context ioc_;
    tcp::acceptor acceptor_;
    unsigned short port_{0};
    std::atomic<bool> stopped_{false};
    std::thread worker_;
};

class ScopedHungTcpServer {
public:
    ScopedHungTcpServer()
        : acceptor_(ioc_, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)),
          port_(acceptor_.local_endpoint().port()) {
        do_accept();
        worker_ = std::thread([this]() { ioc_.run(); });
    }

    ~ScopedHungTcpServer() {
        if (stopped_.exchange(true)) {
            return;
        }
        asio::post(ioc_, [this]() {
            boost::system::error_code ec;
            acceptor_.cancel(ec);
            acceptor_.close(ec);
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& sock : held_sockets_) {
                if (sock) {
                    sock->close(ec);
                }
            }
            held_sockets_.clear();
        });
        if (worker_.joinable()) {
            worker_.join();
        }
        boost::system::error_code ec;
        acceptor_.close(ec);
    }

    unsigned short port() const noexcept {
        return port_;
    }

private:
    void do_accept() {
        if (stopped_.load()) {
            return;
        }
        auto sock = std::make_shared<tcp::socket>(ioc_);
        acceptor_.async_accept(*sock, [this, sock](const boost::system::error_code& ec) {
            if (!ec && !stopped_.load()) {
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    held_sockets_.push_back(sock);
                }
                do_accept();
            }
        });
    }

    asio::io_context ioc_;
    tcp::acceptor acceptor_;
    unsigned short port_{0};
    std::atomic<bool> stopped_{false};
    std::mutex mu_;
    std::vector<std::shared_ptr<tcp::socket>> held_sockets_;
    std::thread worker_;
};

class ScopedRespMockServer {
public:
    explicit ScopedRespMockServer(std::string response_payload)
        : response_(std::move(response_payload)),
          acceptor_(ioc_, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)),
          port_(acceptor_.local_endpoint().port()) {
        do_accept();
        worker_ = std::thread([this]() { ioc_.run(); });
    }

    ~ScopedRespMockServer() {
        if (stopped_.exchange(true)) {
            return;
        }
        asio::post(ioc_, [this]() {
            boost::system::error_code ec;
            acceptor_.cancel(ec);
            acceptor_.close(ec);
        });
        if (worker_.joinable()) {
            worker_.join();
        }
        boost::system::error_code ec;
        acceptor_.close(ec);
    }

    unsigned short port() const noexcept {
        return port_;
    }

private:
    void do_accept() {
        if (stopped_.load()) {
            return;
        }
        auto socket = std::make_shared<tcp::socket>(ioc_);
        acceptor_.async_accept(*socket, [this, socket](const boost::system::error_code& ec) {
            if (!ec && !stopped_.load()) {
                handle_client(*socket);
            }
            if (!stopped_.load()) {
                do_accept();
            }
        });
    }

    void handle_client(tcp::socket& socket) {
        boost::system::error_code ec;
        std::string accumulated;
        std::array<char, 512> buf{};
        while (!stopped_.load()) {
            const std::size_t n = socket.read_some(asio::buffer(buf), ec);
            if (n > 0) {
                accumulated.append(buf.data(), n);
                if (accumulated.find("XLEN") != std::string::npos) {
                    asio::write(socket, asio::buffer(response_), ec);
                    break;
                }
            }
            if (ec) {
                break;
            }
        }
        socket.shutdown(tcp::socket::shutdown_both, ec);
        socket.close(ec);
    }

    std::string response_;
    asio::io_context ioc_;
    tcp::acceptor acceptor_;
    unsigned short port_{0};
    std::atomic<bool> stopped_{false};
    std::thread worker_;
};

class FakePostgresProbe final : public IPostgresProbe {
public:
    explicit FakePostgresProbe(PostgresProbeResult result) : result_(std::move(result)) {}

    PostgresProbeResult probe(std::uint32_t /*deadline_ms*/) override {
        return result_;
    }

private:
    PostgresProbeResult result_;
};

} // namespace

TEST(AdminConsoleV1Test, PrometheusParserAndEffectiveStatusStaleDegradation) {
    constexpr std::string_view kSample =
        "# HELP gateway_sessions_active Active gateway sessions\n"
        "# TYPE gateway_sessions_active gauge\n"
        "gateway_sessions_active 17\n"
        "wb_pending{worker=\"1\",zone=\"local\"} 42\n"
        "nan_metric NaN\n"
        "inf_metric +Inf\n"
        "fractional_metric 3.75\n";

    EXPECT_EQ(extract_prometheus_int64(kSample, "gateway_sessions_active"), 17);
    EXPECT_EQ(extract_prometheus_int64(kSample, "wb_pending"), 42);
    EXPECT_DOUBLE_EQ(extract_prometheus_metric_value(kSample, "fractional_metric").value_or(0.0), 3.75);
    EXPECT_FALSE(extract_prometheus_metric_value(kSample, "nan_metric").has_value());
    EXPECT_FALSE(extract_prometheus_metric_value(kSample, "inf_metric").has_value());
    EXPECT_FALSE(extract_prometheus_int64(kSample, "missing_metric").has_value());

    ServiceObservation uninitialized;
    uninitialized.status = ServiceStatus::READY;
    uninitialized.observed_at_unix_ms = 0;
    EXPECT_EQ(evaluate_effective_status(uninitialized, 1'000'000ULL, 15'000ULL), ServiceStatus::UNKNOWN);

    ServiceObservation fresh;
    fresh.status = ServiceStatus::READY;
    fresh.observed_at_unix_ms = 1'000'000ULL;
    EXPECT_EQ(evaluate_effective_status(fresh, 1'010'000ULL, 15'000ULL), ServiceStatus::READY);
    EXPECT_EQ(evaluate_effective_status(fresh, 1'015'001ULL, 15'000ULL), ServiceStatus::UNKNOWN);

    ServiceObservation completed;
    completed.kind = ServiceKind::ONE_SHOT;
    completed.status = ServiceStatus::COMPLETED;
    completed.observed_at_unix_ms = 1'000'000ULL;
    EXPECT_EQ(evaluate_effective_status(completed, 1'005'000ULL, 15'000ULL), ServiceStatus::COMPLETED);
}

TEST(AdminConsoleV1Test, BoundedHttpProbeHandlesHealthyErrorAndHungSocket) {
    ScopedHttpMockServer http_srv([](std::string_view target) -> ScopedHttpMockServer::HttpReply {
        if (target == "/healthz") {
            return {"200 OK", "ok\n"};
        }
        if (target == "/readyz") {
            return {"503 Service Unavailable", "warming\n"};
        }
        return {"404 Not Found", "not found\n"};
    });
    ScopedHungTcpServer hung_srv;

    const auto ok_res = bounded_http_get("127.0.0.1", http_srv.port(), "/healthz", 1000);
    EXPECT_TRUE(ok_res.ok);
    EXPECT_EQ(ok_res.status_code, 200);
    EXPECT_EQ(ok_res.body, "ok\n");

    const auto fail_res = bounded_http_get("127.0.0.1", http_srv.port(), "/readyz", 1000);
    EXPECT_FALSE(fail_res.ok);
    EXPECT_EQ(fail_res.status_code, 503);

    const auto start = std::chrono::steady_clock::now();
    const auto hung_res = bounded_http_get("127.0.0.1", hung_srv.port(), "/healthz", 120);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    EXPECT_FALSE(hung_res.ok);
    EXPECT_EQ(hung_res.status_code, 0);
    EXPECT_FALSE(hung_res.error.empty());
    EXPECT_LT(elapsed_ms, 1500);
}

TEST(AdminConsoleV1Test, InventoryPollerIsolatesSlowNodeAndClassifiesMigratorAndDlq) {
    ScopedHttpMockServer server1_mock([](std::string_view target) -> ScopedHttpMockServer::HttpReply {
        if (target == "/healthz" || target == "/readyz") {
            return {"200 OK", "ok\n"};
        }
        if (target == "/metrics") {
            return {"200 OK", "chat_session_active 11\n"};
        }
        return {"404 Not Found", ""};
    });

    ScopedHttpMockServer worker_mock([](std::string_view target) -> ScopedHttpMockServer::HttpReply {
        if (target == "/healthz" || target == "/readyz") {
            return {"200 OK", "ok\n"};
        }
        if (target == "/metrics") {
            return {"200 OK", "wb_pending 25\nwb_flush_dlq_total 4\n"};
        }
        return {"404 Not Found", ""};
    });

    ScopedHungTcpServer hung_node;
    ScopedRespMockServer redis_mock("+PONG\r\n:3\r\n");

    PostgresProbeResult pg_res;
    pg_res.healthy = true;
    pg_res.active_rooms = 7;
    pg_res.applied_migrations = 5;
    pg_res.expected_migrations = 5;
    pg_res.all_migrations_applied = true;
    pg_res.latency_ms = 4;
    auto fake_pg = std::make_shared<FakePostgresProbe>(pg_res);

    std::vector<ServiceTargetConfig> catalog;
    {
        ServiceTargetConfig t;
        t.service_id = "server-1";
        t.display_name = "server-1";
        t.kind = ServiceKind::LONG_RUNNING;
        t.layer = TopologyLayer::SERVER;
        t.probe_type = ProbeType::HTTP_HEALTH_READY;
        t.host = "127.0.0.1";
        t.port = server1_mock.port();
        catalog.push_back(t);
    }
    {
        ServiceTargetConfig t;
        t.service_id = "server-hung";
        t.display_name = "server-hung";
        t.kind = ServiceKind::LONG_RUNNING;
        t.layer = TopologyLayer::SERVER;
        t.probe_type = ProbeType::HTTP_HEALTH_READY;
        t.host = "127.0.0.1";
        t.port = hung_node.port();
        catalog.push_back(t);
    }
    {
        ServiceTargetConfig t;
        t.service_id = "wb_worker";
        t.display_name = "Write-Behind Worker";
        t.kind = ServiceKind::LONG_RUNNING;
        t.layer = TopologyLayer::WORKER;
        t.probe_type = ProbeType::HTTP_HEALTH_READY;
        t.host = "127.0.0.1";
        t.port = worker_mock.port();
        catalog.push_back(t);
    }
    {
        ServiceTargetConfig t;
        t.service_id = "redis";
        t.display_name = "Redis";
        t.kind = ServiceKind::INFRASTRUCTURE;
        t.layer = TopologyLayer::DATA;
        t.probe_type = ProbeType::REDIS_RESP;
        t.host = "127.0.0.1";
        t.port = redis_mock.port();
        catalog.push_back(t);
    }
    {
        ServiceTargetConfig t;
        t.service_id = "postgres";
        t.display_name = "PostgreSQL";
        t.kind = ServiceKind::INFRASTRUCTURE;
        t.layer = TopologyLayer::DATA;
        t.probe_type = ProbeType::POSTGRES_SQL;
        catalog.push_back(t);
    }
    {
        ServiceTargetConfig t;
        t.service_id = "migrator";
        t.display_name = "Schema Migrator";
        t.kind = ServiceKind::ONE_SHOT;
        t.layer = TopologyLayer::DATA;
        t.probe_type = ProbeType::POSTGRES_MIGRATOR;
        t.desired_count = 0;
        catalog.push_back(t);
    }

    AdminConfig cfg;
    cfg.probe_timeout_ms = 150;
    cfg.stale_ttl_ms = 15000;
    cfg.dlq_stream_key = "session_events_dlq";

    InventoryPoller poller(cfg, std::move(catalog), fake_pg);
    EXPECT_FALSE(poller.has_completed_initial_poll());

    const auto snap = poller.poll_once(1'700'000'000'000ULL);
    ASSERT_TRUE(snap != nullptr);
    EXPECT_TRUE(poller.has_completed_initial_poll());
    EXPECT_EQ(snap->desired_long_running, 5);
    EXPECT_EQ(snap->ready_long_running, 4);
    EXPECT_EQ(snap->server_sessions, 11ULL);
    EXPECT_EQ(snap->active_rooms, 7);
    EXPECT_EQ(snap->wb_pending, 25);
    EXPECT_EQ(snap->wb_dlq_count, 3);
    EXPECT_EQ(snap->wb_dlq_total, 4ULL);

    auto find_obs = [&](std::string_view id) -> const ServiceObservation* {
        for (const auto& obs : snap->services) {
            if (obs.service_id == id) {
                return &obs;
            }
        }
        return nullptr;
    };

    const auto* s1 = find_obs("server-1");
    ASSERT_NE(s1, nullptr);
    EXPECT_EQ(s1->status, ServiceStatus::READY);
    EXPECT_EQ(s1->active_sessions, 11);

    const auto* shung = find_obs("server-hung");
    ASSERT_NE(shung, nullptr);
    EXPECT_EQ(shung->status, ServiceStatus::UNKNOWN);

    const auto* worker = find_obs("wb_worker");
    ASSERT_NE(worker, nullptr);
    EXPECT_EQ(worker->status, ServiceStatus::DEGRADED);
    EXPECT_EQ(worker->wb_pending, 25);
    EXPECT_EQ(worker->wb_dlq_count, 3);
    EXPECT_EQ(worker->wb_dlq_total, 4ULL);

    const auto* redis = find_obs("redis");
    ASSERT_NE(redis, nullptr);
    EXPECT_EQ(redis->status, ServiceStatus::READY);
    EXPECT_EQ(redis->wb_dlq_count, 3);

    const auto* pg = find_obs("postgres");
    ASSERT_NE(pg, nullptr);
    EXPECT_EQ(pg->status, ServiceStatus::READY);
    EXPECT_EQ(pg->active_rooms, 7);

    const auto* mig = find_obs("migrator");
    ASSERT_NE(mig, nullptr);
    EXPECT_EQ(mig->status, ServiceStatus::COMPLETED);
    EXPECT_EQ(mig->applied_migrations, 5);
    EXPECT_EQ(mig->expected_migrations, 5);
}

TEST(AdminConsoleV1Test, AdminRouterAuthReadOnlyPathTraversalAndSecretRedaction) {
    AdminConfig cfg;
    cfg.auth_mode = AdminAuthMode::TOKEN;
    cfg.read_only = true;
    cfg.viewer_token = "tok_view_secret";
    cfg.admin_token = "tok_admin_secret";
    cfg.web_root = ADMIN_WEB_ASSET_DIR;
    cfg.db_uri = "postgresql://dynaxis:super_db_secret@127.0.0.1:5432/dynaxis_db";

    ServiceTargetConfig pg_target;
    pg_target.service_id = "postgres";
    pg_target.display_name = "PostgreSQL";
    pg_target.kind = ServiceKind::INFRASTRUCTURE;
    pg_target.layer = TopologyLayer::DATA;
    pg_target.probe_type = ProbeType::POSTGRES_SQL;

    PostgresProbeResult failed_pg;
    failed_pg.healthy = false;
    failed_pg.error = sanitize_connection_error(
        "connection failed: postgresql://dynaxis:super_db_secret@127.0.0.1:5432/dynaxis_db");
    auto fake_pg = std::make_shared<FakePostgresProbe>(failed_pg);

    InventoryPoller poller(cfg, {pg_target}, fake_pg);
    poller.poll_once();
    AdminRouter router(cfg, poller);

    AdminHttpRequest unauth_req{"GET", "/api/v1/overview", {}, {}, "127.0.0.1"};
    const auto unauth_resp = router.handle(unauth_req);
    EXPECT_EQ(unauth_resp.status, "401 Unauthorized");
    EXPECT_NE(unauth_resp.extra_headers.find("WWW-Authenticate: Bearer"), std::string::npos);

    AdminHttpRequest bad_tok_req{"GET", "/api/v1/overview", {{"authorization", "Bearer wrong_token"}}, {}, "127.0.0.1"};
    EXPECT_EQ(router.handle(bad_tok_req).status, "401 Unauthorized");

    AdminHttpRequest view_req{"GET", "/api/v1/overview", {{"authorization", "Bearer tok_view_secret"}}, {}, "127.0.0.1"};
    const auto view_resp = router.handle(view_req);
    EXPECT_EQ(view_resp.status, "200 OK");
    const auto overview_json = nlohmann::json::parse(view_resp.body);
    EXPECT_EQ(overview_json.at("meta").at("actor_role"), "viewer");
    EXPECT_EQ(view_resp.body.find("super_db_secret"), std::string::npos);
    EXPECT_EQ(view_resp.body.find("tok_view_secret"), std::string::npos);
    EXPECT_EQ(view_resp.body.find("tok_admin_secret"), std::string::npos);

    AdminHttpRequest mutating_req{"POST", "/api/v1/overview", {{"authorization", "Bearer tok_admin_secret"}}, {}, "127.0.0.1"};
    const auto mutating_resp = router.handle(mutating_req);
    EXPECT_EQ(mutating_resp.status, "403 Forbidden");
    const auto err_json = nlohmann::json::parse(mutating_resp.body);
    EXPECT_EQ(err_json.at("error"), "read_only_mode");

    AdminHttpRequest metrics_req{"GET", "/metrics", {}, {}, "127.0.0.1"};
    const auto metrics_resp = router.handle(metrics_req);
    EXPECT_EQ(metrics_resp.status, "200 OK");
    EXPECT_EQ(metrics_resp.body.find("super_db_secret"), std::string::npos);

    AdminHttpRequest root_req{"GET", "/", {}, {}, "127.0.0.1"};
    EXPECT_EQ(router.handle(root_req).status, "200 OK");

    AdminHttpRequest css_req{"GET", "/styles/tokens.css", {}, {}, "127.0.0.1"};
    EXPECT_EQ(router.handle(css_req).status, "200 OK");

    AdminHttpRequest trav_unix{"GET", "/../CMakeLists.txt", {}, {}, "127.0.0.1"};
    EXPECT_EQ(router.handle(trav_unix).status, "404 Not Found");

    AdminHttpRequest trav_win{"GET", "/js/..\\api.js", {}, {}, "127.0.0.1"};
    EXPECT_EQ(router.handle(trav_win).status, "404 Not Found");
}
