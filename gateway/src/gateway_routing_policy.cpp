#include "gateway/gateway_app.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gateway_app_access.hpp"
#include "gateway_app_state.hpp"
#include "gateway_backend_connection.hpp"
#include "server/core/discovery/world_lifecycle_policy.hpp"
#include "server/core/storage/redis/client.hpp"
#include "server/core/util/log.hpp"

namespace gateway {

namespace {

std::uint64_t steady_time_ms() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
    return static_cast<std::uint64_t>(now.count());
}

constexpr std::string_view kResumeRoutingPrefix = "resume-hash:";

bool is_resume_routing_key(std::string_view value) {
    return value.rfind(kResumeRoutingPrefix, 0) == 0;
}

std::optional<std::string> extract_world_id_from_tags(const std::vector<std::string>& tags) {
    static constexpr std::string_view kWorldPrefix = "world:";
    for (const auto& tag : tags) {
        const std::string_view value(tag);
        if (value.rfind(kWorldPrefix, 0) == 0 && value.size() > kWorldPrefix.size()) {
            return std::string(value.substr(kWorldPrefix.size()));
        }
    }
    return std::nullopt;
}

std::string make_world_policy_key(std::string_view continuity_prefix, std::string_view world_id) {
    return std::string(continuity_prefix) + "world-policy:" + std::string(world_id);
}

std::unordered_map<std::string, server::core::discovery::WorldLifecyclePolicy>
load_world_policy_index(server::core::storage::redis::IRedisClient* redis_client,
                        std::string_view continuity_prefix,
                        const std::vector<server::core::discovery::InstanceRecord>& items) {
    std::unordered_map<std::string, server::core::discovery::WorldLifecyclePolicy> out;
    if (!redis_client || continuity_prefix.empty()) {
        return out;
    }

    std::vector<std::string> world_ids;
    std::vector<std::string> policy_keys;
    for (const auto& item : items) {
        const auto world_id = extract_world_id_from_tags(item.tags);
        if (!world_id.has_value()) {
            continue;
        }
        if (out.contains(*world_id)) {
            continue;
        }
        world_ids.push_back(*world_id);
        policy_keys.push_back(make_world_policy_key(continuity_prefix, *world_id));
        out.emplace(*world_id, server::core::discovery::WorldLifecyclePolicy{});
    }

    if (policy_keys.empty()) {
        return out;
    }

    std::vector<std::optional<std::string>> payloads(policy_keys.size());
    bool mget_ok = false;
    try {
        mget_ok = redis_client->mget(policy_keys, payloads);
    } catch (...) {
        mget_ok = false;
    }

    if (!mget_ok || payloads.size() != policy_keys.size()) {
        payloads.clear();
        payloads.reserve(policy_keys.size());
        for (const auto& key : policy_keys) {
            try {
                payloads.push_back(redis_client->get(key));
            } catch (...) {
                payloads.push_back(std::nullopt);
            }
        }
    }

    for (std::size_t i = 0; i < world_ids.size() && i < payloads.size(); ++i) {
        if (!payloads[i].has_value() || payloads[i]->empty()) {
            continue;
        }
        if (const auto parsed = server::core::discovery::parse_world_lifecycle_policy(*payloads[i])) {
            out[world_ids[i]] = *parsed;
        }
    }

    return out;
}

struct WorldPolicyBackendDecision {
    bool allowed{true};
    bool draining_filtered{false};
    bool replacement_match{false};
    std::optional<std::string> world_id;
};

struct ResumeLocatorHint {
    std::string backend_instance_id;
    std::string world_id;
    std::string role;
    std::string game_mode;
    std::string region;
    std::string shard;
};

WorldPolicyBackendDecision evaluate_world_policy_backend(
    const server::core::discovery::InstanceRecord& record,
    const std::unordered_map<std::string, server::core::discovery::WorldLifecyclePolicy>& world_policy_index) {
    WorldPolicyBackendDecision decision;
    const auto world_id = extract_world_id_from_tags(record.tags);
    if (!world_id.has_value()) {
        return decision;
    }
    decision.world_id = *world_id;

    const auto policy_it = world_policy_index.find(*world_id);
    if (policy_it == world_policy_index.end() || !policy_it->second.draining) {
        return decision;
    }

    decision.draining_filtered = true;
    decision.replacement_match = !policy_it->second.replacement_owner_instance_id.empty()
        && policy_it->second.replacement_owner_instance_id == record.instance_id;
    decision.allowed = decision.replacement_match;
    return decision;
}

std::string serialize_resume_locator_hint(const ResumeLocatorHint& hint) {
    std::ostringstream out;
    out << "backend=" << hint.backend_instance_id << '\n';
    out << "world_id=" << hint.world_id << '\n';
    out << "role=" << hint.role << '\n';
    out << "game_mode=" << hint.game_mode << '\n';
    out << "region=" << hint.region << '\n';
    out << "shard=" << hint.shard << '\n';
    return out.str();
}

std::optional<ResumeLocatorHint> parse_resume_locator_hint(std::string_view payload) {
    ResumeLocatorHint hint;
    std::size_t offset = 0;
    while (offset <= payload.size()) {
        const std::size_t line_end = payload.find('\n', offset);
        const std::string_view line =
            line_end == std::string_view::npos ? payload.substr(offset) : payload.substr(offset, line_end - offset);
        if (!line.empty()) {
            const std::size_t sep = line.find('=');
            if (sep != std::string_view::npos) {
                std::string value(line.substr(sep + 1));
                if (!value.empty() && value.back() == '\r') {
                    value.pop_back();
                }
                const std::string_view key = line.substr(0, sep);
                if (key == "backend") {
                    hint.backend_instance_id = std::move(value);
                } else if (key == "world_id") {
                    hint.world_id = std::move(value);
                } else if (key == "role") {
                    hint.role = std::move(value);
                } else if (key == "game_mode") {
                    hint.game_mode = std::move(value);
                } else if (key == "region") {
                    hint.region = std::move(value);
                } else if (key == "shard") {
                    hint.shard = std::move(value);
                }
            }
        }

        if (line_end == std::string_view::npos) {
            break;
        }
        offset = line_end + 1;
    }

    if (hint.backend_instance_id.empty()
        && hint.world_id.empty()
        && hint.role.empty()
        && hint.game_mode.empty()
        && hint.region.empty()
        && hint.shard.empty()) {
        return std::nullopt;
    }
    return hint;
}

} // namespace

void GatewayAppAccess::record_connection_accept(GatewayApp& app) {
    (void)app.impl_->connections_total_.fetch_add(1, std::memory_order_relaxed);
}

void GatewayAppAccess::record_backend_resolve_fail(GatewayApp& app) {
    (void)app.impl_->backend_resolve_fail_total_.fetch_add(1, std::memory_order_relaxed);
}

void GatewayAppAccess::record_backend_connect_fail(GatewayApp& app) {
    (void)app.impl_->backend_connect_fail_total_.fetch_add(1, std::memory_order_relaxed);
}

void GatewayAppAccess::record_backend_connect_timeout(GatewayApp& app) {
    (void)app.impl_->backend_connect_timeout_total_.fetch_add(1, std::memory_order_relaxed);
}

void GatewayAppAccess::record_backend_write_error(GatewayApp& app) {
    (void)app.impl_->backend_write_error_total_.fetch_add(1, std::memory_order_relaxed);
}

void GatewayAppAccess::record_backend_send_queue_overflow(GatewayApp& app) {
    (void)app.impl_->backend_send_queue_overflow_total_.fetch_add(1, std::memory_order_relaxed);
}

void GatewayAppAccess::record_backend_retry_scheduled(GatewayApp& app) {
    (void)app.impl_->backend_connect_retry_total_.fetch_add(1, std::memory_order_relaxed);
}

void GatewayAppAccess::record_backend_retry_budget_exhausted(GatewayApp& app) {
    (void)app.impl_->backend_retry_budget_exhausted_total_.fetch_add(1, std::memory_order_relaxed);
}

GatewayAppAccess::IngressAdmission GatewayAppAccess::admit_ingress_connection(GatewayApp& app) {
    if (!app.impl_->app_host_.ready() || !app.impl_->app_host_.healthy() || app.impl_->app_host_.stop_requested()) {
        (void)app.impl_->ingress_reject_not_ready_total_.fetch_add(1, std::memory_order_relaxed);
        return IngressAdmission::kRejectNotReady;
    }

    const auto now_ms = steady_time_ms();
    if (app.impl_->backend_circuit_breaker_.is_open(now_ms)) {
        (void)app.impl_->ingress_reject_circuit_open_total_.fetch_add(1, std::memory_order_relaxed);
        return IngressAdmission::kRejectCircuitOpen;
    }

    if (app.impl_->ingress_max_active_sessions_ > 0) {
        std::size_t session_count = 0;
        {
            std::lock_guard<std::mutex> lock(app.impl_->session_mutex_);
            session_count = app.impl_->sessions_.size();
        }
        if (session_count >= app.impl_->ingress_max_active_sessions_) {
            (void)app.impl_->ingress_reject_session_limit_total_.fetch_add(1, std::memory_order_relaxed);
            return IngressAdmission::kRejectSessionLimit;
        }
    }

    if (!app.impl_->ingress_token_bucket_.consume(now_ms)) {
        (void)app.impl_->ingress_reject_rate_limit_total_.fetch_add(1, std::memory_order_relaxed);
        return IngressAdmission::kRejectRateLimited;
    }

    return IngressAdmission::kAccept;
}

const char* GatewayAppAccess::ingress_admission_name(IngressAdmission admission) noexcept {
    switch (admission) {
        case IngressAdmission::kAccept:
            return "accept";
        case IngressAdmission::kRejectNotReady:
            return "not_ready";
        case IngressAdmission::kRejectRateLimited:
            return "rate_limited";
        case IngressAdmission::kRejectSessionLimit:
            return "session_limit";
        case IngressAdmission::kRejectCircuitOpen:
            return "circuit_open";
    }
    return "unknown";
}

bool GatewayAppAccess::backend_circuit_allows_connect(GatewayApp& app) {
    if (app.impl_->backend_circuit_breaker_.allow(steady_time_ms())) {
        return true;
    }

    (void)app.impl_->backend_circuit_reject_total_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void GatewayAppAccess::record_backend_connect_success_event(GatewayApp& app) {
    app.impl_->backend_circuit_breaker_.record_success();
}

void GatewayAppAccess::record_backend_connect_failure_event(GatewayApp& app) {
    if (app.impl_->backend_circuit_breaker_.record_failure(steady_time_ms())) {
        (void)app.impl_->backend_circuit_open_total_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool GatewayAppAccess::consume_backend_retry_budget(GatewayApp& app) {
    return app.impl_->backend_retry_budget_.consume(steady_time_ms());
}

std::chrono::milliseconds GatewayAppAccess::backend_retry_delay(const GatewayApp& app, std::uint32_t attempt) {
    const auto capped_attempt = std::min<std::uint32_t>(attempt, 8);
    const auto factor = 1ull << (capped_attempt == 0 ? 0 : (capped_attempt - 1));
    const auto base_delay = static_cast<std::uint64_t>(app.impl_->backend_connect_retry_backoff_ms_) * factor;
    const auto bounded_delay = std::min<std::uint64_t>(base_delay, app.impl_->backend_connect_retry_backoff_max_ms_);
    return std::chrono::milliseconds{bounded_delay};
}

std::optional<GatewayAppAccess::CreatedBackendSession> GatewayAppAccess::create_backend_connection(
    GatewayApp& app,
    const std::string& client_id,
    std::weak_ptr<GatewayConnection> connection) {
    if (!backend_circuit_allows_connect(app)) {
        server::core::log::warn("GatewayApp backend circuit open: rejecting new backend connect attempt");
        return std::nullopt;
    }

    auto selected = select_best_server(app, client_id);
    if (!selected) {
        server::core::log::error("GatewayApp: No available backend server found");
        return std::nullopt;
    }

    static std::atomic<std::uint64_t> counter{0};
    std::string session_id = app.impl_->gateway_id_ + "-" + app.impl_->boot_id_ + "-" + std::to_string(++counter);

    auto session = std::make_shared<BackendConnection>(
        app,
        session_id,
        client_id,
        selected->record.instance_id,
        selected->sticky_hit,
        std::move(connection),
        app.impl_->backend_send_queue_max_bytes_,
        std::chrono::milliseconds{app.impl_->backend_connect_timeout_ms_}
    );

    server::core::log::info(
        "GatewayApp connecting session " + session_id +
        " backend=" + selected->record.instance_id +
        " addr=" + selected->record.host + ":" + std::to_string(selected->record.port)
    );
    session->connect(selected->record.host, selected->record.port);

    {
        std::lock_guard<std::mutex> lock(app.impl_->session_mutex_);
        auto state = std::make_unique<GatewayApp::Impl::SessionState>();
        state->session = session;
        state->client_id = client_id;
        state->backend_instance_id = selected->record.instance_id;
        app.impl_->sessions_[session_id] = std::move(state);
    }

    CreatedBackendSession created{};
    created.session = session;
    created.session_id = std::move(session_id);
    created.backend_instance_id = selected->record.instance_id;
    return created;
}

void GatewayAppAccess::close_backend_connection(GatewayApp& app, const std::string& session_id) {
    TransportSessionPtr session;
    {
        std::lock_guard<std::mutex> lock(app.impl_->session_mutex_);
        auto it = app.impl_->sessions_.find(session_id);
        if (it != app.impl_->sessions_.end()) {
            session = it->second ? it->second->session : nullptr;
            app.impl_->sessions_.erase(it);
        }
    }
    if (session) {
        session->close();
    }
}

std::optional<GatewayAppAccess::SelectedBackend> GatewayAppAccess::select_best_server(GatewayApp& app,
                                                                                       const std::string& client_id) {
    if (!app.impl_->backend_registry_) {
        return std::nullopt;
    }

    auto instances = app.impl_->backend_registry_->list_instances();
    if (instances.empty()) {
        return std::nullopt;
    }

    const auto world_policy_index =
        load_world_policy_index(app.impl_->redis_client_.get(), app.impl_->continuity_prefix_, instances);
    const auto backend_policy_decision = [&](const server::core::discovery::InstanceRecord& record) {
        return evaluate_world_policy_backend(record, world_policy_index);
    };

    if (app.impl_->session_directory_ && !client_id.empty() && client_id != "anonymous") {
        if (auto backend_id = app.impl_->session_directory_->find_backend(client_id)) {
            auto it = std::find_if(instances.begin(), instances.end(), [&](const auto& rec) {
                return rec.instance_id == *backend_id && rec.ready;
            });
            if (it != instances.end()) {
                const auto policy_decision = backend_policy_decision(*it);
                if (policy_decision.allowed) {
                    if (is_resume_routing_key(client_id)) {
                        (void)app.impl_->resume_routing_hit_total_.fetch_add(1, std::memory_order_relaxed);
                    }
                    return SelectedBackend{*it, true};
                }
                if (policy_decision.draining_filtered) {
                    (void)app.impl_->world_policy_filtered_sticky_total_.fetch_add(1, std::memory_order_relaxed);
                }
            }
            app.impl_->session_directory_->release_backend(client_id, *backend_id);
        }
    }

    std::optional<server::core::discovery::InstanceSelector> resume_locator_selector;
    if (is_resume_routing_key(client_id)) {
        resume_locator_selector = load_resume_locator_selector(app, client_id);
    }

    std::unordered_map<std::string, std::size_t> local_backend_load;
    {
        std::lock_guard<std::mutex> lock(app.impl_->session_mutex_);
        for (const auto& [session_id, state] : app.impl_->sessions_) {
            (void)session_id;
            if (state && !state->backend_instance_id.empty()) {
                ++local_backend_load[state->backend_instance_id];
            }
        }
    }

    const auto build_candidates =
        [&](const std::optional<server::core::discovery::InstanceSelector>& selector) {
            std::vector<const server::core::discovery::InstanceRecord*> candidates;
            std::unordered_set<std::string> filtered_world_ids;
            std::uint64_t min_effective_load = std::numeric_limits<std::uint64_t>::max();
            for (const auto& rec : instances) {
                if (!rec.ready || rec.instance_id.empty() || rec.host.empty() || rec.port == 0) {
                    continue;
                }
                if (selector.has_value() && !server::core::discovery::matches_selector(rec, *selector)) {
                    continue;
                }
                const auto policy_decision = backend_policy_decision(rec);
                if (!policy_decision.allowed) {
                    if (policy_decision.draining_filtered) {
                        (void)app.impl_->world_policy_filtered_candidate_total_.fetch_add(1, std::memory_order_relaxed);
                        if (policy_decision.world_id.has_value()) {
                            filtered_world_ids.insert(*policy_decision.world_id);
                        }
                    }
                    continue;
                }

                const auto local_it = local_backend_load.find(rec.instance_id);
                const std::uint64_t local_load =
                    local_it == local_backend_load.end() ? 0ull : static_cast<std::uint64_t>(local_it->second);
                const std::uint64_t effective_load = static_cast<std::uint64_t>(rec.active_sessions) + local_load;

                if (effective_load < min_effective_load) {
                    min_effective_load = effective_load;
                    candidates.clear();
                    candidates.push_back(&rec);
                } else if (effective_load == min_effective_load) {
                    candidates.push_back(&rec);
                }
            }
            return std::pair{std::move(candidates), std::move(filtered_world_ids)};
        };

    auto [candidates, filtered_world_ids] = build_candidates(resume_locator_selector);
    if (candidates.empty() && resume_locator_selector.has_value()) {
        (void)app.impl_->resume_locator_selector_fallback_total_.fetch_add(1, std::memory_order_relaxed);
        std::tie(candidates, filtered_world_ids) = build_candidates(std::nullopt);
    } else if (!candidates.empty() && resume_locator_selector.has_value()) {
        (void)app.impl_->resume_locator_selector_hit_total_.fetch_add(1, std::memory_order_relaxed);
    }

    if (candidates.empty()) {
        return std::nullopt;
    }

    std::size_t selected_index = 0;
    if (candidates.size() > 1) {
        if (!client_id.empty() && client_id != "anonymous") {
            selected_index = std::hash<std::string>{}(client_id) % candidates.size();
        } else {
            static std::atomic<std::uint64_t> rr_counter{0};
            selected_index = static_cast<std::size_t>(
                rr_counter.fetch_add(1, std::memory_order_relaxed) % candidates.size());
        }
    }

    const auto& selected = *candidates[selected_index];
    const auto selected_policy_decision = backend_policy_decision(selected);
    if (selected_policy_decision.replacement_match
        && selected_policy_decision.world_id.has_value()
        && filtered_world_ids.contains(*selected_policy_decision.world_id)) {
        (void)app.impl_->world_policy_replacement_selected_total_.fetch_add(1, std::memory_order_relaxed);
    }

    return SelectedBackend{selected, false};
}

void GatewayAppAccess::register_resume_routing_key(GatewayApp& app,
                                                   const std::string& routing_key,
                                                   const std::string& backend_instance_id) {
    if (!app.impl_->session_directory_ || routing_key.empty() || backend_instance_id.empty()) {
        return;
    }
    if (!is_resume_routing_key(routing_key)) {
        return;
    }

    const auto bound = app.impl_->session_directory_->ensure_backend(routing_key, backend_instance_id);
    if (bound && *bound != backend_instance_id) {
        server::core::log::warn(
            "GatewayApp resume routing key already bound: key=" + routing_key
            + " desired=" + backend_instance_id
            + " existing=" + *bound);
        return;
    }

    (void)app.impl_->resume_routing_bind_total_.fetch_add(1, std::memory_order_relaxed);

    if (!app.impl_->backend_registry_) {
        return;
    }

    const auto instances = app.impl_->backend_registry_->list_instances();
    const auto it = std::find_if(instances.begin(), instances.end(), [&](const auto& rec) {
        return rec.instance_id == backend_instance_id;
    });
    if (it != instances.end()) {
        persist_resume_locator_hint(app, routing_key, *it);
    }
}

std::string GatewayAppAccess::make_resume_locator_key(const GatewayApp& app, std::string_view routing_key) {
    if (routing_key.empty()) {
        return {};
    }

    std::string key;
    key.reserve(app.impl_->resume_locator_prefix_.size() + routing_key.size());
    key.append(app.impl_->resume_locator_prefix_);
    key.append(routing_key);
    return key;
}

std::optional<server::core::discovery::InstanceSelector> GatewayAppAccess::load_resume_locator_selector(
    GatewayApp& app,
    std::string_view routing_key) {
    if (!app.impl_->redis_client_ || routing_key.empty() || !is_resume_routing_key(routing_key)) {
        return std::nullopt;
    }

    const auto payload = app.impl_->redis_client_->get(make_resume_locator_key(app, routing_key));
    if (!payload.has_value() || payload->empty()) {
        (void)app.impl_->resume_locator_lookup_miss_total_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }
    (void)app.impl_->resume_locator_lookup_hit_total_.fetch_add(1, std::memory_order_relaxed);

    const auto hint = parse_resume_locator_hint(*payload);
    if (!hint.has_value()) {
        return std::nullopt;
    }

    server::core::discovery::InstanceSelector selector{};
    if (!hint->world_id.empty()) {
        selector.tags.push_back("world:" + hint->world_id);
    }
    if (!hint->role.empty()) {
        selector.roles.push_back(hint->role);
    }
    if (!hint->game_mode.empty()) {
        selector.game_modes.push_back(hint->game_mode);
    }
    if (!hint->region.empty()) {
        selector.regions.push_back(hint->region);
    }
    if (!hint->shard.empty()) {
        selector.shards.push_back(hint->shard);
    }

    if (selector.roles.empty()
        && selector.tags.empty()
        && selector.game_modes.empty()
        && selector.regions.empty()
        && selector.shards.empty()) {
        return std::nullopt;
    }
    return selector;
}

void GatewayAppAccess::persist_resume_locator_hint(GatewayApp& app,
                                                   std::string_view routing_key,
                                                   const server::core::discovery::InstanceRecord& record) {
    if (!app.impl_->redis_client_ || routing_key.empty() || !is_resume_routing_key(routing_key)
        || app.impl_->resume_locator_ttl_sec_ == 0) {
        return;
    }

    ResumeLocatorHint hint;
    hint.backend_instance_id = record.instance_id;
    hint.world_id = extract_world_id_from_tags(record.tags).value_or("");
    hint.role = record.role;
    hint.game_mode = record.game_mode;
    hint.region = record.region;
    hint.shard = record.shard;

    if (app.impl_->redis_client_->setex(
            make_resume_locator_key(app, routing_key),
            serialize_resume_locator_hint(hint),
            app.impl_->resume_locator_ttl_sec_)) {
        (void)app.impl_->resume_locator_bind_total_.fetch_add(1, std::memory_order_relaxed);
    } else {
        server::core::log::warn("GatewayApp failed to persist resume locator hint");
    }
}

void GatewayAppAccess::on_backend_connected(GatewayApp& app,
                                            const std::string& client_id,
                                            const std::string& backend_instance_id,
                                            bool sticky_hit) {
    if (!app.impl_->session_directory_) {
        return;
    }
    if (client_id.empty() || client_id == "anonymous") {
        return;
    }
    if (backend_instance_id.empty()) {
        return;
    }

    auto bound = app.impl_->session_directory_->ensure_backend(client_id, backend_instance_id);
    if (sticky_hit && bound && *bound != backend_instance_id) {
        server::core::log::warn(
            "GatewayApp sticky mismatch: client_id=" + client_id +
            " desired=" + backend_instance_id +
            " existing=" + *bound
        );
    }
}

} // namespace gateway
