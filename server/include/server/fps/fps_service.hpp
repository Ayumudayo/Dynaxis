#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>

#include "server/core/realtime/runtime.hpp"

namespace server::core { class Session; }

namespace server::app::fps {

using FixedStepDriver = server::core::realtime::FixedStepDriver;
using RuntimeConfig = server::core::realtime::RuntimeConfig;
using InputCommand = server::core::realtime::InputCommand;
using ActorTransformSample = server::core::realtime::ActorTransformSample;
using RuntimeSnapshot = server::core::realtime::RuntimeSnapshot;
using StageInputDisposition = server::core::realtime::StageInputDisposition;
using StageInputResult = server::core::realtime::StageInputResult;
using ReplicationKind = server::core::realtime::ReplicationKind;
using ReplicationUpdate = server::core::realtime::ReplicationUpdate;
using RewindQuery = server::core::realtime::RewindQuery;
using RewindResult = server::core::realtime::RewindResult;
using WorldRuntime = server::core::realtime::WorldRuntime;

/** @brief Session-facing FPS entrypoint that decodes input and dispatches replication output. */
class FpsService {
public:
    using Session = server::core::Session;

    explicit FpsService(RuntimeConfig config = {});

    void on_input(Session& session, std::span<const std::uint8_t> payload);
    void on_session_close(std::shared_ptr<Session> session);
    void tick();

    RuntimeSnapshot snapshot() const { return world_.snapshot(); }
    std::optional<ActorTransformSample> latest_history_at_or_before(
        std::uint32_t actor_id,
        std::uint32_t server_tick) const {
        const auto rewind = world_.rewind_at_or_before(RewindQuery{
            .actor_id = actor_id,
            .server_tick = server_tick,
        });
        if (!rewind.has_value()) {
            return std::nullopt;
        }
        return rewind->sample;
    }
    std::optional<std::uint32_t> actor_id_for_session(std::uint32_t session_id) const {
        return world_.actor_id_for_session(session_id);
    }

private:
    mutable std::mutex sessions_mu_;
    std::unordered_map<std::uint32_t, std::weak_ptr<Session>> sessions_;
    WorldRuntime world_;
};

} // namespace server::app::fps
