#include <gtest/gtest.h>

#include <server/core/worlds/world_transfer.hpp>

TEST(MmorpgWorldTransferContractTest, ReportsAwaitingOwnerHandoffWhenTargetIsReadyButOwnerUnchanged) {
    server::core::worlds::ObservedWorldTransferState state;
    state.world_id = "starter-a";
    state.owner_instance_id = "server-1";
    state.draining = true;
    state.replacement_owner_instance_id = "server-2";
    state.instances = {
        {.instance_id = "server-1", .ready = true},
        {.instance_id = "server-2", .ready = true},
    };

    const auto status = server::core::worlds::evaluate_world_transfer(state);
    EXPECT_EQ(status.phase, server::core::worlds::WorldTransferPhase::kAwaitingOwnerHandoff);
    EXPECT_TRUE(status.summary.transfer_declared);
    EXPECT_TRUE(status.summary.target_present);
    EXPECT_TRUE(status.summary.target_ready);
    EXPECT_FALSE(status.summary.owner_matches_target);
}

TEST(MmorpgWorldTransferContractTest, ReportsCommittedWhenOwnerMatchesTarget) {
    server::core::worlds::ObservedWorldTransferState state;
    state.world_id = "starter-a";
    state.owner_instance_id = "server-2";
    state.draining = true;
    state.replacement_owner_instance_id = "server-2";
    state.instances = {
        {.instance_id = "server-1", .ready = true},
        {.instance_id = "server-2", .ready = true},
    };

    const auto status = server::core::worlds::evaluate_world_transfer(state);
    EXPECT_EQ(status.phase, server::core::worlds::WorldTransferPhase::kOwnerHandoffCommitted);
    EXPECT_TRUE(status.summary.owner_matches_target);
    EXPECT_EQ(status.summary.instances_total, 2u);
    EXPECT_EQ(status.summary.ready_instances, 2u);
}

TEST(MmorpgWorldTransferContractTest, ReportsBlockedStatesForMissingOrUnreadyTarget) {
    server::core::worlds::ObservedWorldTransferState missing_target;
    missing_target.world_id = "starter-a";
    missing_target.owner_instance_id = "server-1";
    missing_target.draining = true;
    missing_target.replacement_owner_instance_id = "server-2";
    missing_target.instances = {
        {.instance_id = "server-1", .ready = true},
    };

    const auto missing_status = server::core::worlds::evaluate_world_transfer(missing_target);
    EXPECT_EQ(missing_status.phase, server::core::worlds::WorldTransferPhase::kTargetMissing);

    server::core::worlds::ObservedWorldTransferState unready_target = missing_target;
    unready_target.instances.push_back({.instance_id = "server-2", .ready = false});

    const auto unready_status = server::core::worlds::evaluate_world_transfer(unready_target);
    EXPECT_EQ(unready_status.phase, server::core::worlds::WorldTransferPhase::kTargetNotReady);
}
