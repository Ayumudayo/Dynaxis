#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace server::core { class Session; }

namespace server::app::fps {

class FixedStepDriver {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;

    FixedStepDriver(std::uint32_t tick_rate_hz, std::size_t max_catch_up_steps = 4);

    std::size_t consume_elapsed(Duration elapsed);
    Duration step_duration() const noexcept { return step_duration_; }
    std::size_t max_catch_up_steps() const noexcept { return max_catch_up_steps_; }
    void clear() noexcept { accumulator_ = Duration::zero(); }

private:
    Duration step_duration_;
    Duration accumulator_{Duration::zero()};
    std::size_t max_catch_up_steps_{4};
};

struct RuntimeConfig {
    std::uint32_t tick_rate_hz{30};
    std::uint32_t snapshot_refresh_ticks{30};
    std::int32_t interest_cell_size_mm{10'000};
    std::int32_t interest_radius_cells{1};
    std::uint32_t max_interest_recipients_per_tick{64};
    std::uint32_t history_ticks{64};
};

struct InputCommand {
    std::uint32_t input_seq{0};
    std::int32_t move_x_mm{0};
    std::int32_t move_y_mm{0};
    std::int32_t yaw_mdeg{0};
};

struct ActorTransformSample {
    std::uint32_t actor_id{0};
    std::int32_t x_mm{0};
    std::int32_t y_mm{0};
    std::int32_t yaw_mdeg{0};
    std::uint32_t last_applied_input_seq{0};
    std::uint32_t server_tick{0};
};

struct RuntimeSnapshot {
    std::uint32_t server_tick{0};
    std::uint64_t tick_total{0};
    std::uint64_t snapshot_total{0};
    std::uint64_t delta_total{0};
    std::size_t actor_count{0};
};

struct OutboundMessage {
    std::uint32_t session_id{0};
    std::uint16_t msg_id{0};
    std::vector<std::uint8_t> payload;
};

class WorldRuntime {
public:
    explicit WorldRuntime(RuntimeConfig config = {});

    bool stage_input(std::uint32_t session_id, const InputCommand& input);
    void remove_session(std::uint32_t session_id);
    std::vector<OutboundMessage> tick();

    std::optional<ActorTransformSample> latest_history_at_or_before(
        std::uint32_t actor_id,
        std::uint32_t server_tick) const;
    std::optional<std::uint32_t> actor_id_for_session(std::uint32_t session_id) const;
    RuntimeSnapshot snapshot() const;

private:
    struct StagedInput {
        InputCommand input;
        bool present{false};
    };

    struct ActorState {
        std::uint32_t actor_id{0};
        std::uint32_t session_id{0};
        std::int32_t x_mm{0};
        std::int32_t y_mm{0};
        std::int32_t yaw_mdeg{0};
        std::uint32_t last_applied_input_seq{0};
        std::int32_t cell_x{0};
        std::int32_t cell_y{0};
        bool dirty{false};
        std::deque<ActorTransformSample> history;
    };

    struct ViewerState {
        std::optional<std::uint32_t> actor_id;
        std::set<std::uint32_t> visible_actor_ids;
        bool snapshot_pending{false};
        std::uint32_t last_snapshot_tick{0};
    };

    RuntimeConfig sanitize_config(RuntimeConfig config) const;
    std::uint32_t ensure_actor_for_session(std::uint32_t session_id);
    void update_actor_cells(ActorState& actor) const;

    mutable std::mutex mu_;
    RuntimeConfig config_{};
    std::uint32_t next_actor_id_{1};
    std::uint32_t server_tick_{0};
    std::uint64_t tick_total_{0};
    std::uint64_t snapshot_total_{0};
    std::uint64_t delta_total_{0};
    std::unordered_map<std::uint32_t, StagedInput> staged_inputs_;
    std::unordered_map<std::uint32_t, std::uint32_t> session_to_actor_;
    std::unordered_map<std::uint32_t, ActorState> actors_;
    std::unordered_map<std::uint32_t, ViewerState> viewers_;
    std::set<std::uint32_t> removed_actor_ids_;
};

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
        return world_.latest_history_at_or_before(actor_id, server_tick);
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
