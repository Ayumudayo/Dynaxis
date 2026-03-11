#include <gtest/gtest.h>

#include <server/core/app/app_host.hpp>
#include <server/core/app/termination_signals.hpp>

#include <boost/asio/io_context.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using AppHost = server::core::app::AppHost;

void reset_termination_signal_flag() {
    server::core::app::detail::g_termination_signal_received = 0;
}

class TerminationSignalGuard {
public:
    TerminationSignalGuard() {
        reset_termination_signal_flag();
    }

    ~TerminationSignalGuard() {
        reset_termination_signal_flag();
    }
};

int shutdown_signal_for_test() {
#if defined(SIGINT)
    return SIGINT;
#elif defined(SIGTERM)
    return SIGTERM;
#else
    return 0;
#endif
}

template <typename Predicate>
bool wait_until(Predicate&& predicate,
                std::chrono::steady_clock::duration timeout = 2s,
                std::chrono::milliseconds poll_interval = 10ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(poll_interval);
    }
    return predicate();
}

} // namespace

TEST(AppHostLifecycleTest, RequiredDependenciesControlReadinessWhileOptionalDependenciesDoNot) {
    AppHost host{"app_host_dependency_test"};

    host.set_ready(true);
    host.declare_dependency("redis");
    host.declare_dependency("telemetry", AppHost::DependencyRequirement::kOptional);

    EXPECT_FALSE(host.dependencies_ok());
    EXPECT_FALSE(host.ready());
    EXPECT_EQ(host.readiness_body(false), "not ready: deps=redis\n");

    std::string dependency_metrics = host.dependency_metrics_text();
    EXPECT_NE(dependency_metrics.find("runtime_dependency_ready{name=\"redis\",required=\"true\"} 0"), std::string::npos);
    EXPECT_NE(dependency_metrics.find("runtime_dependency_ready{name=\"telemetry\",required=\"false\"} 0"), std::string::npos);
    EXPECT_NE(dependency_metrics.find("runtime_dependencies_ok 0"), std::string::npos);

    host.set_dependency_ok("telemetry", false);
    EXPECT_FALSE(host.dependencies_ok());

    host.set_dependency_ok("redis", true);
    EXPECT_TRUE(host.dependencies_ok());
    EXPECT_TRUE(host.ready());
    EXPECT_EQ(host.readiness_body(true), "ready\n");

    dependency_metrics = host.dependency_metrics_text();
    EXPECT_NE(dependency_metrics.find("runtime_dependency_ready{name=\"redis\",required=\"true\"} 1"), std::string::npos);
    EXPECT_NE(dependency_metrics.find("runtime_dependencies_ok 1"), std::string::npos);
}

TEST(AppHostLifecycleTest, ReadinessAndHealthStayIndependentFromLifecyclePhase) {
    AppHost host{"app_host_lifecycle_state_test"};

    host.declare_dependency("redis");
    host.set_dependency_ok("redis", true);
    host.set_ready(true);
    host.set_healthy(false);
    host.set_lifecycle_phase(AppHost::LifecyclePhase::kBootstrapping);

    EXPECT_TRUE(host.ready());
    EXPECT_FALSE(host.healthy());
    EXPECT_EQ(host.lifecycle_phase(), AppHost::LifecyclePhase::kBootstrapping);
    EXPECT_EQ(host.health_body(false), "unhealthy\n");
    EXPECT_EQ(host.readiness_body(false), "not ready: unhealthy\n");

    const std::string lifecycle_metrics = host.lifecycle_metrics_text();
    EXPECT_NE(lifecycle_metrics.find("runtime_lifecycle_phase_code 1"), std::string::npos);
    EXPECT_NE(lifecycle_metrics.find("runtime_lifecycle_phase{phase=\"bootstrapping\"} 1"), std::string::npos);
}

TEST(AppHostLifecycleTest, LifecyclePhaseNamesCoverTerminalStates) {
    AppHost host{"app_host_terminal_phase_test"};

    host.set_lifecycle_phase(AppHost::LifecyclePhase::kStopped);
    EXPECT_EQ(host.lifecycle_phase(), AppHost::LifecyclePhase::kStopped);
    EXPECT_STREQ(AppHost::lifecycle_phase_name(AppHost::LifecyclePhase::kStopped), "stopped");
    std::string lifecycle_metrics = host.lifecycle_metrics_text();
    EXPECT_NE(lifecycle_metrics.find("runtime_lifecycle_phase_code 4"), std::string::npos);
    EXPECT_NE(lifecycle_metrics.find("runtime_lifecycle_phase{phase=\"stopped\"} 1"), std::string::npos);

    host.set_lifecycle_phase(AppHost::LifecyclePhase::kFailed);
    EXPECT_EQ(host.lifecycle_phase(), AppHost::LifecyclePhase::kFailed);
    EXPECT_STREQ(AppHost::lifecycle_phase_name(AppHost::LifecyclePhase::kFailed), "failed");
    lifecycle_metrics = host.lifecycle_metrics_text();
    EXPECT_NE(lifecycle_metrics.find("runtime_lifecycle_phase_code 5"), std::string::npos);
    EXPECT_NE(lifecycle_metrics.find("runtime_lifecycle_phase{phase=\"failed\"} 1"), std::string::npos);
}

TEST(AppHostLifecycleTest, TerminationSignalFlagReflectsGlobalStopIntent) {
    TerminationSignalGuard signal_guard;

    EXPECT_FALSE(server::core::app::termination_signal_received());
    server::core::app::install_termination_signal_handlers();

    const int signal_number = shutdown_signal_for_test();
    ASSERT_NE(signal_number, 0);

    server::core::app::detail::termination_signal_handler(signal_number);
    EXPECT_TRUE(server::core::app::termination_signal_received());

    AppHost host{"app_host_global_signal_test"};
    EXPECT_TRUE(host.stop_requested());
    EXPECT_EQ(host.health_body(false), "stopping\n");
}

TEST(AppHostLifecycleTest, AsioTerminationSignalRunsShutdownStepsInLifoOrder) {
    TerminationSignalGuard signal_guard;

    boost::asio::io_context io;
    AppHost host{"app_host_asio_signal_test"};
    host.set_ready(true);
    host.set_lifecycle_phase(AppHost::LifecyclePhase::kRunning);

    std::mutex order_mutex;
    std::vector<std::string> shutdown_order;
    std::atomic<int> shutdown_calls{0};

    host.add_shutdown_step("storage", [&]() {
        std::lock_guard<std::mutex> lock(order_mutex);
        shutdown_order.push_back("storage");
    });
    host.add_shutdown_step("acceptor", [&]() {
        std::lock_guard<std::mutex> lock(order_mutex);
        shutdown_order.push_back("acceptor");
    });

    host.install_asio_termination_signals(io, [&]() {
        shutdown_calls.fetch_add(1, std::memory_order_relaxed);
        io.stop();
    });

    std::thread io_thread([&]() {
        io.run();
    });

    std::this_thread::sleep_for(50ms);

    const int signal_number = shutdown_signal_for_test();
    ASSERT_NE(signal_number, 0);
    ASSERT_EQ(std::raise(signal_number), 0);

    if (!wait_until([&]() { return shutdown_calls.load(std::memory_order_relaxed) == 1; })) {
        io.stop();
        io_thread.join();
        FAIL() << "timed out waiting for shutdown callback";
    }

    io_thread.join();

    EXPECT_TRUE(host.stop_requested());
    EXPECT_FALSE(host.ready());
    EXPECT_EQ(host.lifecycle_phase(), AppHost::LifecyclePhase::kStopping);
    EXPECT_EQ(host.health_body(false), "stopping\n");
    EXPECT_EQ(host.readiness_body(false), "not ready: stopping, starting\n");
    EXPECT_EQ(shutdown_calls.load(std::memory_order_relaxed), 1);

    const std::vector<std::string> expected{"acceptor", "storage"};
    std::lock_guard<std::mutex> lock(order_mutex);
    EXPECT_EQ(shutdown_order, expected);
}
