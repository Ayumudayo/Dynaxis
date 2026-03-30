#include "admin_fanout_subscription.hpp"

#include "server_runtime_state.hpp"

#include "server/chat/chat_service.hpp"
#include "server/core/discovery/instance_registry.hpp"
#include "server/core/runtime_metrics.hpp"
#include "server/core/security/admin_command_auth.hpp"
#include "server/core/storage/redis/client.hpp"
#include "server/core/util/log.hpp"

#include <array>
#include <cctype>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace server::app {

namespace corelog = server::core::log;
namespace admin_auth = server::core::security::admin_command_auth;

namespace {

enum class ExactFanoutChannel {
    kWhisper,
    kAdminDisconnect,
    kAdminAnnounce,
    kAdminSettings,
    kAdminModeration,
};

using ExactChannelEntry = std::pair<std::string, ExactFanoutChannel>;

struct AdminSelectorParseResult {
    server::core::state::InstanceSelector selector;
    bool selector_specified{false};
    bool valid{true};
};

std::string trim_ascii(std::string_view value) {
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

std::unordered_map<std::string, std::string> parse_kv_lines(std::string_view payload) {
    std::unordered_map<std::string, std::string> out;
    std::size_t start = 0;
    while (start <= payload.size()) {
        std::size_t end = payload.find('\n', start);
        if (end == std::string_view::npos) {
            end = payload.size();
        }

        const auto line = payload.substr(start, end - start);
        const auto eq = line.find('=');
        if (eq != std::string_view::npos) {
            const std::string key = trim_ascii(line.substr(0, eq));
            const std::string value = trim_ascii(line.substr(eq + 1));
            if (!key.empty()) {
                out[key] = value;
            }
        }

        if (end == payload.size()) {
            break;
        }
        start = end + 1;
    }
    return out;
}

std::vector<std::string> split_csv(std::string_view input) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= input.size()) {
        std::size_t end = input.find(',', start);
        if (end == std::string_view::npos) {
            end = input.size();
        }

        std::string token = trim_ascii(input.substr(start, end - start));
        if (!token.empty()) {
            out.push_back(std::move(token));
        }

        if (end == input.size()) {
            break;
        }
        start = end + 1;
    }
    return out;
}

std::string to_lower_ascii(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char ch : value) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

AdminSelectorParseResult parse_admin_selector_fields(
    const std::unordered_map<std::string, std::string>& fields) {
    AdminSelectorParseResult result;

    auto parse_csv_field = [&](const char* key, std::vector<std::string>& out_values) {
        const auto it = fields.find(key);
        if (it == fields.end()) {
            return;
        }
        result.selector_specified = true;
        out_values = split_csv(it->second);
    };

    if (const auto it = fields.find("all"); it != fields.end()) {
        result.selector_specified = true;
        const std::string normalized = to_lower_ascii(trim_ascii(it->second));
        if (normalized.empty() || normalized == "1" || normalized == "true"
            || normalized == "yes" || normalized == "on") {
            result.selector.all = true;
        } else if (normalized == "0" || normalized == "false"
                   || normalized == "no" || normalized == "off") {
            result.selector.all = false;
        } else {
            result.valid = false;
            return result;
        }
    }

    parse_csv_field("server_ids", result.selector.server_ids);
    parse_csv_field("roles", result.selector.roles);
    parse_csv_field("game_modes", result.selector.game_modes);
    parse_csv_field("regions", result.selector.regions);
    parse_csv_field("shards", result.selector.shards);
    parse_csv_field("tags", result.selector.tags);

    return result;
}

std::optional<ExactFanoutChannel> match_exact_channel(
    std::string_view channel,
    const std::array<ExactChannelEntry, 5>& exact_channels) {
    for (const auto& [name, kind] : exact_channels) {
        if (channel == name) {
            return kind;
        }
    }
    return std::nullopt;
}

bool verify_admin_fields(
    std::string_view channel,
    const std::unordered_map<std::string, std::string>& admin_fields,
    const std::shared_ptr<admin_auth::Verifier>& admin_command_verifier,
    const server::core::state::InstanceRecord& local_instance_selector_context) {
    const admin_auth::VerifyResult verify_result = admin_command_verifier->verify(admin_fields);
    record_bootstrap_admin_verify_result(verify_result);
    if (verify_result != admin_auth::VerifyResult::kOk) {
        const auto request_id_it = admin_fields.find("request_id");
        const auto actor_it = admin_fields.find("actor");
        const std::string request_id =
            request_id_it == admin_fields.end() ? std::string("unknown") : request_id_it->second;
        const std::string actor =
            actor_it == admin_fields.end() ? std::string("unknown") : actor_it->second;
        corelog::warn(
            "admin command rejected channel=" + std::string(channel)
            + " reason=" + admin_auth::to_string(verify_result)
            + " request_id=" + request_id
            + " actor=" + actor);
        return false;
    }

    const AdminSelectorParseResult selector_parse = parse_admin_selector_fields(admin_fields);
    if (!selector_parse.selector_specified) {
        return true;
    }

    bool target_match = false;
    if (selector_parse.valid) {
        target_match = server::core::state::matches_selector(
            local_instance_selector_context,
            selector_parse.selector);
    }

    if (target_match) {
        return true;
    }

    record_bootstrap_admin_target_mismatch();
    const auto request_id_it = admin_fields.find("request_id");
    const auto actor_it = admin_fields.find("actor");
    const std::string request_id =
        request_id_it == admin_fields.end() ? std::string("unknown") : request_id_it->second;
    const std::string actor =
        actor_it == admin_fields.end() ? std::string("unknown") : actor_it->second;

    std::string layer = "invalid";
    if (selector_parse.valid) {
        layer = std::string(server::core::state::selector_policy_layer_name(
            server::core::state::classify_selector_policy_layer(selector_parse.selector)));
    }

    corelog::info(
        "admin command ignored by target selector channel=" + std::string(channel)
        + " request_id=" + request_id
        + " actor=" + actor
        + " layer=" + layer
        + " instance_id=" + local_instance_selector_context.instance_id);
    return false;
}

void dispatch_exact_fanout_channel(
    chat::ChatService& chat,
    ExactFanoutChannel exact_match,
    const std::string& payload,
    const std::unordered_map<std::string, std::string>& admin_fields) {
    switch (exact_match) {
    case ExactFanoutChannel::kWhisper: {
        std::vector<std::uint8_t> body(payload.begin(), payload.end());
        chat.deliver_remote_whisper(body);
        record_bootstrap_subscribe();
        return;
    }
    case ExactFanoutChannel::kAdminDisconnect: {
        const auto it = admin_fields.find("client_ids");
        if (it == admin_fields.end() || it->second.empty()) {
            return;
        }

        auto users = split_csv(it->second);
        if (users.empty()) {
            return;
        }

        std::string reason = "Disconnected by administrator";
        if (const auto reason_it = admin_fields.find("reason");
            reason_it != admin_fields.end() && !reason_it->second.empty()) {
            reason = reason_it->second;
        }

        chat.admin_disconnect_users(users, reason);
        record_bootstrap_subscribe();
        return;
    }
    case ExactFanoutChannel::kAdminAnnounce: {
        const auto text_it = admin_fields.find("text");
        if (text_it == admin_fields.end() || text_it->second.empty()) {
            return;
        }

        chat.admin_broadcast_notice(text_it->second);
        record_bootstrap_subscribe();
        return;
    }
    case ExactFanoutChannel::kAdminSettings: {
        const auto key_it = admin_fields.find("key");
        const auto value_it = admin_fields.find("value");
        if (key_it == admin_fields.end() || value_it == admin_fields.end()
            || key_it->second.empty() || value_it->second.empty()) {
            return;
        }

        chat.admin_apply_runtime_setting(key_it->second, value_it->second);
        record_bootstrap_subscribe();
        return;
    }
    case ExactFanoutChannel::kAdminModeration: {
        const auto op_it = admin_fields.find("op");
        const auto users_it = admin_fields.find("client_ids");
        if (op_it == admin_fields.end() || users_it == admin_fields.end()
            || op_it->second.empty() || users_it->second.empty()) {
            return;
        }

        auto users = split_csv(users_it->second);
        if (users.empty()) {
            return;
        }

        std::uint32_t duration_sec = 0;
        if (const auto duration_it = admin_fields.find("duration_sec");
            duration_it != admin_fields.end() && !duration_it->second.empty()) {
            try {
                duration_sec = static_cast<std::uint32_t>(std::stoul(duration_it->second));
            } catch (...) {
                server::core::runtime_metrics::record_exception_ignored();
                corelog::warn(
                    "component=server_bootstrap error_code=INVALID_DURATION admin moderation duration parse failed duration_sec="
                    + duration_it->second);
                duration_sec = 0;
            }
        }

        std::string reason;
        if (const auto reason_it = admin_fields.find("reason"); reason_it != admin_fields.end()) {
            reason = reason_it->second;
        }

        chat.admin_apply_user_moderation(op_it->second, users, duration_sec, reason);
        record_bootstrap_subscribe();
        return;
    }
    }
}

} // namespace

std::shared_ptr<server::core::security::admin_command_auth::Verifier>
make_admin_command_verifier(const ServerConfig& config) {
    admin_auth::VerifyOptions admin_verify_options;
    admin_verify_options.ttl_ms = config.admin_command_ttl_ms;
    admin_verify_options.future_skew_ms = config.admin_command_future_skew_ms;
    return std::make_shared<admin_auth::Verifier>(
        config.admin_command_signing_secret,
        admin_verify_options);
}

server::core::state::InstanceRecord
make_local_instance_selector_context(const ServerConfig& config) {
    server::core::state::InstanceRecord local_instance_selector_context{};
    local_instance_selector_context.instance_id = config.server_instance_id;
    local_instance_selector_context.role = config.server_role;
    local_instance_selector_context.game_mode = config.server_game_mode;
    local_instance_selector_context.region = config.server_region;
    local_instance_selector_context.shard = config.server_shard;
    local_instance_selector_context.tags = config.server_tags;
    return local_instance_selector_context;
}

void start_chat_fanout_subscription(
    chat::ChatService& chat,
    const std::shared_ptr<server::core::storage::redis::IRedisClient>& redis,
    const ServerConfig& config,
    const std::shared_ptr<server::core::security::admin_command_auth::Verifier>& admin_command_verifier,
    const server::core::state::InstanceRecord& local_instance_selector_context) {
    if (!redis || !config.use_redis_pubsub) {
        return;
    }

    if (config.admin_command_signing_secret.empty()) {
        corelog::warn("ADMIN_COMMAND_SIGNING_SECRET is empty; admin fanout commands will be rejected");
    }

    const std::string pattern_all = config.redis_channel_prefix + std::string("fanout:*");
    const std::string gwid = config.gateway_id;
    const std::string refresh_prefix = config.redis_channel_prefix + "fanout:refresh:";
    const std::string room_prefix = config.redis_channel_prefix + "fanout:room:";
    const std::array<ExactChannelEntry, 5> exact_channels{{
        {config.redis_channel_prefix + "fanout:whisper", ExactFanoutChannel::kWhisper},
        {config.redis_channel_prefix + "fanout:admin:disconnect", ExactFanoutChannel::kAdminDisconnect},
        {config.redis_channel_prefix + "fanout:admin:announce", ExactFanoutChannel::kAdminAnnounce},
        {config.redis_channel_prefix + "fanout:admin:settings", ExactFanoutChannel::kAdminSettings},
        {config.redis_channel_prefix + "fanout:admin:moderation", ExactFanoutChannel::kAdminModeration},
    }};

    redis->start_psubscribe(
        pattern_all,
        [&chat,
         gwid,
         refresh_prefix,
         room_prefix,
         exact_channels,
         admin_command_verifier,
         local_instance_selector_context](const std::string& channel, const std::string& message) {
            if (message.rfind("gw=", 0) != 0) {
                return;
            }

            const auto nl = message.find('\n');
            const std::string from =
                nl != std::string::npos ? message.substr(3, nl - 3) : message.substr(3);
            if (from == gwid) {
                return;
            }

            if (channel.rfind(refresh_prefix, 0) == 0) {
                const std::string room = channel.substr(refresh_prefix.size());
                chat.broadcast_refresh_local(room);
                corelog::info("DEBUG: Received refresh notify for room: " + room + " from " + from);
                return;
            }

            if (channel.rfind(room_prefix, 0) == 0) {
                if (nl == std::string::npos) {
                    return;
                }
                const std::string payload = message.substr(nl + 1);
                const std::string room = channel.substr(room_prefix.size());
                std::vector<std::uint8_t> body(payload.begin(), payload.end());
                chat.broadcast_room(room, body, nullptr);
                record_bootstrap_subscribe();
                return;
            }

            const auto exact_match = match_exact_channel(channel, exact_channels);
            if (!exact_match.has_value() || nl == std::string::npos) {
                return;
            }

            const std::string payload = message.substr(nl + 1);
            std::unordered_map<std::string, std::string> admin_fields;
            if (*exact_match != ExactFanoutChannel::kWhisper) {
                admin_fields = parse_kv_lines(payload);
                if (!verify_admin_fields(
                        channel,
                        admin_fields,
                        admin_command_verifier,
                        local_instance_selector_context)) {
                    return;
                }
            }

            dispatch_exact_fanout_channel(chat, *exact_match, payload, admin_fields);
        });

    corelog::info(std::string("Subscribed Redis pattern: ") + pattern_all);
}

} // namespace server::app
