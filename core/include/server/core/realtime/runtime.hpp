#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace server::core::realtime {

/** @brief authoritative engine tick의 catch-up 작업량을 상한 안에 묶는 fixed-step 누산기입니다. */
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

/** @brief fixed-step tick, interest, delta, snapshot, history 동작을 조정하는 정적 설정값입니다. */
struct RuntimeConfig {
    std::uint32_t tick_rate_hz{30};
    std::uint32_t snapshot_refresh_ticks{30};
    std::int32_t interest_cell_size_mm{10'000};
    std::int32_t interest_radius_cells{1};
    std::uint32_t max_interest_recipients_per_tick{64};
    std::uint32_t max_delta_actors_per_tick{32};
    std::uint32_t history_ticks{64};
};

/** @brief 다음 authoritative tick에 적용할 순서 번호 기반 입력 샘플입니다. */
struct InputCommand {
    std::uint32_t input_seq{0};
    std::int32_t move_x_mm{0};
    std::int32_t move_y_mm{0};
    std::int32_t yaw_mdeg{0};
};

/** @brief replication과 rewind 조회를 위해 보관하는 authoritative actor transform 샘플입니다. */
struct ActorTransformSample {
    std::uint32_t actor_id{0};
    std::int32_t x_mm{0};
    std::int32_t y_mm{0};
    std::int32_t yaw_mdeg{0};
    std::uint32_t last_applied_input_seq{0};
    std::uint32_t server_tick{0};
};

/** @brief 검증과 public consumer가 읽는 경량 runtime 카운터 스냅샷입니다. */
struct RuntimeSnapshot {
    std::uint32_t server_tick{0};
    std::uint64_t tick_total{0};
    std::uint64_t snapshot_total{0};
    std::uint64_t delta_total{0};
    std::uint64_t delta_budget_snapshot_total{0};
    std::uint64_t stale_input_reject_total{0};
    std::size_t actor_count{0};
};

/** @brief 입력을 다음 fixed-step tick에 적재했을 때의 결과 분류입니다. */
enum class StageInputDisposition : std::uint8_t {
    kAccepted = 0,
    kRejectedStale = 1,
};

/** @brief tick 정렬 입력 적재 결과입니다. */
struct StageInputResult {
    StageInputDisposition disposition{StageInputDisposition::kAccepted};
    std::uint32_t target_server_tick{0};
    std::optional<std::uint32_t> actor_id;
};

/** @brief app-local wire/protocol 인코딩 전의 replication payload 종류입니다. */
enum class ReplicationKind : std::uint8_t {
    kSnapshot = 0,
    kDelta = 1,
};

/** @brief 한 viewer를 대상으로 계산한 runtime replication 결과 한 건입니다. */
struct ReplicationUpdate {
    std::uint32_t session_id{0};
    ReplicationKind kind{ReplicationKind::kSnapshot};
    std::uint32_t server_tick{0};
    std::uint32_t self_actor_id{0};
    std::vector<ActorTransformSample> actors;
    std::vector<std::uint32_t> removed_actor_ids;
};

/** @brief rewind 또는 lag-compensation에 쓰는 명시적 조회 요청입니다. */
struct RewindQuery {
    std::uint32_t actor_id{0};
    std::uint32_t server_tick{0};
};

/** @brief 지정한 tick 이하에서 authoritative history를 샘플링한 결과입니다. */
struct RewindResult {
    ActorTransformSample sample;
    bool exact_tick{false};
};

/**
 * @brief authoritative per-tick world state, replication fanout, rewind history를 함께 소유하는 runtime입니다.
 *
 * 이 타입은 session/opcode/wire format을 모르는 엔진 중립적 substrate입니다. 입력 적재,
 * tick 진행, replication 계산, rewind 조회만 책임지고, 실제 프로토콜 해석은 앱 계층에 남깁니다.
 */
class WorldRuntime {
public:
    explicit WorldRuntime(RuntimeConfig config = {});

    StageInputResult stage_input(std::uint32_t session_id, const InputCommand& input);
    void remove_session(std::uint32_t session_id);
    std::vector<ReplicationUpdate> tick();

    std::optional<RewindResult> rewind_at_or_before(const RewindQuery& query) const;
    std::optional<std::uint32_t> actor_id_for_session(std::uint32_t session_id) const;
    RuntimeSnapshot snapshot() const;
    RuntimeConfig config() const;

private:
    /** @brief 다음 tick에 적용할 세션별 staged input 슬롯입니다. */
    struct StagedInput {
        InputCommand input;
        bool present{false};
    };

    /** @brief authoritative transform, history, dirty 상태를 보관하는 actor 슬롯입니다. */
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

    /** @brief viewer별 가시 actor 집합과 snapshot cadence를 추적합니다. */
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
    std::uint64_t delta_budget_snapshot_total_{0};
    std::uint64_t stale_input_reject_total_{0};
    std::unordered_map<std::uint32_t, StagedInput> staged_inputs_;
    std::unordered_map<std::uint32_t, std::uint32_t> session_to_actor_;
    std::unordered_map<std::uint32_t, ActorState> actors_;
    std::unordered_map<std::uint32_t, ViewerState> viewers_;
    std::set<std::uint32_t> removed_actor_ids_;
};

} // namespace server::core::realtime
