#include <gtest/gtest.h>

#include <server/core/app/engine_builder.hpp>
#include <server/core/app/engine_context.hpp>
#include <server/core/app/engine_runtime.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace {

using AppHost = server::core::app::AppHost;

struct EngineCompositionService {
    int value{0};
};

} // namespace

TEST(EngineCompositionTest, EngineContextIsInstanceScoped) {
    server::core::app::EngineContext left;
    server::core::app::EngineContext right;

    auto service = std::make_shared<EngineCompositionService>();
    service->value = 42;
    left.set(service);

    EXPECT_TRUE(left.has<EngineCompositionService>());
    EXPECT_FALSE(right.has<EngineCompositionService>());
    EXPECT_EQ(left.get<EngineCompositionService>(), service);
    EXPECT_EQ(left.require<EngineCompositionService>().value, 42);

    left.clear();
    EXPECT_FALSE(left.has<EngineCompositionService>());
}

TEST(EngineCompositionTest, BuilderSeedsRuntimeLifecycleAndDependencies) {
    server::core::app::EngineBuilder builder("engine_runtime_test");
    builder.declare_dependency("redis");
    builder.initial_ready(true);

    auto runtime = builder.build();

    EXPECT_EQ(runtime.name(), "engine_runtime_test");
    EXPECT_EQ(runtime.lifecycle_phase(), AppHost::LifecyclePhase::kBootstrapping);
    EXPECT_FALSE(runtime.ready());
    EXPECT_FALSE(runtime.dependencies_ok());

    runtime.set_dependency_ok("redis", true);
    EXPECT_TRUE(runtime.dependencies_ok());
    EXPECT_TRUE(runtime.ready());

    runtime.set_lifecycle_phase(AppHost::LifecyclePhase::kRunning);
    EXPECT_EQ(runtime.lifecycle_phase(), AppHost::LifecyclePhase::kRunning);

    auto service = std::make_shared<EngineCompositionService>();
    service->value = 7;
    runtime.context().set(service);

    EXPECT_TRUE(runtime.context().has<EngineCompositionService>());
    EXPECT_EQ(runtime.context().require<EngineCompositionService>().value, 7);

    const std::string dependency_metrics = runtime.host().dependency_metrics_text();
    EXPECT_NE(dependency_metrics.find("runtime_dependency_ready{name=\"redis\",required=\"true\"} 1"), std::string::npos);
}

TEST(EngineCompositionTest, BridgeServiceExportsToGlobalRegistry) {
    server::core::util::services::clear();

    server::core::app::EngineRuntime runtime =
        server::core::app::EngineBuilder("engine_runtime_bridge_test").build();

    auto service = std::make_shared<EngineCompositionService>();
    service->value = 99;
    runtime.bridge_service(service);

    ASSERT_TRUE(runtime.context().has<EngineCompositionService>());
    ASSERT_TRUE(server::core::util::services::has<EngineCompositionService>());
    EXPECT_EQ(runtime.context().require<EngineCompositionService>().value, 99);
    EXPECT_EQ(server::core::util::services::require<EngineCompositionService>().value, 99);

    server::core::util::services::clear();
}

TEST(EngineCompositionTest, BridgeAliasAndLifecycleHelpersStandardizeBootstrapFlow) {
    server::core::util::services::clear();

    server::core::app::EngineRuntime runtime =
        server::core::app::EngineBuilder("engine_runtime_alias_test").build();

    int local_flag = 5;
    runtime.bridge_alias(local_flag);

    ASSERT_TRUE(runtime.context().has<int>());
    EXPECT_EQ(runtime.context().require<int>(), 5);
    EXPECT_EQ(*server::core::util::services::get<int>(), 5);

    runtime.mark_running();
    EXPECT_TRUE(runtime.ready());
    EXPECT_EQ(runtime.lifecycle_phase(), AppHost::LifecyclePhase::kRunning);

    runtime.mark_stopped();
    EXPECT_FALSE(runtime.ready());
    EXPECT_EQ(runtime.lifecycle_phase(), AppHost::LifecyclePhase::kStopped);

    runtime.mark_failed();
    EXPECT_FALSE(runtime.ready());
    EXPECT_EQ(runtime.lifecycle_phase(), AppHost::LifecyclePhase::kFailed);

    runtime.clear_global_services();
    EXPECT_FALSE(server::core::util::services::has<int>());
}

TEST(EngineCompositionTest, WaitForStopAndRunShutdownStandardizeNormalTeardown) {
    server::core::app::EngineRuntime runtime =
        server::core::app::EngineBuilder("engine_runtime_shutdown_test").build();

    std::atomic<int> shutdown_count{0};
    runtime.add_shutdown_step("increment", [&]() { shutdown_count.fetch_add(1, std::memory_order_relaxed); });

    std::thread stopper([&runtime]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        runtime.request_stop();
    });

    runtime.mark_running();
    runtime.wait_for_stop(std::chrono::milliseconds(1));
    runtime.run_shutdown();
    runtime.mark_stopped();

    stopper.join();

    EXPECT_EQ(shutdown_count.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(runtime.lifecycle_phase(), AppHost::LifecyclePhase::kStopped);
    EXPECT_FALSE(runtime.ready());

    runtime.run_shutdown();
    EXPECT_EQ(shutdown_count.load(std::memory_order_relaxed), 1);
}
