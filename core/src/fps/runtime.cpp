#include "server/core/fps/runtime.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace server::core::fps {

namespace {

std::int32_t clamp_i32(std::int64_t value) {
    if (value < static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min())) {
        return std::numeric_limits<std::int32_t>::min();
    }
    if (value > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())) {
        return std::numeric_limits<std::int32_t>::max();
    }
    return static_cast<std::int32_t>(value);
}

std::int32_t cell_coord(std::int32_t value_mm, std::int32_t cell_size_mm) {
    if (cell_size_mm <= 0) {
        return 0;
    }
    if (value_mm >= 0) {
        return value_mm / cell_size_mm;
    }
    return -(((-value_mm) + cell_size_mm - 1) / cell_size_mm);
}

} // namespace

FixedStepDriver::FixedStepDriver(std::uint32_t tick_rate_hz, std::size_t max_catch_up_steps)
    : step_duration_(std::chrono::nanoseconds(
          1'000'000'000LL / static_cast<long long>(std::max<std::uint32_t>(1, tick_rate_hz))))
    , max_catch_up_steps_(std::max<std::size_t>(1, max_catch_up_steps)) {
}

std::size_t FixedStepDriver::consume_elapsed(Duration elapsed) {
    if (elapsed <= Duration::zero()) {
        return 0;
    }

    accumulator_ += elapsed;
    const auto max_accumulator = step_duration_ * static_cast<long long>(max_catch_up_steps_);
    if (accumulator_ > max_accumulator) {
        accumulator_ = max_accumulator;
    }

    std::size_t steps = 0;
    while (accumulator_ >= step_duration_ && steps < max_catch_up_steps_) {
        accumulator_ -= step_duration_;
        ++steps;
    }
    return steps;
}

WorldRuntime::WorldRuntime(RuntimeConfig config)
    : config_(sanitize_config(config)) {
}

RuntimeConfig WorldRuntime::sanitize_config(RuntimeConfig config) const {
    if (config.tick_rate_hz == 0) {
        config.tick_rate_hz = 30;
    }
    if (config.snapshot_refresh_ticks == 0) {
        config.snapshot_refresh_ticks = 30;
    }
    if (config.interest_cell_size_mm <= 0) {
        config.interest_cell_size_mm = 10'000;
    }
    if (config.interest_radius_cells < 0) {
        config.interest_radius_cells = 0;
    }
    if (config.max_interest_recipients_per_tick == 0) {
        config.max_interest_recipients_per_tick = 64;
    }
    if (config.max_delta_actors_per_tick == 0) {
        config.max_delta_actors_per_tick = 32;
    }
    if (config.history_ticks == 0) {
        config.history_ticks = 64;
    }
    return config;
}

void WorldRuntime::update_actor_cells(ActorState& actor) const {
    actor.cell_x = cell_coord(actor.x_mm, config_.interest_cell_size_mm);
    actor.cell_y = cell_coord(actor.y_mm, config_.interest_cell_size_mm);
}

std::uint32_t WorldRuntime::ensure_actor_for_session(std::uint32_t session_id) {
    if (const auto existing = session_to_actor_.find(session_id); existing != session_to_actor_.end()) {
        return existing->second;
    }

    const auto actor_id = next_actor_id_++;
    ActorState actor;
    actor.actor_id = actor_id;
    actor.session_id = session_id;
    update_actor_cells(actor);
    actor.dirty = true;
    session_to_actor_[session_id] = actor_id;
    actors_.emplace(actor_id, std::move(actor));

    auto& viewer = viewers_[session_id];
    viewer.actor_id = actor_id;
    viewer.snapshot_pending = true;
    return actor_id;
}

StageInputResult WorldRuntime::stage_input(std::uint32_t session_id, const InputCommand& input) {
    std::lock_guard<std::mutex> lock(mu_);

    const auto actor_id = ensure_actor_for_session(session_id);
    auto& staged = staged_inputs_[session_id];
    if (staged.present && input.input_seq < staged.input.input_seq) {
        ++stale_input_reject_total_;
        return StageInputResult{
            .disposition = StageInputDisposition::kRejectedStale,
            .target_server_tick = server_tick_ + 1,
            .actor_id = actor_id,
        };
    }

    staged.input = input;
    staged.present = true;
    return StageInputResult{
        .disposition = StageInputDisposition::kAccepted,
        .target_server_tick = server_tick_ + 1,
        .actor_id = actor_id,
    };
}

void WorldRuntime::remove_session(std::uint32_t session_id) {
    std::lock_guard<std::mutex> lock(mu_);
    staged_inputs_.erase(session_id);
    viewers_.erase(session_id);

    const auto actor_it = session_to_actor_.find(session_id);
    if (actor_it == session_to_actor_.end()) {
        return;
    }

    removed_actor_ids_.insert(actor_it->second);
    actors_.erase(actor_it->second);
    session_to_actor_.erase(actor_it);
}

std::vector<ReplicationUpdate> WorldRuntime::tick() {
    std::lock_guard<std::mutex> lock(mu_);

    ++server_tick_;
    ++tick_total_;

    for (auto& [session_id, actor_id] : session_to_actor_) {
        auto actor_it = actors_.find(actor_id);
        if (actor_it == actors_.end()) {
            continue;
        }

        auto& actor = actor_it->second;
        if (const auto staged_it = staged_inputs_.find(session_id);
            staged_it != staged_inputs_.end()
            && staged_it->second.present
            && staged_it->second.input.input_seq > actor.last_applied_input_seq) {
            const auto& input = staged_it->second.input;
            actor.x_mm = clamp_i32(static_cast<std::int64_t>(actor.x_mm) + input.move_x_mm);
            actor.y_mm = clamp_i32(static_cast<std::int64_t>(actor.y_mm) + input.move_y_mm);
            actor.yaw_mdeg = input.yaw_mdeg;
            actor.last_applied_input_seq = input.input_seq;
            actor.dirty = true;
        }

        update_actor_cells(actor);
        actor.history.push_back(ActorTransformSample{
            .actor_id = actor.actor_id,
            .x_mm = actor.x_mm,
            .y_mm = actor.y_mm,
            .yaw_mdeg = actor.yaw_mdeg,
            .last_applied_input_seq = actor.last_applied_input_seq,
            .server_tick = server_tick_,
        });
        while (actor.history.size() > config_.history_ticks) {
            actor.history.pop_front();
        }
    }

    std::vector<ReplicationUpdate> outbound;
    outbound.reserve(viewers_.size());

    for (auto& [session_id, viewer] : viewers_) {
        if (!viewer.actor_id.has_value()) {
            continue;
        }

        const auto viewer_actor_it = actors_.find(*viewer.actor_id);
        if (viewer_actor_it == actors_.end()) {
            continue;
        }

        const auto& viewer_actor = viewer_actor_it->second;
        std::set<std::uint32_t> desired_visible;
        desired_visible.insert(viewer_actor.actor_id);

        for (const auto& [actor_id, actor] : actors_) {
            if (actor_id == viewer_actor.actor_id) {
                continue;
            }
            if (std::abs(actor.cell_x - viewer_actor.cell_x) <= config_.interest_radius_cells
                && std::abs(actor.cell_y - viewer_actor.cell_y) <= config_.interest_radius_cells) {
                desired_visible.insert(actor_id);
            }
        }

        if (desired_visible.size() > config_.max_interest_recipients_per_tick) {
            std::set<std::uint32_t> limited;
            limited.insert(viewer_actor.actor_id);
            for (const auto actor_id : desired_visible) {
                if (limited.size() >= config_.max_interest_recipients_per_tick) {
                    break;
                }
                limited.insert(actor_id);
            }
            desired_visible = std::move(limited);
        }

        std::vector<std::uint32_t> removed_for_viewer;
        for (const auto actor_id : viewer.visible_actor_ids) {
            if (!desired_visible.contains(actor_id)) {
                removed_for_viewer.push_back(actor_id);
            }
        }
        for (const auto actor_id : removed_actor_ids_) {
            if (viewer.visible_actor_ids.contains(actor_id)
                && std::find(removed_for_viewer.begin(), removed_for_viewer.end(), actor_id) == removed_for_viewer.end()) {
                removed_for_viewer.push_back(actor_id);
            }
        }

        if (desired_visible != viewer.visible_actor_ids) {
            viewer.snapshot_pending = true;
        }
        if (viewer.last_snapshot_tick == 0
            || (server_tick_ - viewer.last_snapshot_tick) >= config_.snapshot_refresh_ticks) {
            viewer.snapshot_pending = true;
        }

        std::vector<ActorTransformSample> dirty_samples;
        dirty_samples.reserve(desired_visible.size());
        for (const auto actor_id : desired_visible) {
            const auto actor_it = actors_.find(actor_id);
            if (actor_it == actors_.end() || !actor_it->second.dirty) {
                continue;
            }
            dirty_samples.push_back(ActorTransformSample{
                .actor_id = actor_it->second.actor_id,
                .x_mm = actor_it->second.x_mm,
                .y_mm = actor_it->second.y_mm,
                .yaw_mdeg = actor_it->second.yaw_mdeg,
                .last_applied_input_seq = actor_it->second.last_applied_input_seq,
                .server_tick = server_tick_,
            });
        }

        if (!viewer.snapshot_pending && dirty_samples.size() > config_.max_delta_actors_per_tick) {
            viewer.snapshot_pending = true;
            ++delta_budget_snapshot_total_;
        }

        if (viewer.snapshot_pending) {
            ReplicationUpdate update;
            update.session_id = session_id;
            update.kind = ReplicationKind::kSnapshot;
            update.server_tick = server_tick_;
            update.self_actor_id = viewer_actor.actor_id;
            update.removed_actor_ids = removed_for_viewer;
            update.actors.reserve(desired_visible.size());
            for (const auto actor_id : desired_visible) {
                const auto actor_it = actors_.find(actor_id);
                if (actor_it == actors_.end()) {
                    continue;
                }
                update.actors.push_back(ActorTransformSample{
                    .actor_id = actor_it->second.actor_id,
                    .x_mm = actor_it->second.x_mm,
                    .y_mm = actor_it->second.y_mm,
                    .yaw_mdeg = actor_it->second.yaw_mdeg,
                    .last_applied_input_seq = actor_it->second.last_applied_input_seq,
                    .server_tick = server_tick_,
                });
            }

            outbound.push_back(std::move(update));
            ++snapshot_total_;
            viewer.snapshot_pending = false;
            viewer.last_snapshot_tick = server_tick_;
            viewer.visible_actor_ids = desired_visible;
            continue;
        }

        if (!dirty_samples.empty() || !removed_for_viewer.empty()) {
            outbound.push_back(ReplicationUpdate{
                .session_id = session_id,
                .kind = ReplicationKind::kDelta,
                .server_tick = server_tick_,
                .self_actor_id = viewer_actor.actor_id,
                .actors = std::move(dirty_samples),
                .removed_actor_ids = std::move(removed_for_viewer),
            });
            ++delta_total_;
        }

        viewer.visible_actor_ids = desired_visible;
    }

    for (auto& [actor_id, actor] : actors_) {
        actor.dirty = false;
    }
    removed_actor_ids_.clear();

    return outbound;
}

std::optional<RewindResult> WorldRuntime::rewind_at_or_before(const RewindQuery& query) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto actor_it = actors_.find(query.actor_id);
    if (actor_it == actors_.end()) {
        return std::nullopt;
    }

    const auto& history = actor_it->second.history;
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (it->server_tick <= query.server_tick) {
            return RewindResult{
                .sample = *it,
                .exact_tick = (it->server_tick == query.server_tick),
            };
        }
    }
    return std::nullopt;
}

std::optional<std::uint32_t> WorldRuntime::actor_id_for_session(std::uint32_t session_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = session_to_actor_.find(session_id);
    if (it == session_to_actor_.end()) {
        return std::nullopt;
    }
    return it->second;
}

RuntimeSnapshot WorldRuntime::snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return RuntimeSnapshot{
        .server_tick = server_tick_,
        .tick_total = tick_total_,
        .snapshot_total = snapshot_total_,
        .delta_total = delta_total_,
        .delta_budget_snapshot_total = delta_budget_snapshot_total_,
        .stale_input_reject_total = stale_input_reject_total_,
        .actor_count = actors_.size(),
    };
}

RuntimeConfig WorldRuntime::config() const {
    std::lock_guard<std::mutex> lock(mu_);
    return config_;
}

} // namespace server::core::fps
