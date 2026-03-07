#pragma once

#include "scenario_types.hpp"

namespace loadgen {

RunSummary run_scenario(const ScenarioConfig& scenario, const CliOptions& cli);

}  // namespace loadgen
