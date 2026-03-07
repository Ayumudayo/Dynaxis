#pragma once

#include "scenario_types.hpp"

#include <nlohmann/json.hpp>

namespace loadgen {

nlohmann::json to_json(const RunSummary& summary);
void write_report(const std::filesystem::path& report_path, const nlohmann::json& report);
void print_summary(const RunSummary& summary, const std::filesystem::path& report_path);

}  // namespace loadgen
