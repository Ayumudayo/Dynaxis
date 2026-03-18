#include <gtest/gtest.h>

#include <server/core/worlds/migration.hpp>
#include <server/core/worlds/world_drain.hpp>
#include <server/core/worlds/world_transfer.hpp>

TEST(MmorpgWorldDrainContractTest, ReportsDrainingSessionsWhenDrainDeclaredAndSessionsRemain) {
    server::core::worlds::ObservedWorldDrainState state;
    state.world_id = "starter-a";
    state.owner_instance_id = "server-1";
    state.draining = true;
    state.instances = {
        {.instance_id = "server-1", .ready = true, .active_sessions = 2},
    };

    const auto status = server::core::worlds::evaluate_world_drain(state);
    EXPECT_EQ(status.phase, server::core::worlds::WorldDrainPhase::kDrainingSessions);
    EXPECT_TRUE(status.summary.drain_declared);
    EXPECT_EQ(status.summary.active_sessions_total, 2u);
    EXPECT_EQ(status.summary.owner_active_sessions, 2u);
}

TEST(MmorpgWorldDrainContractTest, ReportsDrainedWhenNoSessionsRemain) {
    server::core::worlds::ObservedWorldDrainState state;
    state.world_id = "starter-a";
    state.owner_instance_id = "server-1";
    state.draining = true;
    state.instances = {
        {.instance_id = "server-1", .ready = true, .active_sessions = 0},
    };

    const auto status = server::core::worlds::evaluate_world_drain(state);
    EXPECT_EQ(status.phase, server::core::worlds::WorldDrainPhase::kDrained);
    EXPECT_EQ(status.summary.active_sessions_total, 0u);
}

TEST(MmorpgWorldDrainContractTest, ReportsBlockedReplacementStatesWhenDeclaredTargetIsMissingOrUnready) {
    server::core::worlds::ObservedWorldDrainState missing_target;
    missing_target.world_id = "starter-a";
    missing_target.owner_instance_id = "server-1";
    missing_target.draining = true;
    missing_target.replacement_owner_instance_id = "server-2";
    missing_target.instances = {
        {.instance_id = "server-1", .ready = true, .active_sessions = 1},
    };

    const auto missing_status = server::core::worlds::evaluate_world_drain(missing_target);
    EXPECT_EQ(missing_status.phase, server::core::worlds::WorldDrainPhase::kReplacementTargetMissing);

    server::core::worlds::ObservedWorldDrainState unready_target = missing_target;
    unready_target.instances.push_back({.instance_id = "server-2", .ready = false, .active_sessions = 0});

    const auto unready_status = server::core::worlds::evaluate_world_drain(unready_target);
    EXPECT_EQ(unready_status.phase, server::core::worlds::WorldDrainPhase::kReplacementTargetNotReady);
    EXPECT_TRUE(unready_status.summary.replacement_present);
    EXPECT_FALSE(unready_status.summary.replacement_ready);
}

TEST(MmorpgWorldDrainContractTest, OrchestrationRequestsOwnerCommitAfterDrainCompletesWithTransferDeclared) {
    const auto drain = server::core::worlds::evaluate_world_drain(
        server::core::worlds::ObservedWorldDrainState{
            .world_id = "starter-a",
            .owner_instance_id = "server-1",
            .draining = true,
            .replacement_owner_instance_id = "server-2",
            .instances = {
                {.instance_id = "server-1", .ready = true, .active_sessions = 0},
                {.instance_id = "server-2", .ready = true, .active_sessions = 0},
            },
        });
    const auto transfer = server::core::worlds::evaluate_world_transfer(
        server::core::worlds::ObservedWorldTransferState{
            .world_id = "starter-a",
            .owner_instance_id = "server-1",
            .draining = true,
            .replacement_owner_instance_id = "server-2",
            .instances = {
                {.instance_id = "server-1", .ready = true},
                {.instance_id = "server-2", .ready = true},
            },
        });

    const auto orchestration =
        server::core::worlds::evaluate_world_drain_orchestration(drain, transfer, std::nullopt);
    EXPECT_EQ(
        orchestration.phase,
        server::core::worlds::WorldDrainOrchestrationPhase::kAwaitingOwnerTransfer);
    EXPECT_EQ(
        orchestration.next_action,
        server::core::worlds::WorldDrainNextAction::kCommitOwnerTransfer);
    EXPECT_TRUE(orchestration.summary.transfer_declared);
    EXPECT_FALSE(orchestration.summary.transfer_committed);
}

TEST(MmorpgWorldDrainContractTest, OrchestrationRequestsClearAfterCommittedTransfer) {
    const auto drain = server::core::worlds::evaluate_world_drain(
        server::core::worlds::ObservedWorldDrainState{
            .world_id = "starter-a",
            .owner_instance_id = "server-2",
            .draining = true,
            .replacement_owner_instance_id = "server-2",
            .instances = {
                {.instance_id = "server-1", .ready = true, .active_sessions = 0},
                {.instance_id = "server-2", .ready = true, .active_sessions = 0},
            },
        });
    const auto transfer = server::core::worlds::evaluate_world_transfer(
        server::core::worlds::ObservedWorldTransferState{
            .world_id = "starter-a",
            .owner_instance_id = "server-2",
            .draining = true,
            .replacement_owner_instance_id = "server-2",
            .instances = {
                {.instance_id = "server-1", .ready = true},
                {.instance_id = "server-2", .ready = true},
            },
        });

    const auto orchestration =
        server::core::worlds::evaluate_world_drain_orchestration(drain, transfer, std::nullopt);
    EXPECT_EQ(
        orchestration.phase,
        server::core::worlds::WorldDrainOrchestrationPhase::kReadyToClear);
    EXPECT_EQ(
        orchestration.next_action,
        server::core::worlds::WorldDrainNextAction::kClearPolicy);
    EXPECT_TRUE(orchestration.summary.transfer_committed);
    EXPECT_TRUE(orchestration.summary.clear_allowed);
}

TEST(MmorpgWorldDrainContractTest, OrchestrationWaitsForMigrationReadinessWhenEnvelopeIsNotReady) {
    const auto drain = server::core::worlds::evaluate_world_drain(
        server::core::worlds::ObservedWorldDrainState{
            .world_id = "starter-a",
            .owner_instance_id = "server-1",
            .draining = true,
            .instances = {
                {.instance_id = "server-1", .ready = true, .active_sessions = 0},
            },
        });
    const auto migration = server::core::worlds::evaluate_world_migration(
        server::core::worlds::ObservedWorldMigrationWorld{
            .world_id = "starter-a",
            .current_owner_instance_id = "server-1",
            .draining = true,
            .instances = {
                {.instance_id = "server-1", .ready = true},
            },
        },
        server::core::worlds::WorldMigrationEnvelope{
            .target_world_id = "starter-b",
            .target_owner_instance_id = "server-2",
            .preserve_room = true,
        },
        server::core::worlds::ObservedWorldMigrationWorld{
            .world_id = "starter-b",
            .current_owner_instance_id = "server-2",
            .draining = false,
            .instances = {
                {.instance_id = "server-2", .ready = false},
            },
        });

    const auto orchestration =
        server::core::worlds::evaluate_world_drain_orchestration(drain, std::nullopt, migration);
    EXPECT_EQ(
        orchestration.phase,
        server::core::worlds::WorldDrainOrchestrationPhase::kAwaitingMigration);
    EXPECT_EQ(
        orchestration.next_action,
        server::core::worlds::WorldDrainNextAction::kAwaitMigration);
    EXPECT_TRUE(orchestration.summary.migration_declared);
    EXPECT_FALSE(orchestration.summary.migration_ready);
}

TEST(MmorpgWorldDrainContractTest, OrchestrationRequestsClearAfterReadyMigration) {
    const auto drain = server::core::worlds::evaluate_world_drain(
        server::core::worlds::ObservedWorldDrainState{
            .world_id = "starter-a",
            .owner_instance_id = "server-1",
            .draining = true,
            .instances = {
                {.instance_id = "server-1", .ready = true, .active_sessions = 0},
            },
        });
    const auto migration = server::core::worlds::evaluate_world_migration(
        server::core::worlds::ObservedWorldMigrationWorld{
            .world_id = "starter-a",
            .current_owner_instance_id = "server-1",
            .draining = true,
            .instances = {
                {.instance_id = "server-1", .ready = true},
            },
        },
        server::core::worlds::WorldMigrationEnvelope{
            .target_world_id = "starter-b",
            .target_owner_instance_id = "server-2",
            .preserve_room = true,
        },
        server::core::worlds::ObservedWorldMigrationWorld{
            .world_id = "starter-b",
            .current_owner_instance_id = "server-2",
            .draining = false,
            .instances = {
                {.instance_id = "server-2", .ready = true},
            },
        });

    const auto orchestration =
        server::core::worlds::evaluate_world_drain_orchestration(drain, std::nullopt, migration);
    EXPECT_EQ(
        orchestration.phase,
        server::core::worlds::WorldDrainOrchestrationPhase::kReadyToClear);
    EXPECT_EQ(
        orchestration.next_action,
        server::core::worlds::WorldDrainNextAction::kClearPolicy);
    EXPECT_TRUE(orchestration.summary.migration_declared);
    EXPECT_TRUE(orchestration.summary.migration_ready);
    EXPECT_TRUE(orchestration.summary.clear_allowed);
    EXPECT_EQ(orchestration.target_world_id, "starter-b");
    EXPECT_EQ(orchestration.target_owner_instance_id, "server-2");
}
