#include <gtest/gtest.h>

#include <tuple>
#include <vector>

#include "server/core/realtime/simulation_phase.hpp"
#include "server/core/realtime/runtime.hpp"

namespace {

using server::core::realtime::FixedStepDriver;
using server::core::realtime::InputCommand;
using server::core::realtime::ReplicationKind;
using server::core::realtime::RewindQuery;
using server::core::realtime::RuntimeConfig;
using server::core::realtime::StageInputDisposition;
using server::core::realtime::SimulationPhase;
using server::core::realtime::SimulationPhaseContext;
using server::core::realtime::ISimulationPhaseObserver;
using server::core::realtime::WorldRuntime;

class RecordingPhaseObserver final : public ISimulationPhaseObserver {
public:
    void on_simulation_phase(SimulationPhase phase, const SimulationPhaseContext& context) override {
        events.emplace_back(phase, context.server_tick, context.replication_update_count);
    }

    std::vector<std::tuple<SimulationPhase, std::uint32_t, std::size_t>> events;
};

TEST(FixedStepDriverTest, BoundsCatchUpSteps) {
    FixedStepDriver driver(30, 4);

    const auto steps = driver.consume_elapsed(std::chrono::milliseconds(200));
    EXPECT_EQ(steps, 4u);

    const auto next_steps = driver.consume_elapsed(std::chrono::milliseconds(10));
    EXPECT_LE(next_steps, 1u);
}

TEST(FpsWorldRuntimeTest, RejectsOlderStagedInputOrder) {
    WorldRuntime runtime(RuntimeConfig{});

    const auto accepted = runtime.stage_input(1, InputCommand{.input_seq = 2, .move_x_mm = 100});
    EXPECT_EQ(accepted.disposition, StageInputDisposition::kAccepted);
    EXPECT_EQ(accepted.target_server_tick, 1u);

    const auto rejected = runtime.stage_input(1, InputCommand{.input_seq = 1, .move_x_mm = 999});
    EXPECT_EQ(rejected.disposition, StageInputDisposition::kRejectedStale);
    EXPECT_EQ(rejected.target_server_tick, 1u);

    (void)runtime.tick();
    const auto actor_id = runtime.actor_id_for_session(1);
    ASSERT_TRUE(actor_id.has_value());

    const auto sample = runtime.rewind_at_or_before(RewindQuery{.actor_id = *actor_id, .server_tick = 1});
    ASSERT_TRUE(sample.has_value());
    EXPECT_TRUE(sample->exact_tick);
    EXPECT_EQ(sample->sample.last_applied_input_seq, 2u);
    EXPECT_EQ(sample->sample.x_mm, 100);
}

TEST(FpsWorldRuntimeTest, EmitsSnapshotThenDelta) {
    WorldRuntime runtime(RuntimeConfig{});

    EXPECT_EQ(
        runtime.stage_input(1, InputCommand{.input_seq = 1, .move_x_mm = 100}).disposition,
        StageInputDisposition::kAccepted);
    const auto first_tick = runtime.tick();
    ASSERT_EQ(first_tick.size(), 1u);
    EXPECT_EQ(first_tick.front().kind, ReplicationKind::kSnapshot);
    EXPECT_EQ(first_tick.front().actors.size(), 1u);
    EXPECT_EQ(first_tick.front().server_tick, 1u);
    EXPECT_EQ(first_tick.front().actors.front().server_tick, 1u);

    EXPECT_EQ(
        runtime.stage_input(1, InputCommand{.input_seq = 2, .move_x_mm = 100}).disposition,
        StageInputDisposition::kAccepted);
    const auto second_tick = runtime.tick();
    ASSERT_EQ(second_tick.size(), 1u);
    EXPECT_EQ(second_tick.front().kind, ReplicationKind::kDelta);
    EXPECT_EQ(second_tick.front().actors.size(), 1u);
    EXPECT_EQ(second_tick.front().actors.front().last_applied_input_seq, 2u);
}

TEST(FpsWorldRuntimeTest, InterestSelectionUsesCoarseCells) {
    WorldRuntime runtime(RuntimeConfig{
        .interest_cell_size_mm = 10'000,
        .interest_radius_cells = 1,
        .max_interest_recipients_per_tick = 64,
    });

    EXPECT_EQ(runtime.stage_input(1, InputCommand{.input_seq = 1}).disposition, StageInputDisposition::kAccepted);
    EXPECT_EQ(
        runtime.stage_input(2, InputCommand{.input_seq = 1, .move_x_mm = 25'000}).disposition,
        StageInputDisposition::kAccepted);

    const auto tick = runtime.tick();
    ASSERT_EQ(tick.size(), 2u);

    for (const auto& message : tick) {
        ASSERT_EQ(message.kind, ReplicationKind::kSnapshot);
        EXPECT_EQ(message.actors.size(), 1u);
    }
}

TEST(FpsWorldRuntimeTest, HistoryLookupReturnsLatestSampleAtOrBeforeTick) {
    WorldRuntime runtime(RuntimeConfig{});

    EXPECT_EQ(
        runtime.stage_input(1, InputCommand{.input_seq = 1, .move_x_mm = 100}).disposition,
        StageInputDisposition::kAccepted);
    (void)runtime.tick();
    EXPECT_EQ(
        runtime.stage_input(1, InputCommand{.input_seq = 2, .move_x_mm = 200}).disposition,
        StageInputDisposition::kAccepted);
    (void)runtime.tick();

    const auto actor_id = runtime.actor_id_for_session(1);
    ASSERT_TRUE(actor_id.has_value());

    const auto sample_tick1 = runtime.rewind_at_or_before(RewindQuery{.actor_id = *actor_id, .server_tick = 1});
    ASSERT_TRUE(sample_tick1.has_value());
    EXPECT_TRUE(sample_tick1->exact_tick);
    EXPECT_EQ(sample_tick1->sample.server_tick, 1u);
    EXPECT_EQ(sample_tick1->sample.x_mm, 100);

    const auto sample_tick2 = runtime.rewind_at_or_before(RewindQuery{.actor_id = *actor_id, .server_tick = 2});
    ASSERT_TRUE(sample_tick2.has_value());
    EXPECT_TRUE(sample_tick2->exact_tick);
    EXPECT_EQ(sample_tick2->sample.server_tick, 2u);
    EXPECT_EQ(sample_tick2->sample.x_mm, 300);
}

TEST(FpsWorldRuntimeTest, DeltaBudgetFallsBackToSnapshot) {
    WorldRuntime runtime(RuntimeConfig{
        .max_delta_actors_per_tick = 1,
    });

    EXPECT_EQ(runtime.stage_input(1, InputCommand{.input_seq = 1, .move_x_mm = 100}).disposition, StageInputDisposition::kAccepted);
    EXPECT_EQ(runtime.stage_input(2, InputCommand{.input_seq = 1, .move_x_mm = 150}).disposition, StageInputDisposition::kAccepted);
    (void)runtime.tick();

    EXPECT_EQ(runtime.stage_input(1, InputCommand{.input_seq = 2, .move_x_mm = 100}).disposition, StageInputDisposition::kAccepted);
    EXPECT_EQ(runtime.stage_input(2, InputCommand{.input_seq = 2, .move_x_mm = 150}).disposition, StageInputDisposition::kAccepted);

    const auto second_tick = runtime.tick();
    ASSERT_EQ(second_tick.size(), 2u);
    EXPECT_EQ(second_tick[0].kind, ReplicationKind::kSnapshot);
    EXPECT_EQ(second_tick[1].kind, ReplicationKind::kSnapshot);
    EXPECT_GT(runtime.snapshot().delta_budget_snapshot_total, 0u);
}

TEST(FpsWorldRuntimeTest, EmitsSimulationPhasesInDeterministicOrder) {
    RecordingPhaseObserver observer;
    WorldRuntime runtime(RuntimeConfig{});
    runtime.set_simulation_phase_observer(&observer);

    EXPECT_EQ(runtime.stage_input(1, InputCommand{.input_seq = 1, .move_x_mm = 100}).disposition, StageInputDisposition::kAccepted);

    const auto updates = runtime.tick();
    ASSERT_FALSE(updates.empty());
    ASSERT_EQ(observer.events.size(), 5u);
    EXPECT_EQ(std::get<0>(observer.events[0]), SimulationPhase::kTickBegin);
    EXPECT_EQ(std::get<0>(observer.events[1]), SimulationPhase::kInputsApplied);
    EXPECT_EQ(std::get<0>(observer.events[2]), SimulationPhase::kActorsAdvanced);
    EXPECT_EQ(std::get<0>(observer.events[3]), SimulationPhase::kReplicationComputed);
    EXPECT_EQ(std::get<0>(observer.events[4]), SimulationPhase::kTickEnd);
    EXPECT_EQ(std::get<1>(observer.events[0]), 1u);
    EXPECT_EQ(std::get<2>(observer.events[3]), updates.size());
}

} // namespace
