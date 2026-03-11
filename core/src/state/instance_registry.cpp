#include "server/core/state/instance_registry.hpp"

#include <cctype>
#include <unordered_set>

namespace server::core::state {

namespace {

std::string normalize_token(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    std::string out;
    out.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(value[i]))));
    }
    return out;
}

std::unordered_set<std::string> normalize_token_set(const std::vector<std::string>& values) {
    std::unordered_set<std::string> out;
    out.reserve(values.size());
    for (const auto& value : values) {
        std::string token = normalize_token(value);
        if (!token.empty()) {
            out.insert(std::move(token));
        }
    }
    return out;
}

bool matches_field(std::string_view value, const std::vector<std::string>& filters) {
    if (filters.empty()) {
        return true;
    }
    const auto normalized_filters = normalize_token_set(filters);
    if (normalized_filters.empty()) {
        return false;
    }
    const std::string normalized_value = normalize_token(value);
    return normalized_filters.find(normalized_value) != normalized_filters.end();
}

bool matches_tags(const std::vector<std::string>& record_tags, const std::vector<std::string>& selector_tags) {
    if (selector_tags.empty()) {
        return true;
    }
    const auto normalized_selector_tags = normalize_token_set(selector_tags);
    if (normalized_selector_tags.empty()) {
        return false;
    }
    for (const auto& tag : record_tags) {
        const std::string normalized_tag = normalize_token(tag);
        if (normalized_selector_tags.find(normalized_tag) != normalized_selector_tags.end()) {
            return true;
        }
    }
    return false;
}

bool has_at_least_one_selector_field(const InstanceSelector& selector) {
    return selector.all
        || !selector.server_ids.empty()
        || !selector.roles.empty()
        || !selector.game_modes.empty()
        || !selector.regions.empty()
        || !selector.shards.empty()
        || !selector.tags.empty();
}

} // namespace

bool InMemoryStateBackend::upsert(const InstanceRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    records_[record.instance_id] = record;
    return true;
}

bool InMemoryStateBackend::remove(const std::string& instance_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.erase(instance_id) > 0;
}

bool InMemoryStateBackend::touch(const std::string& instance_id, std::uint64_t heartbeat_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find(instance_id);
    if (it == records_.end()) {
        return false;
    }
    it->second.last_heartbeat_ms = heartbeat_ms;
    return true;
}

std::vector<InstanceRecord> InMemoryStateBackend::list_instances() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<InstanceRecord> result;
    result.reserve(records_.size());
    for (const auto& [_, record] : records_) {
        result.push_back(record);
    }
    return result;
}

bool matches_selector(const InstanceRecord& record, const InstanceSelector& selector) {
    if (selector.all) {
        return true;
    }

    if (!has_at_least_one_selector_field(selector)) {
        return false;
    }

    if (!matches_field(record.instance_id, selector.server_ids)) {
        return false;
    }
    if (!matches_field(record.role, selector.roles)) {
        return false;
    }
    if (!matches_field(record.game_mode, selector.game_modes)) {
        return false;
    }
    if (!matches_field(record.region, selector.regions)) {
        return false;
    }
    if (!matches_field(record.shard, selector.shards)) {
        return false;
    }
    if (!matches_tags(record.tags, selector.tags)) {
        return false;
    }
    return true;
}

SelectorPolicyLayer classify_selector_policy_layer(const InstanceSelector& selector) {
    if (selector.all) {
        return SelectorPolicyLayer::kGlobal;
    }
    if (!selector.server_ids.empty()) {
        return SelectorPolicyLayer::kServer;
    }
    if (!selector.shards.empty()) {
        return SelectorPolicyLayer::kShard;
    }
    if (!selector.regions.empty()) {
        return SelectorPolicyLayer::kRegion;
    }
    if (!selector.game_modes.empty()) {
        return SelectorPolicyLayer::kGameMode;
    }
    return SelectorPolicyLayer::kGlobal;
}

std::string_view selector_policy_layer_name(SelectorPolicyLayer layer) {
    switch (layer) {
    case SelectorPolicyLayer::kGlobal:
        return "global";
    case SelectorPolicyLayer::kGameMode:
        return "game_mode";
    case SelectorPolicyLayer::kRegion:
        return "region";
    case SelectorPolicyLayer::kShard:
        return "shard";
    case SelectorPolicyLayer::kServer:
        return "server";
    }
    return "global";
}

std::vector<InstanceRecord> select_instances(const std::vector<InstanceRecord>& instances,
                                             const InstanceSelector& selector,
                                             SelectorMatchStats* stats) {
    SelectorMatchStats local_stats{};
    std::vector<InstanceRecord> matched;
    matched.reserve(instances.size());

    for (const auto& record : instances) {
        ++local_stats.scanned;
        if (matches_selector(record, selector)) {
            ++local_stats.matched;
            matched.push_back(record);
        } else {
            ++local_stats.selector_mismatch;
        }
    }

    if (stats != nullptr) {
        *stats = local_stats;
    }
    return matched;
}

} // namespace server::core::state
