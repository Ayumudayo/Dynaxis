#include <gtest/gtest.h>

#include <server/core/mmorpg/world_transfer.hpp>

TEST(MmorpgWorldTransferContractTest, ReportsAwaitingOwnerHandoffWhenTargetIsReadyButOwnerUnchanged) {
    server::core::mmorpg::ObservedWorldTransferState state;
    state.world_id = "starter-a";
    state.owner_instance_id = "server-1";
    state.draining = true;
    state.replacement_owner_instance_id = "server-2";
    state.instances = {
        {.instance_id = "server-1", .ready = true},
        {.instance_id = "server-2", .ready = true},
    };

    const auto status = server::core::mmorpg::evaluate_world_transfer(state);
    EXPECT_EQ(status.phase, server::core::mmorpg::WorldTransferPhase::kAwaitingOwnerHandoff);
    EXPECT_TRUE(status.summary.transfer_declared);
    EXPECT_TRUE(status.summary.target_present);
    EXPECT_TRUE(status.summary.target_ready);
    EXPECT_FALSE(status.summary.owner_matches_target);
}

TEST(MmorpgWorldTransferContractTest, ReportsCommittedWhenOwnerMatchesTarget) {
    server::core::mmorpg::ObservedWorldTransferState state;
    state.world_id = "starter-a";
    state.owner_instance_id = "server-2";
    state.draining = true;
    state.replacement_owner_instance_id = "server-2";
    state.instances = {
        {.instance_id = "server-1", .ready = true},
        {.instance_id = "server-2", .ready = true},
    };

    const auto status = server::core::mmorpg::evaluate_world_transfer(state);
    EXPECT_EQ(status.phase, server::core::mmorpg::WorldTransferPhase::kOwnerHandoffCommitted);
    EXPECT_TRUE(status.summary.owner_matches_target);
    EXPECT_EQ(status.summary.instances_total, 2u);
    EXPECT_EQ(status.summary.ready_instances, 2u);
}

TEST(MmorpgWorldTransferContractTest, ReportsBlockedStatesForMissingOrUnreadyTarget) {
    server::core::mmorpg::ObservedWorldTransferState missing_target;
    missing_target.world_id = "starter-a";
    missing_target.owner_instance_id = "server-1";
    missing_target.draining = true;
    missing_target.replacement_owner_instance_id = "server-2";
    missing_target.instances = {
        {.instance_id = "server-1", .ready = true},
    };

    const auto missing_status = server::core::mmorpg::evaluate_world_transfer(missing_target);
    EXPECT_EQ(missing_status.phase, server::core::mmorpg::WorldTransferPhase::kTargetMissing);

    server::core::mmorpg::ObservedWorldTransferState unready_target = missing_target;
    unready_target.instances.push_back({.instance_id = "server-2", .ready = false});

    const auto unready_status = server::core::mmorpg::evaluate_world_transfer(unready_target);
    EXPECT_EQ(unready_status.phase, server::core::mmorpg::WorldTransferPhase::kTargetNotReady);
}
