#include "server/fps/fps_service.hpp"

#include "server/core/net/session.hpp"
#include "server/core/protocol/protocol_errors.hpp"
#include "server/protocol/game_opcodes.hpp"
#include "server/wire/codec.hpp"
#include "wire.pb.h"

#include <algorithm>

namespace server::app::fps {

namespace {

template <typename Message>
std::vector<std::uint8_t> encode_message(const Message& message) {
    return server::wire::codec::Encode(message);
}

std::uint16_t replication_msg_id(const ReplicationUpdate& update) {
    return update.kind == ReplicationKind::kSnapshot
        ? server::protocol::MSG_FPS_STATE_SNAPSHOT
        : server::protocol::MSG_FPS_STATE_DELTA;
}

std::vector<std::uint8_t> encode_replication_payload(const ReplicationUpdate& update) {
    if (update.kind == ReplicationKind::kSnapshot) {
        server::wire::v1::FpsStateSnapshot snapshot_message;
        snapshot_message.set_server_tick(update.server_tick);
        snapshot_message.set_self_actor_id(update.self_actor_id);
        for (const auto& actor : update.actors) {
            auto* actor_proto = snapshot_message.add_actors();
            actor_proto->set_actor_id(actor.actor_id);
            actor_proto->set_x_mm(actor.x_mm);
            actor_proto->set_y_mm(actor.y_mm);
            actor_proto->set_yaw_mdeg(actor.yaw_mdeg);
            actor_proto->set_last_applied_input_seq(actor.last_applied_input_seq);
            actor_proto->set_server_tick(actor.server_tick);
        }
        for (const auto actor_id : update.removed_actor_ids) {
            snapshot_message.add_removed_actor_ids(actor_id);
        }
        return encode_message(snapshot_message);
    }

    server::wire::v1::FpsStateDelta delta_message;
    delta_message.set_server_tick(update.server_tick);
    delta_message.set_self_actor_id(update.self_actor_id);
    for (const auto& actor : update.actors) {
        auto* actor_proto = delta_message.add_actors();
        actor_proto->set_actor_id(actor.actor_id);
        actor_proto->set_x_mm(actor.x_mm);
        actor_proto->set_y_mm(actor.y_mm);
        actor_proto->set_yaw_mdeg(actor.yaw_mdeg);
        actor_proto->set_last_applied_input_seq(actor.last_applied_input_seq);
        actor_proto->set_server_tick(actor.server_tick);
    }
    for (const auto actor_id : update.removed_actor_ids) {
        delta_message.add_removed_actor_ids(actor_id);
    }
    return encode_message(delta_message);
}

} // namespace

FpsService::FpsService(RuntimeConfig config)
    : world_(std::move(config)) {}

void FpsService::on_input(Session& session, std::span<const std::uint8_t> payload) {
    server::wire::v1::FpsInput input_message;
    if (!server::wire::codec::Decode(payload.data(), payload.size(), input_message)) {
        session.send_error(server::core::protocol::errc::INVALID_PAYLOAD, "invalid fps input");
        return;
    }

    try {
        auto shared = session.shared_from_this();
        std::lock_guard<std::mutex> lock(sessions_mu_);
        sessions_[session.session_id()] = shared;
    } catch (...) {
        session.send_error(server::core::protocol::errc::SERVER_BUSY, "fps session unavailable");
        return;
    }

    const auto staged = world_.stage_input(session.session_id(), InputCommand{
        .input_seq = input_message.input_seq(),
        .move_x_mm = input_message.move_x_mm(),
        .move_y_mm = input_message.move_y_mm(),
        .yaw_mdeg = input_message.yaw_mdeg(),
    });
    if (staged.disposition == StageInputDisposition::kRejectedStale) {
        session.send_error(server::core::protocol::errc::FORBIDDEN, "stale fps input");
    }
}

void FpsService::on_session_close(std::shared_ptr<Session> session) {
    if (!session) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(sessions_mu_);
        sessions_.erase(session->session_id());
    }
    world_.remove_session(session->session_id());
}

void FpsService::tick() {
    const auto outbound = world_.tick();

    std::vector<std::uint32_t> stale_sessions;
    for (const auto& update : outbound) {
        std::shared_ptr<Session> session;
        {
            std::lock_guard<std::mutex> lock(sessions_mu_);
            const auto it = sessions_.find(update.session_id);
            if (it != sessions_.end()) {
                session = it->second.lock();
            }
        }

        if (!session) {
            stale_sessions.push_back(update.session_id);
            continue;
        }

        session->async_send(
            replication_msg_id(update),
            encode_replication_payload(update),
            0);
    }

    for (const auto session_id : stale_sessions) {
        world_.remove_session(session_id);
        std::lock_guard<std::mutex> lock(sessions_mu_);
        sessions_.erase(session_id);
    }
}

} // namespace server::app::fps
