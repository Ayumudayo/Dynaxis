#include "chat_message_delivery.hpp"

#include "chat_service_private_access.hpp"
#include "chat_service_state.hpp"

#include "server/core/storage/redis/client.hpp"
#include "server/core/protocol/protocol_flags.hpp"
#include "server/core/util/log.hpp"
#include "server/protocol/game_opcodes.hpp"
#include "server/storage/connection_pool.hpp"
#include "server/storage/unit_of_work.hpp"

#include <atomic>
#include <chrono>
#include <utility>

namespace proto = server::core::protocol;
namespace game_proto = server::protocol;
namespace corelog = server::core::log;

namespace server::app::chat {

PreparedChatBroadcast prepare_chat_broadcast(std::string_view room,
                                             std::string_view sender,
                                             std::string_view text,
                                             std::uint32_t sender_session_id) {
    PreparedChatBroadcast prepared;
    prepared.ts_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    server::wire::v1::ChatBroadcast broadcast;
    broadcast.set_room(std::string(room));
    broadcast.set_sender(std::string(sender));
    broadcast.set_text(std::string(text));
    broadcast.set_sender_sid(sender_session_id);
    broadcast.set_ts_ms(prepared.ts_ms);
    broadcast.SerializeToString(&prepared.serialized_payload);

    prepared.body.assign(prepared.serialized_payload.begin(), prepared.serialized_payload.end());
    prepared.snapshot_message.set_sender(std::string(sender));
    prepared.snapshot_message.set_text(std::string(text));
    prepared.snapshot_message.set_ts_ms(prepared.ts_ms);
    return prepared;
}

void send_local_chat_broadcast(
    const std::shared_ptr<server::core::net::Session>& sender_session,
    const std::vector<std::shared_ptr<server::core::net::Session>>& targets,
    const std::vector<std::uint8_t>& body) {
    if (targets.empty()) {
        sender_session->async_send(game_proto::MSG_CHAT_BROADCAST, body, proto::FLAG_SELF);
        return;
    }

    for (const auto& target : targets) {
        const auto flags = (target.get() == sender_session.get()) ? proto::FLAG_SELF : 0;
        target->async_send(game_proto::MSG_CHAT_BROADCAST, body, flags);
    }
}

std::vector<std::shared_ptr<server::core::net::Session>>
ChatServicePrivateAccess::collect_chat_broadcast_targets(ChatService& service,
                                                         std::string_view room_name,
                                                         std::string_view sender) {
    std::vector<std::shared_ptr<server::core::net::Session>> targets;
    std::lock_guard<std::mutex> lk(service.impl_->state.mu);

    const auto room_it = service.impl_->state.rooms.find(std::string(room_name));
    if (room_it == service.impl_->state.rooms.end()) {
        return targets;
    }

    collect_room_sessions(room_it->second, targets);

    std::vector<std::shared_ptr<server::core::net::Session>> filtered_targets;
    filtered_targets.reserve(targets.size());
    for (auto& target : targets) {
        const auto receiver_it = service.impl_->state.user.find(target.get());
        if (receiver_it == service.impl_->state.user.end()) {
            continue;
        }

        const std::string& receiver = receiver_it->second;
        if (const auto blk_it = service.impl_->state.user_blacklists.find(receiver);
            blk_it != service.impl_->state.user_blacklists.end() &&
            blk_it->second.count(std::string(sender)) > 0) {
            continue;
        }

        filtered_targets.push_back(target);
    }
    return filtered_targets;
}

std::optional<std::string> ChatServicePrivateAccess::lookup_user_uuid(
    ChatService& service,
    const server::core::net::Session& session) {
    std::lock_guard<std::mutex> lk(service.impl_->state.mu);
    const auto it = service.impl_->state.user_uuid.find(
        const_cast<server::core::net::Session*>(&session));
    if (it == service.impl_->state.user_uuid.end()) {
        return std::nullopt;
    }
    return it->second;
}

ChatServicePersistedMessage ChatServicePrivateAccess::persist_room_message(
    ChatService& service,
    const server::core::net::Session& session,
    const std::string& room_name,
    const std::string& text) {
    ChatServicePersistedMessage persisted;
    if (!service.impl_->runtime.db_pool) {
        return persisted;
    }

    try {
        persisted.room_id = service.ensure_room_id_ci(room_name);
        if (persisted.room_id.empty()) {
            return persisted;
        }

        const auto user_uuid = lookup_user_uuid(service, session);
        auto uow = service.impl_->runtime.db_pool->make_repository_unit_of_work();
        auto msg = uow->messages().create(persisted.room_id, room_name, user_uuid, text);
        persisted.message_id = msg.id;
        uow->commit();
    } catch (const std::exception& e) {
        corelog::error(std::string("Failed to persist message: ") + e.what());
    }

    return persisted;
}

bool ChatServicePrivateAccess::touch_session_presence(ChatService& service,
                                                      const server::core::net::Session& session) {
    if (!service.impl_->runtime.redis) {
        return false;
    }

    const auto user_uuid = lookup_user_uuid(service, session);
    if (!user_uuid.has_value()) {
        return false;
    }

    try {
        service.touch_user_presence(*user_uuid);
        return true;
    } catch (...) {
        return false;
    }
}

bool ChatServicePrivateAccess::publish_room_fanout(ChatService& service,
                                                   const std::string& room_name,
                                                   std::string_view serialized_payload) {
    if (!service.impl_->runtime.redis || !service.pubsub_enabled()) {
        return false;
    }

    try {
        static std::atomic<std::uint64_t> publish_total{0};
        std::string channel = service.impl_->presence.prefix + std::string("fanout:room:") + room_name;
        std::string message;
        message.reserve(3 + service.impl_->runtime.gateway_id.size() + serialized_payload.size());
        message.append("gw=").append(service.impl_->runtime.gateway_id);
        message.push_back('\n');
        message.append(serialized_payload);
        const bool published = service.impl_->runtime.redis->publish(channel, std::move(message));
        if (published) {
            const auto publish_count = ++publish_total;
            if ((publish_count & 1023ULL) == 0) {
                corelog::debug(
                    std::string("metric=publish_total value=") + std::to_string(publish_count) +
                    " room=" + room_name);
            }
        }
        return published;
    } catch (...) {
        return false;
    }
}

bool ChatServicePrivateAccess::update_membership_last_seen(
    ChatService& service,
    const server::core::net::Session& session,
    const std::string& room_id,
    std::uint64_t message_id) {
    if (!service.impl_->runtime.db_pool || room_id.empty() || message_id == 0) {
        return false;
    }

    const auto user_uuid = lookup_user_uuid(service, session);
    if (!user_uuid.has_value() || user_uuid->empty()) {
        return false;
    }

    try {
        auto uow = service.impl_->runtime.db_pool->make_repository_unit_of_work();
        uow->memberships().update_last_seen(*user_uuid, room_id, message_id);
        uow->commit();
        return true;
    } catch (...) {
        return false;
    }
}

void ChatService::dispatch_room_message(std::shared_ptr<Session> session_sp,
                                        const std::string& current_room,
                                        const std::string& sender,
                                        const std::string& text) {
    auto targets = ChatServicePrivateAccess::collect_chat_broadcast_targets(
        *this,
        current_room,
        sender);
    auto prepared = prepare_chat_broadcast(
        current_room,
        sender,
        text,
        session_sp->session_id());

    const auto persisted = ChatServicePrivateAccess::persist_room_message(
        *this,
        *session_sp,
        current_room,
        text);

    if (impl_->runtime.redis && !persisted.room_id.empty() && persisted.message_id != 0) {
        prepared.snapshot_message.set_id(persisted.message_id);
        if (!ChatServicePrivateAccess::cache_recent_message(
                *this,
                persisted.room_id,
                prepared.snapshot_message)) {
            corelog::warn(std::string("Redis recent history update failed for room_id=") + persisted.room_id);
        }
    } else {
        if (!impl_->runtime.redis) {
            corelog::warn("Redis not available for caching");
        }
        if (persisted.room_id.empty()) {
            corelog::warn("Room ID not found for caching");
        }
        if (persisted.message_id == 0) {
            corelog::warn("Message ID not generated (DB persist failed?)");
        }
    }

    send_local_chat_broadcast(session_sp, targets, prepared.body);
    (void)ChatServicePrivateAccess::touch_session_presence(*this, *session_sp);
    (void)ChatServicePrivateAccess::publish_room_fanout(
        *this,
        current_room,
        prepared.serialized_payload);
    (void)ChatServicePrivateAccess::update_membership_last_seen(
        *this,
        *session_sp,
        persisted.room_id,
        persisted.message_id);
}

} // namespace server::app::chat
