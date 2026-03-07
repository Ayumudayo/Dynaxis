#pragma once

#include "scenario_types.hpp"

namespace loadgen {

CliOptions parse_cli(int argc, char** argv);
ScenarioConfig load_scenario(const std::filesystem::path& path);

}  // namespace loadgen
