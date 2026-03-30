#include "chat_room_state.hpp"

#include <algorithm>

namespace server::app::chat {

namespace {

void erase_room_metadata_locked(ChatServiceState& state, std::string_view room_name) {
    const std::string room(room_name);
    state.rooms.erase(room);
    state.room_passwords.erase(room);
    state.room_owners.erase(room);
    state.room_invites.erase(room);
}

std::string select_replacement_room_owner_locked(
    const ChatServiceState& state,
    const ChatRoomSet& members) {
    std::string new_owner;
    for (const auto& weak : members) {
        if (auto candidate = weak.lock()) {
            auto user_it = state.user.find(candidate.get());
            if (user_it == state.user.end()) {
                continue;
            }

            new_owner = user_it->second;
            if (new_owner != "guest") {
                break;
            }
        }
    }
    return new_owner;
}

} // namespace

std::vector<std::shared_ptr<server::core::net::Session>> collect_room_targets_locked(
    ChatServiceState& state,
    std::string_view room_name,
    std::string_view sender_name) {
    std::vector<std::shared_ptr<server::core::net::Session>> targets;
    const auto room_it = state.rooms.find(std::string(room_name));
    if (room_it == state.rooms.end()) {
        return targets;
    }

    collect_room_sessions(room_it->second, targets);
    if (sender_name.empty()) {
        return targets;
    }

    std::vector<std::shared_ptr<server::core::net::Session>> filtered_targets;
    filtered_targets.reserve(targets.size());
    for (auto& target : targets) {
        auto receiver_it = state.user.find(target.get());
        if (receiver_it == state.user.end()) {
            continue;
        }
        const std::string& receiver = receiver_it->second;
        if (auto blacklist_it = state.user_blacklists.find(receiver);
            blacklist_it != state.user_blacklists.end()
            && blacklist_it->second.count(std::string(sender_name)) > 0) {
            continue;
        }
        filtered_targets.push_back(std::move(target));
    }
    return filtered_targets;
}

std::vector<std::string> collect_room_user_names_locked(
    ChatServiceState& state,
    std::string_view room_name) {
    std::vector<std::string> users;
    const auto room_it = state.rooms.find(std::string(room_name));
    if (room_it == state.rooms.end()) {
        return users;
    }

    for (auto member_it = room_it->second.begin(); member_it != room_it->second.end();) {
        if (auto session = member_it->lock()) {
            auto user_it = state.user.find(session.get());
            users.push_back(user_it != state.user.end() ? user_it->second : std::string("guest"));
            ++member_it;
        } else {
            member_it = room_it->second.erase(member_it);
        }
    }
    return users;
}

std::vector<std::string> collect_authenticated_room_user_names_locked(
    ChatServiceState& state,
    std::string_view room_name) {
    std::vector<std::string> users;
    const auto room_it = state.rooms.find(std::string(room_name));
    if (room_it == state.rooms.end()) {
        return users;
    }

    for (auto member_it = room_it->second.begin(); member_it != room_it->second.end();) {
        if (auto session = member_it->lock()) {
            if (state.authed.count(session.get()) > 0 && state.guest.count(session.get()) == 0) {
                if (auto user_it = state.user.find(session.get()); user_it != state.user.end()) {
                    users.push_back(user_it->second);
                }
            }
            ++member_it;
        } else {
            member_it = room_it->second.erase(member_it);
        }
    }

    std::sort(users.begin(), users.end());
    users.erase(std::unique(users.begin(), users.end()), users.end());
    return users;
}

std::vector<std::shared_ptr<server::core::net::Session>> collect_authenticated_session_targets_locked(
    ChatServiceState& state) {
    std::vector<std::shared_ptr<server::core::net::Session>> sessions;
    std::unordered_set<server::core::net::Session*> seen;

    for (auto& [_, room_set] : state.rooms) {
        for (auto member_it = room_set.begin(); member_it != room_set.end();) {
            if (auto session = member_it->lock()) {
                if (state.authed.count(session.get()) > 0
                    && state.guest.count(session.get()) == 0
                    && seen.insert(session.get()).second) {
                    sessions.push_back(std::move(session));
                }
                ++member_it;
            } else {
                member_it = room_set.erase(member_it);
            }
        }
    }

    return sessions;
}

std::vector<ChatRoomSummary> collect_local_room_summaries_locked(ChatServiceState& state) {
    std::vector<ChatRoomSummary> rooms;
    rooms.reserve(state.rooms.size());

    for (auto room_it = state.rooms.begin(); room_it != state.rooms.end();) {
        const std::string room_name = room_it->first;
        auto current_it = room_it++;

        std::size_t alive = 0;
        for (auto member_it = current_it->second.begin(); member_it != current_it->second.end();) {
            if (member_it->lock()) {
                ++alive;
                ++member_it;
            } else {
                member_it = current_it->second.erase(member_it);
            }
        }

        if (alive == 0 && room_name != "lobby") {
            erase_room_metadata_locked(state, room_name);
            continue;
        }

        rooms.push_back(ChatRoomSummary{
            room_name,
            alive,
            state.room_passwords.count(room_name) > 0,
        });
    }

    return rooms;
}

std::vector<std::string> collect_sorted_room_names_locked(const ChatServiceState& state) {
    std::vector<std::string> rooms;
    rooms.reserve(state.rooms.size());
    for (const auto& [room_name, _] : state.rooms) {
        rooms.push_back(room_name);
    }
    std::sort(rooms.begin(), rooms.end());
    return rooms;
}

std::size_t count_local_rooms_locked(const ChatServiceState& state) {
    return state.rooms.size();
}

bool room_contains_session_locked(
    ChatServiceState& state,
    std::string_view room_name,
    server::core::net::Session& session) {
    const auto room_it = state.rooms.find(std::string(room_name));
    if (room_it == state.rooms.end()) {
        return false;
    }

    for (auto member_it = room_it->second.begin(); member_it != room_it->second.end();) {
        if (auto member = member_it->lock()) {
            if (member.get() == &session) {
                return true;
            }
            ++member_it;
        } else {
            member_it = room_it->second.erase(member_it);
        }
    }
    return false;
}

bool room_exists_locked(const ChatServiceState& state, std::string_view room_name) {
    return state.rooms.find(std::string(room_name)) != state.rooms.end();
}

bool room_is_locked_locked(const ChatServiceState& state, std::string_view room_name) {
    return state.room_passwords.find(std::string(room_name)) != state.room_passwords.end();
}

std::optional<std::string> find_room_owner_locked(
    const ChatServiceState& state,
    std::string_view room_name) {
    auto owner_it = state.room_owners.find(std::string(room_name));
    if (owner_it == state.room_owners.end()) {
        return std::nullopt;
    }
    return owner_it->second;
}

ChatRoomDepartureResult remove_session_from_room_locked(
    ChatServiceState& state,
    const std::shared_ptr<server::core::net::Session>& session,
    std::string_view room_name,
    std::string_view sender_name) {
    ChatRoomDepartureResult result;
    const std::string room(room_name);
    auto room_it = state.rooms.find(room);
    if (room_it == state.rooms.end()) {
        return result;
    }

    const bool was_owner =
        room != "lobby"
        && [&]() {
            auto owner_it = state.room_owners.find(room);
            return owner_it != state.room_owners.end() && owner_it->second == sender_name;
        }();

    room_it->second.erase(session);
    result.targets = collect_room_targets_locked(state, room, sender_name);

    room_it = state.rooms.find(room);
    if (room_it == state.rooms.end()) {
        return result;
    }

    if (room_it->second.empty() && room != "lobby") {
        erase_room_metadata_locked(state, room);
        result.room_removed = true;
        return result;
    }

    if (was_owner && room != "lobby") {
        auto owner_it = state.room_owners.find(room);
        if (owner_it != state.room_owners.end()) {
            const std::string new_owner = select_replacement_room_owner_locked(state, room_it->second);
            if (!new_owner.empty()) {
                owner_it->second = new_owner;
            }
        }
    }

    return result;
}

void place_session_in_room_locked(
    ChatServiceState& state,
    const std::shared_ptr<server::core::net::Session>& session,
    std::string_view room_name,
    std::string_view sender_name,
    bool consume_invite,
    bool assign_owner) {
    const std::string room(room_name);
    state.cur_room[session.get()] = room;
    state.rooms[room].insert(session);

    if (room == "lobby" || !assign_owner) {
        return;
    }

    if (state.room_owners.find(room) == state.room_owners.end() && sender_name != "guest") {
        state.room_owners[room] = std::string(sender_name);
    }

    if (!consume_invite) {
        return;
    }

    auto invite_it = state.room_invites.find(room);
    if (invite_it == state.room_invites.end()) {
        return;
    }

    invite_it->second.erase(std::string(sender_name));
    if (invite_it->second.empty()) {
        state.room_invites.erase(invite_it);
    }
}

} // namespace server::app::chat
