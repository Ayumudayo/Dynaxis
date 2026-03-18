#include <type_traits>

#include <gtest/gtest.h>

#include <server/core/mmorpg/migration.hpp>
#include <server/core/mmorpg/topology.hpp>
#include <server/core/mmorpg/world_drain.hpp>
#include <server/core/mmorpg/world_transfer.hpp>

TEST(MmorpgCompatHeadersTest, LegacyWrappersAliasCanonicalWorldsSurface) {
    static_assert(std::is_same_v<
        server::core::mmorpg::DesiredTopologyDocument,
        server::core::worlds::DesiredTopologyDocument>);
    static_assert(std::is_same_v<
        server::core::mmorpg::WorldMigrationEnvelope,
        server::core::worlds::WorldMigrationEnvelope>);
    static_assert(std::is_same_v<
        server::core::mmorpg::WorldDrainStatus,
        server::core::worlds::WorldDrainStatus>);
    static_assert(std::is_same_v<
        server::core::mmorpg::WorldTransferStatus,
        server::core::worlds::WorldTransferStatus>);

    const auto world_transfer = server::core::mmorpg::evaluate_world_transfer(
        server::core::mmorpg::ObservedWorldTransferState{
            .world_id = "starter-a",
            .owner_instance_id = "server-1",
            .draining = true,
            .replacement_owner_instance_id = "server-2",
            .instances = {
                {.instance_id = "server-1", .ready = true},
                {.instance_id = "server-2", .ready = true},
            },
        });

    EXPECT_EQ(world_transfer.phase, server::core::worlds::WorldTransferPhase::kAwaitingOwnerHandoff);

    const auto topology_reconciliation = server::core::mmorpg::reconcile_topology(
        server::core::mmorpg::DesiredTopologyDocument{
            .topology_id = "compat-topology",
            .revision = 1,
            .pools = {{.world_id = "starter-a", .shard = "alpha", .replicas = 1}},
        },
        std::vector<server::core::mmorpg::ObservedTopologyPool>{
            {.world_id = "starter-a", .shard = "alpha", .instances = 1, .ready_instances = 1},
        });

    ASSERT_EQ(topology_reconciliation.pools.size(), 1u);
    EXPECT_EQ(topology_reconciliation.pools.front().status, server::core::worlds::TopologyPoolStatus::kAligned);
}

