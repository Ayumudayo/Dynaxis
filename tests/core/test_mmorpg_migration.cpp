#include <gtest/gtest.h>

#include <server/core/mmorpg/migration.hpp>

TEST(MmorpgMigrationContractTest, ReportsReadyToResumeWhenTargetWorldOwnerIsReadyAndSourceDrains) {
    server::core::mmorpg::ObservedWorldMigrationWorld source_world{
        .world_id = "starter-a",
        .current_owner_instance_id = "server-1",
        .draining = true,
        .instances = {{.instance_id = "server-1", .ready = true}},
    };
    server::core::mmorpg::WorldMigrationEnvelope envelope{
        .target_world_id = "starter-b",
        .target_owner_instance_id = "server-2",
        .preserve_room = true,
        .payload_kind = "chat-room",
        .payload_ref = "opaque-ref",
    };
    server::core::mmorpg::ObservedWorldMigrationWorld target_world{
        .world_id = "starter-b",
        .current_owner_instance_id = "server-2",
        .draining = false,
        .instances = {{.instance_id = "server-2", .ready = true}},
    };

    const auto status =
        server::core::mmorpg::evaluate_world_migration(source_world, envelope, target_world);
    EXPECT_EQ(status.phase, server::core::mmorpg::WorldMigrationPhase::kReadyToResume);
    EXPECT_TRUE(status.summary.envelope_present);
    EXPECT_TRUE(status.summary.source_draining);
    EXPECT_TRUE(status.summary.target_owner_present);
    EXPECT_TRUE(status.summary.target_owner_ready);
    EXPECT_TRUE(status.summary.target_owner_matches_target_world_owner);
    EXPECT_TRUE(status.summary.preserve_room);
}

TEST(MmorpgMigrationContractTest, ReportsAwaitingDrainWhenTargetIsReadyButSourceIsNotDraining) {
    server::core::mmorpg::ObservedWorldMigrationWorld source_world{
        .world_id = "starter-a",
        .current_owner_instance_id = "server-1",
        .draining = false,
    };
    server::core::mmorpg::WorldMigrationEnvelope envelope{
        .target_world_id = "starter-b",
        .target_owner_instance_id = "server-2",
    };
    server::core::mmorpg::ObservedWorldMigrationWorld target_world{
        .world_id = "starter-b",
        .current_owner_instance_id = "server-2",
        .instances = {{.instance_id = "server-2", .ready = true}},
    };

    const auto status =
        server::core::mmorpg::evaluate_world_migration(source_world, envelope, target_world);
    EXPECT_EQ(status.phase, server::core::mmorpg::WorldMigrationPhase::kAwaitingSourceDrain);
}

TEST(MmorpgMigrationContractTest, ReportsMissingTargetWorldOrOwnerStates) {
    server::core::mmorpg::ObservedWorldMigrationWorld source_world{
        .world_id = "starter-a",
        .current_owner_instance_id = "server-1",
        .draining = true,
    };
    server::core::mmorpg::WorldMigrationEnvelope envelope{
        .target_world_id = "starter-b",
        .target_owner_instance_id = "server-2",
    };

    const auto missing_world =
        server::core::mmorpg::evaluate_world_migration(source_world, envelope, std::nullopt);
    EXPECT_EQ(missing_world.phase, server::core::mmorpg::WorldMigrationPhase::kTargetWorldMissing);

    server::core::mmorpg::ObservedWorldMigrationWorld missing_owner_world{
        .world_id = "starter-b",
        .current_owner_instance_id = "server-3",
        .instances = {{.instance_id = "server-3", .ready = true}},
    };
    const auto missing_owner =
        server::core::mmorpg::evaluate_world_migration(source_world, envelope, missing_owner_world);
    EXPECT_EQ(missing_owner.phase, server::core::mmorpg::WorldMigrationPhase::kTargetOwnerMissing);

    server::core::mmorpg::ObservedWorldMigrationWorld unready_owner_world{
        .world_id = "starter-b",
        .current_owner_instance_id = "server-2",
        .instances = {{.instance_id = "server-2", .ready = false}},
    };
    const auto unready_owner =
        server::core::mmorpg::evaluate_world_migration(source_world, envelope, unready_owner_world);
    EXPECT_EQ(unready_owner.phase, server::core::mmorpg::WorldMigrationPhase::kTargetOwnerNotReady);
}
