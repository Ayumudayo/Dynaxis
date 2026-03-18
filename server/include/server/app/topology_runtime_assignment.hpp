#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "server/core/mmorpg/topology.hpp"

namespace server::app {

inline std::optional<server::core::mmorpg::TopologyActuationRuntimeAssignmentDocument>
parse_topology_actuation_runtime_assignment_document(std::string_view payload) {
    const auto parsed = nlohmann::json::parse(payload, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::nullopt;
    }
    if (!parsed.contains("adapter_id") || !parsed["adapter_id"].is_string()) {
        return std::nullopt;
    }
    if (!parsed.contains("revision")
        || (!parsed["revision"].is_number_unsigned() && !parsed["revision"].is_number_integer())) {
        return std::nullopt;
    }
    if (!parsed.contains("lease_revision")
        || (!parsed["lease_revision"].is_number_unsigned() && !parsed["lease_revision"].is_number_integer())) {
        return std::nullopt;
    }
    if (!parsed.contains("assignments") || !parsed["assignments"].is_array()) {
        return std::nullopt;
    }

    server::core::mmorpg::TopologyActuationRuntimeAssignmentDocument document;
    document.adapter_id = parsed["adapter_id"].get<std::string>();
    document.revision = parsed["revision"].get<std::uint64_t>();
    document.lease_revision = parsed["lease_revision"].get<std::uint64_t>();
    if (parsed.contains("updated_at_ms")
        && (parsed["updated_at_ms"].is_number_unsigned() || parsed["updated_at_ms"].is_number_integer())) {
        document.updated_at_ms = parsed["updated_at_ms"].get<std::uint64_t>();
    }

    for (const auto& item : parsed["assignments"]) {
        if (!item.is_object()) {
            return std::nullopt;
        }
        if (!item.contains("instance_id") || !item["instance_id"].is_string()) {
            return std::nullopt;
        }
        if (!item.contains("world_id") || !item["world_id"].is_string()) {
            return std::nullopt;
        }
        if (!item.contains("shard") || !item["shard"].is_string()) {
            return std::nullopt;
        }
        if (!item.contains("action") || !item["action"].is_string()) {
            return std::nullopt;
        }

        const auto parsed_action = server::core::mmorpg::parse_topology_actuation_action_kind(
            item["action"].get<std::string>());
        if (!parsed_action.has_value()) {
            return std::nullopt;
        }

        document.assignments.push_back({
            .instance_id = item["instance_id"].get<std::string>(),
            .world_id = item["world_id"].get<std::string>(),
            .shard = item["shard"].get<std::string>(),
            .action = *parsed_action,
        });
    }

    return document;
}

inline std::optional<server::core::mmorpg::TopologyActuationRuntimeAssignmentItem>
find_topology_actuation_runtime_assignment_for_instance(
    const server::core::mmorpg::TopologyActuationRuntimeAssignmentDocument& document,
    std::string_view instance_id) {
    if (const auto* assignment =
            server::core::mmorpg::find_topology_actuation_runtime_assignment(document, instance_id);
        assignment != nullptr) {
        return *assignment;
    }
    return std::nullopt;
}

inline std::vector<std::string> apply_topology_runtime_assignment_tags(
    const std::vector<std::string>& base_tags,
    const std::optional<server::core::mmorpg::TopologyActuationRuntimeAssignmentItem>& assignment) {
    if (!assignment.has_value()) {
        return base_tags;
    }

    std::vector<std::string> out;
    out.reserve(base_tags.size() + 1u);

    for (const auto& tag : base_tags) {
        if (tag.rfind("world:", 0) == 0) {
            continue;
        }
        out.push_back(tag);
    }

    if (!assignment->world_id.empty()) {
        out.push_back("world:" + assignment->world_id);
    }

    return out;
}

inline std::string resolve_topology_runtime_assignment_world_id(
    std::string_view base_world_id,
    const std::optional<server::core::mmorpg::TopologyActuationRuntimeAssignmentItem>& assignment) {
    if (assignment.has_value() && !assignment->world_id.empty()) {
        return assignment->world_id;
    }
    return std::string(base_world_id);
}

inline std::string resolve_topology_runtime_assignment_shard(
    std::string_view base_shard,
    const std::optional<server::core::mmorpg::TopologyActuationRuntimeAssignmentItem>& assignment) {
    if (assignment.has_value() && !assignment->shard.empty()) {
        return assignment->shard;
    }
    return std::string(base_shard);
}

} // namespace server::app
