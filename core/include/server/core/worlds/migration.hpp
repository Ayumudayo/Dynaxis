#pragma once

#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace server::core::worlds {

/** @brief source world drain 이후 target world resume에 필요한 opaque migration envelope입니다. */
struct WorldMigrationEnvelope {
    std::string target_world_id;
    std::string target_owner_instance_id;
    bool preserve_room{false};
    std::string payload_kind;
    std::string payload_ref;
    std::uint64_t updated_at_ms{0};
};

/** @brief migration target world에서 관측한 instance readiness 한 건입니다. */
struct ObservedWorldMigrationInstance {
    std::string instance_id;
    bool ready{false};
};

/** @brief migration target world의 owner 및 instance readiness 관측값입니다. */
struct ObservedWorldMigrationWorld {
    std::string world_id;
    std::string current_owner_instance_id;
    bool draining{false};
    std::vector<ObservedWorldMigrationInstance> instances;
};

enum class WorldMigrationPhase : std::uint8_t {
    kIdle = 0,
    kTargetWorldMissing,
    kTargetOwnerMissing,
    kTargetOwnerNotReady,
    kAwaitingSourceDrain,
    kReadyToResume,
};

inline constexpr std::string_view world_migration_phase_name(WorldMigrationPhase phase) noexcept {
    switch (phase) {
    case WorldMigrationPhase::kIdle:
        return "idle";
    case WorldMigrationPhase::kTargetWorldMissing:
        return "target_world_missing";
    case WorldMigrationPhase::kTargetOwnerMissing:
        return "target_owner_missing";
    case WorldMigrationPhase::kTargetOwnerNotReady:
        return "target_owner_not_ready";
    case WorldMigrationPhase::kAwaitingSourceDrain:
        return "awaiting_source_drain";
    case WorldMigrationPhase::kReadyToResume:
        return "ready_to_resume";
    }
    return "idle";
}

/** @brief migration readiness 판단에 필요한 요약 플래그입니다. */
struct WorldMigrationSummary {
    bool envelope_present{false};
    bool source_draining{false};
    bool target_world_present{false};
    bool target_owner_present{false};
    bool target_owner_ready{false};
    bool target_owner_matches_target_world_owner{false};
    bool preserve_room{false};
};

/** @brief source->target world migration handoff의 현재 상태입니다. */
struct WorldMigrationStatus {
    std::string source_world_id;
    std::string target_world_id;
    std::string target_owner_instance_id;
    std::string payload_kind;
    std::string payload_ref;
    WorldMigrationPhase phase{WorldMigrationPhase::kIdle};
    WorldMigrationSummary summary;
};

inline std::string trim_migration_ascii_copy(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }

    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return std::string(value.substr(begin, end - begin));
}

inline bool parse_migration_bool(std::string_view raw) {
    std::string normalized;
    normalized.reserve(raw.size());
    for (char ch : raw) {
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    return normalized == "1"
        || normalized == "true"
        || normalized == "yes"
        || normalized == "on";
}

inline std::optional<WorldMigrationEnvelope> parse_world_migration_envelope(std::string_view payload) {
    if (payload.empty()) {
        return std::nullopt;
    }

    WorldMigrationEnvelope envelope;
    bool recognized = false;
    std::size_t offset = 0;
    while (offset <= payload.size()) {
        const std::size_t line_end = payload.find('\n', offset);
        const std::string_view line = line_end == std::string_view::npos
            ? payload.substr(offset)
            : payload.substr(offset, line_end - offset);
        if (!line.empty()) {
            const std::size_t separator = line.find('=');
            if (separator != std::string_view::npos) {
                const std::string key = trim_migration_ascii_copy(line.substr(0, separator));
                const std::string value = trim_migration_ascii_copy(line.substr(separator + 1));
                if (key == "target_world_id") {
                    envelope.target_world_id = value;
                    recognized = true;
                } else if (key == "target_owner_instance_id") {
                    envelope.target_owner_instance_id = value;
                    recognized = true;
                } else if (key == "preserve_room") {
                    envelope.preserve_room = parse_migration_bool(value);
                    recognized = true;
                } else if (key == "payload_kind") {
                    envelope.payload_kind = value;
                    recognized = true;
                } else if (key == "payload_ref") {
                    envelope.payload_ref = value;
                    recognized = true;
                } else if (key == "updated_at_ms") {
                    if (!value.empty()) {
                        envelope.updated_at_ms = static_cast<std::uint64_t>(std::strtoull(value.c_str(), nullptr, 10));
                    }
                    recognized = true;
                }
            }
        }

        if (line_end == std::string_view::npos) {
            break;
        }
        offset = line_end + 1;
    }

    if (!recognized || envelope.target_world_id.empty() || envelope.target_owner_instance_id.empty()) {
        return std::nullopt;
    }
    return envelope;
}

inline std::string serialize_world_migration_envelope(const WorldMigrationEnvelope& envelope) {
    std::ostringstream out;
    out << "target_world_id=" << envelope.target_world_id << '\n';
    out << "target_owner_instance_id=" << envelope.target_owner_instance_id << '\n';
    out << "preserve_room=" << (envelope.preserve_room ? "1" : "0") << '\n';
    out << "payload_kind=" << envelope.payload_kind << '\n';
    out << "payload_ref=" << envelope.payload_ref << '\n';
    out << "updated_at_ms=" << envelope.updated_at_ms << '\n';
    return out.str();
}

inline WorldMigrationStatus evaluate_world_migration(
    const ObservedWorldMigrationWorld& source_world,
    const std::optional<WorldMigrationEnvelope>& envelope,
    const std::optional<ObservedWorldMigrationWorld>& target_world) {
    WorldMigrationStatus out;
    out.source_world_id = source_world.world_id;
    out.summary.source_draining = source_world.draining;

    if (!envelope.has_value()) {
        out.phase = WorldMigrationPhase::kIdle;
        return out;
    }

    out.target_world_id = envelope->target_world_id;
    out.target_owner_instance_id = envelope->target_owner_instance_id;
    out.payload_kind = envelope->payload_kind;
    out.payload_ref = envelope->payload_ref;
    out.summary.envelope_present = true;
    out.summary.preserve_room = envelope->preserve_room;

    if (!target_world.has_value() || target_world->world_id.empty()) {
        out.phase = WorldMigrationPhase::kTargetWorldMissing;
        return out;
    }

    out.summary.target_world_present = true;
    out.summary.target_owner_matches_target_world_owner =
        !target_world->current_owner_instance_id.empty()
        && target_world->current_owner_instance_id == envelope->target_owner_instance_id;

    for (const auto& instance : target_world->instances) {
        if (instance.instance_id != envelope->target_owner_instance_id) {
            continue;
        }
        out.summary.target_owner_present = true;
        out.summary.target_owner_ready = instance.ready;
        break;
    }

    if (!out.summary.target_owner_present) {
        out.phase = WorldMigrationPhase::kTargetOwnerMissing;
    } else if (!out.summary.target_owner_ready) {
        out.phase = WorldMigrationPhase::kTargetOwnerNotReady;
    } else if (!source_world.draining) {
        out.phase = WorldMigrationPhase::kAwaitingSourceDrain;
    } else {
        out.phase = WorldMigrationPhase::kReadyToResume;
    }

    return out;
}

} // namespace server::core::worlds

