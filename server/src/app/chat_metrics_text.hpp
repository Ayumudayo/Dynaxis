#pragma once

#include "server_runtime_state.hpp"
#include "server/chat/chat_metrics.hpp"

#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace server::app {

struct MetricsTextWriter {
    std::ostream& stream;

    void append_counter(const char* name, std::uint64_t value) const;
    void append_gauge(const char* name, long double value) const;
    void append_labeled_counter(const char* name, std::string_view labels, std::uint64_t value) const;
    void append_labeled_gauge(const char* name, std::string_view labels, long double value) const;
};

std::string escape_prometheus_label_value(std::string_view value);

void append_chat_bootstrap_metrics(MetricsTextWriter& writer, const BootstrapMetricsSnapshot& snapshot);
void append_chat_continuity_metrics(MetricsTextWriter& writer,
                                    std::ostream& stream,
                                    const std::optional<server::app::chat::ContinuityMetrics>& snapshot);
void append_chat_hook_plugin_metrics(std::ostream& stream,
                                     const std::optional<server::app::chat::ChatHookPluginsMetrics>& snapshot);
void append_chat_lua_hook_metrics(std::ostream& stream,
                                  const std::optional<server::app::chat::LuaHooksMetrics>& snapshot);

} // namespace server::app
