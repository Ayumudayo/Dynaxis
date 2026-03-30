#pragma once

#include "chat_service_state.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace server::app::chat {

/** @brief room/session 전이 후 남은 로컬 알림 대상을 담는 server-local 결과입니다. */
struct ChatRoomDepartureResult {
    std::vector<std::shared_ptr<server::core::net::Session>> targets;
    bool room_removed{false};
};

/** @brief room 목록 응답/스냅샷에서 쓰는 server-local 요약입니다. */
struct ChatRoomSummary {
    std::string name;
    std::size_t member_count{0};
    bool is_locked{false};
};

std::vector<std::shared_ptr<server::core::net::Session>> collect_room_targets_locked(
    ChatServiceState& state,
    std::string_view room_name,
    std::string_view sender_name = {});

std::vector<std::string> collect_room_user_names_locked(
    ChatServiceState& state,
    std::string_view room_name);

std::vector<std::string> collect_authenticated_room_user_names_locked(
    ChatServiceState& state,
    std::string_view room_name);

std::vector<std::shared_ptr<server::core::net::Session>> collect_authenticated_session_targets_locked(
    ChatServiceState& state);

std::vector<ChatRoomSummary> collect_local_room_summaries_locked(ChatServiceState& state);

std::vector<std::string> collect_sorted_room_names_locked(const ChatServiceState& state);

std::size_t count_local_rooms_locked(const ChatServiceState& state);

bool room_contains_session_locked(
    ChatServiceState& state,
    std::string_view room_name,
    server::core::net::Session& session);

bool room_exists_locked(const ChatServiceState& state, std::string_view room_name);

bool room_is_locked_locked(const ChatServiceState& state, std::string_view room_name);

std::optional<std::string> find_room_owner_locked(
    const ChatServiceState& state,
    std::string_view room_name);

ChatRoomDepartureResult remove_session_from_room_locked(
    ChatServiceState& state,
    const std::shared_ptr<server::core::net::Session>& session,
    std::string_view room_name,
    std::string_view sender_name);

void place_session_in_room_locked(
    ChatServiceState& state,
    const std::shared_ptr<server::core::net::Session>& session,
    std::string_view room_name,
    std::string_view sender_name,
    bool consume_invite = false,
    bool assign_owner = true);

} // namespace server::app::chat
