#pragma once

#include "chat_metrics_text.hpp"

#include "server/core/app/engine_runtime.hpp"
#include "server/core/runtime_metrics.hpp"

#include <iosfwd>
#include <optional>
#include <vector>

namespace server::app {

void append_chat_runtime_metrics(MetricsTextWriter& writer,
                                 std::ostream& stream,
                                 const server::core::runtime_metrics::Snapshot& snapshot);

void append_server_runtime_module_metrics(
    MetricsTextWriter& writer,
    std::ostream& stream,
    const std::optional<server::core::app::EngineRuntime::Snapshot>& runtime_snapshot,
    const std::vector<server::core::app::EngineRuntime::ModuleSnapshot>& modules);

} // namespace server::app
