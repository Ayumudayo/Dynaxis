#pragma once

#include <cstddef>
#include <cstdint>

namespace server::core::realtime {

/**
 * @brief 한 authoritative tick 안에서 runtime이 외부에 드러내는 결정론적 phase 순서입니다.
 *
 * 이 enum은 게임 규칙을 모델링하지 않습니다. 목적은 fixed-step runtime이 어떤 순서로
 * 입력 적용, actor 전진, replication 계산을 수행했는지 관측 가능한 어휘로 고정하는 것입니다.
 */
enum class SimulationPhase : std::uint8_t {
    /** @brief 새로운 authoritative tick을 시작했습니다. */
    kTickBegin = 0,
    /** @brief staged input를 authoritative actor 상태에 반영했습니다. */
    kInputsApplied,
    /** @brief actor transform/history 전진이 끝났습니다. */
    kActorsAdvanced,
    /** @brief viewer별 replication update 계산이 끝났습니다. */
    kReplicationComputed,
    /** @brief authoritative tick의 모든 내부 처리가 끝났습니다. */
    kTickEnd,
};

/**
 * @brief 특정 simulation phase 시점의 경량 runtime 관측 값입니다.
 *
 * 이 구조체는 mutable 내부 컨테이너를 노출하지 않고, observer가 tick 진행 상황을
 * 안정적으로 기록할 수 있도록 count 기반의 요약 값만 전달합니다.
 */
struct SimulationPhaseContext {
    std::uint32_t server_tick{0};
    std::size_t actor_count{0};
    std::size_t viewer_count{0};
    std::size_t staged_input_count{0};
    std::size_t replication_update_count{0};
};

/**
 * @brief `WorldRuntime`가 fixed-step phase 진행을 외부에 알릴 때 사용하는 선택적 observer 계약입니다.
 *
 * observer는 관측용 contract입니다. callback에서 gameplay rule을 수행하거나 runtime 내부 lock을
 * 기대하면 안 되며, phase/context를 기록하거나 app-local orchestration seam으로 전달하는 용도로만 씁니다.
 */
class ISimulationPhaseObserver {
public:
    virtual ~ISimulationPhaseObserver() = default;

    /**
     * @brief runtime이 한 phase를 마칠 때 호출됩니다.
     * @param phase 방금 완료된 authoritative simulation phase
     * @param context phase 시점에 캡처한 경량 runtime 요약 값
     */
    virtual void on_simulation_phase(SimulationPhase phase, const SimulationPhaseContext& context) = 0;
};

} // namespace server::core::realtime
