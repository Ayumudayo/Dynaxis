#pragma once

#include "chat_service_state.hpp"

#include "wire.pb.h"

#include <memory>
#include <string_view>
#include <vector>

namespace server::app::chat {



struct ChatServicePersistedMessage {
    std::string room_id;
    std::uint64_t message_id{0};
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

    static std::vector<std::shared_ptr<server::core::net::Session>> collect_chat_broadcast_targets(
        ChatService& service,
        std::string_view room_name,
        std::string_view sender);

    static std::optional<std::string> lookup_user_uuid(
        ChatService& service,
        const server::core::net::Session& session);

    static ChatServicePersistedMessage persist_room_message(
        ChatService& service,
        const server::core::net::Session& session,
        const std::string& room_name,
        const std::string& text);

    static bool touch_session_presence(
        ChatService& service,
        const server::core::net::Session& session);

    static bool publish_room_fanout(
        ChatService& service,
        const std::string& room_name,
        std::string_view serialized_payload);

    static bool update_membership_last_seen(
        ChatService& service,
        const server::core::net::Session& session,
        const std::string& room_id,
        std::uint64_t message_id);

    static std::optional<ChatService::ContinuityLease> try_resume_continuity_lease(
        ChatService& service,
        std::string_view token);

    static std::optional<std::string> lookup_room_owner(
        ChatService& service,
        std::string_view room_name);
};

} // namespace server::app::chat
