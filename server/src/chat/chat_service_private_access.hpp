#pragma once

#include "chat_service_state.hpp"

#include "server/core/discovery/world_lifecycle_policy.hpp"
#include "server/core/worlds/migration.hpp"
#include "server/core/worlds/topology.hpp"
#include "wire.pb.h"

namespace server::app::chat {

struct ChatServiceAppMigrationRoomHandoff {
    bool recognized{false};
    std::string room;
};

/**
 * @brief `ChatService`의 비공개 continuity/history seam에 접근하는 server-local helper입니다.
 *
 * 공개 헤더를 오염시키지 않으면서 테스트와 구현 T.U.가 동일한 내부 helper를 재사용하도록
 * friend access 경로를 한곳으로 모읍니다.
 */
struct ChatServicePrivateAccess {
    static void override_history_config(ChatService& service,
                                        std::size_t recent_limit,
                                        std::size_t max_list_len);

    static bool cache_recent_message(
        ChatService& service,
        const std::string& room_id,
        const server::wire::v1::StateSnapshot::SnapshotMessage& message);

    static bool load_recent_messages_from_cache(
        ChatService& service,
        const std::string& room_id,
        std::vector<server::wire::v1::StateSnapshot::SnapshotMessage>& out);

    static std::optional<ChatService::ContinuityLease> try_resume_continuity_lease(
        ChatService& service,
        std::string_view token);

    static std::string make_continuity_world_owner_key(const ChatService& service, const std::string& world_id);
    static std::string make_continuity_world_policy_key(const ChatService& service, const std::string& world_id);
    static std::string make_continuity_world_migration_key(const ChatService& service, const std::string& world_id);

    static std::optional<server::core::discovery::WorldLifecyclePolicy> load_continuity_world_policy(
        ChatService& service,
        const std::string& world_id);

    static std::optional<server::core::worlds::WorldMigrationEnvelope> load_continuity_world_migration(
        ChatService& service,
        const std::string& world_id);

    static std::optional<server::core::worlds::TopologyActuationRuntimeAssignmentItem>
    load_topology_runtime_assignment(const ChatService& service);

    static ChatServiceAppMigrationRoomHandoff resolve_app_world_migration_room_handoff(
        const server::core::worlds::WorldMigrationEnvelope& migration);

    static std::optional<std::string> lookup_room_owner(
        ChatService& service,
        std::string_view room_name);
};

} // namespace server::app::chat
