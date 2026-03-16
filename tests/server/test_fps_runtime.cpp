#include <gtest/gtest.h>

#include "server/fps/fps_service.hpp"
#include "server/protocol/game_opcodes.hpp"
#include "server/wire/codec.hpp"
#include "wire.pb.h"

namespace {

using server::app::fps::FixedStepDriver;
using server::app::fps::InputCommand;
using server::app::fps::RuntimeConfig;
using server::app::fps::WorldRuntime;

TEST(FixedStepDriverTest, BoundsCatchUpSteps) {
    FixedStepDriver driver(30, 4);

    const auto steps = driver.consume_elapsed(std::chrono::milliseconds(200));
    EXPECT_EQ(steps, 4u);

    const auto next_steps = driver.consume_elapsed(std::chrono::milliseconds(10));
    EXPECT_LE(next_steps, 1u);
}

TEST(FpsWorldRuntimeTest, RejectsOlderStagedInputOrder) {
    WorldRuntime runtime(RuntimeConfig{});

    EXPECT_TRUE(runtime.stage_input(1, InputCommand{.input_seq = 2, .move_x_mm = 100}));
    EXPECT_FALSE(runtime.stage_input(1, InputCommand{.input_seq = 1, .move_x_mm = 999}));

    (void)runtime.tick();
    const auto actor_id = runtime.actor_id_for_session(1);
    ASSERT_TRUE(actor_id.has_value());

    const auto sample = runtime.latest_history_at_or_before(*actor_id, 1);
    ASSERT_TRUE(sample.has_value());
    EXPECT_EQ(sample->last_applied_input_seq, 2u);
    EXPECT_EQ(sample->x_mm, 100);
}

TEST(FpsWorldRuntimeTest, EmitsSnapshotThenDelta) {
    WorldRuntime runtime(RuntimeConfig{});

    EXPECT_TRUE(runtime.stage_input(1, InputCommand{.input_seq = 1, .move_x_mm = 100}));
    const auto first_tick = runtime.tick();
    ASSERT_EQ(first_tick.size(), 1u);
    EXPECT_EQ(first_tick.front().msg_id, server::protocol::MSG_FPS_STATE_SNAPSHOT);

    server::wire::v1::FpsStateSnapshot snapshot;
    ASSERT_TRUE(server::wire::codec::Decode(
        first_tick.front().payload.data(), first_tick.front().payload.size(), snapshot));
    EXPECT_EQ(snapshot.actors_size(), 1);

    EXPECT_TRUE(runtime.stage_input(1, InputCommand{.input_seq = 2, .move_x_mm = 100}));
    const auto second_tick = runtime.tick();
    ASSERT_EQ(second_tick.size(), 1u);
    EXPECT_EQ(second_tick.front().msg_id, server::protocol::MSG_FPS_STATE_DELTA);

    server::wire::v1::FpsStateDelta delta;
    ASSERT_TRUE(server::wire::codec::Decode(
        second_tick.front().payload.data(), second_tick.front().payload.size(), delta));
    EXPECT_EQ(delta.actors_size(), 1);
    EXPECT_EQ(delta.actors(0).last_applied_input_seq(), 2u);
}

TEST(FpsWorldRuntimeTest, InterestSelectionUsesCoarseCells) {
    WorldRuntime runtime(RuntimeConfig{
        .interest_cell_size_mm = 10'000,
        .interest_radius_cells = 1,
        .max_interest_recipients_per_tick = 64,
    });

    EXPECT_TRUE(runtime.stage_input(1, InputCommand{.input_seq = 1}));
    EXPECT_TRUE(runtime.stage_input(2, InputCommand{.input_seq = 1, .move_x_mm = 25'000}));

    const auto tick = runtime.tick();
    ASSERT_EQ(tick.size(), 2u);

    for (const auto& message : tick) {
        ASSERT_EQ(message.msg_id, server::protocol::MSG_FPS_STATE_SNAPSHOT);
        server::wire::v1::FpsStateSnapshot snapshot;
        ASSERT_TRUE(server::wire::codec::Decode(message.payload.data(), message.payload.size(), snapshot));
        EXPECT_EQ(snapshot.actors_size(), 1);
    }
}

TEST(FpsWorldRuntimeTest, HistoryLookupReturnsLatestSampleAtOrBeforeTick) {
    WorldRuntime runtime(RuntimeConfig{});

    EXPECT_TRUE(runtime.stage_input(1, InputCommand{.input_seq = 1, .move_x_mm = 100}));
    (void)runtime.tick();
    EXPECT_TRUE(runtime.stage_input(1, InputCommand{.input_seq = 2, .move_x_mm = 200}));
    (void)runtime.tick();

    const auto actor_id = runtime.actor_id_for_session(1);
    ASSERT_TRUE(actor_id.has_value());

    const auto sample_tick1 = runtime.latest_history_at_or_before(*actor_id, 1);
    ASSERT_TRUE(sample_tick1.has_value());
    EXPECT_EQ(sample_tick1->server_tick, 1u);
    EXPECT_EQ(sample_tick1->x_mm, 100);

    const auto sample_tick2 = runtime.latest_history_at_or_before(*actor_id, 2);
    ASSERT_TRUE(sample_tick2.has_value());
    EXPECT_EQ(sample_tick2->server_tick, 2u);
    EXPECT_EQ(sample_tick2->x_mm, 300);
}

} // namespace
