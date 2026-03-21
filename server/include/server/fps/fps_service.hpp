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

/**
 * @brief 세션 관점에서 FPS 입력을 받아 authoritative runtime으로 넘기고 복제 결과를 내보내는 진입점입니다.
 *
 * 이 타입이 필요한 이유는 wire payload 해석과 `server::core::realtime::WorldRuntime`
 * 자체를 분리하기 위해서입니다. runtime은 엔진 중립적인 fixed-step 상태만 소유하고,
 * 세션/opcode/payload 해석은 여전히 앱 계층이 맡습니다.
 */
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
