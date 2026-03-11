#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "server/core/state/instance_registry.hpp"

namespace server::state {

using server::core::state::InstanceRecord;
using server::core::state::InstanceSelector;
using server::core::state::SelectorMatchStats;
using server::core::state::SelectorPolicyLayer;
using server::core::state::matches_selector;
using server::core::state::classify_selector_policy_layer;
using server::core::state::selector_policy_layer_name;
using server::core::state::select_instances;
using server::core::state::IInstanceStateBackend;
using server::core::state::InMemoryStateBackend;

namespace detail {
std::string serialize_json(const InstanceRecord& record);
std::optional<InstanceRecord> deserialize_json(std::string_view payload);
} // namespace detail

} // namespace server::state
