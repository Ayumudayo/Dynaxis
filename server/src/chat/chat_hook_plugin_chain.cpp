#include "chat_hook_plugin_chain.hpp"

#include "server/core/util/log.hpp"

#include <array>
#include <mutex>
#include <unordered_map>

/**
 * @brief 다중 chat-hook 플러그인 체인 재구성/적용 구현입니다.
 *
 * 체인 구성/재스캔/리로드 메커니즘은 core::plugin::PluginChainHost로 위임하고,
 * 여기서는 chat-hook ABI 호출 정책만 담당합니다.
 */
namespace server::app::chat {

namespace corelog = server::core::log;

namespace {

using OnChatSendV2Fn = HookDecisionV2 (CHAT_HOOK_CALL*)(void*, const ChatHookChatSendV2*, ChatHookChatSendOutV2*);

const ChatHookApiV2* adapt_v1_api_to_v2(const ChatHookApiV1* api_v1) {
    if (!api_v1) {
        return nullptr;
    }

    static std::mutex cache_mu;
    static std::unordered_map<const ChatHookApiV1*, ChatHookApiV2> cache;

    std::lock_guard<std::mutex> lock(cache_mu);
    auto [it, inserted] = cache.emplace(api_v1, ChatHookApiV2{});
    if (inserted) {
        ChatHookApiV2 adapted{};
        adapted.abi_version = CHAT_HOOK_ABI_VERSION_V2;
        adapted.name = api_v1->name;
        adapted.version = api_v1->version;
        adapted.create = api_v1->create;
        adapted.destroy = api_v1->destroy;
        adapted.on_chat_send = reinterpret_cast<OnChatSendV2Fn>(api_v1->on_chat_send);
        adapted.on_login = nullptr;
        adapted.on_join = nullptr;
        adapted.on_leave = nullptr;
        adapted.on_session_event = nullptr;
        adapted.on_admin_command = nullptr;
        it->second = adapted;
    }

    return &it->second;
}

server::core::plugin::PluginChainHost<ChatHookApiV2>::Config make_chain_host_config(ChatHookPluginChain::Config cfg) {
    server::core::plugin::PluginChainHost<ChatHookApiV2>::Config out{};
    out.plugin_paths = std::move(cfg.plugin_paths);
    out.plugins_dir = std::move(cfg.plugins_dir);
    out.fallback_plugins_dir = std::move(cfg.fallback_plugins_dir);
    out.cache_dir = std::move(cfg.cache_dir);
    out.single_lock_path = std::move(cfg.single_lock_path);
    out.entrypoint_symbol = "chat_hook_api_v2";
    out.fallback_entrypoint_symbols = {"chat_hook_api_v1"};

    out.api_resolver = [](void* symbol, std::string& error) -> const ChatHookApiV2* {
        using GetApiFn = const ChatHookApiV2* (CHAT_HOOK_CALL*)();
        auto get_api = reinterpret_cast<GetApiFn>(symbol);
        try {
            const ChatHookApiV2* api = get_api();
            if (!api) {
                error = "null api";
                return nullptr;
            }

            if (api->abi_version == CHAT_HOOK_ABI_VERSION_V2) {
                return api;
            }

            if (api->abi_version == CHAT_HOOK_ABI_VERSION_V1) {
                const auto* api_v1 = reinterpret_cast<const ChatHookApiV1*>(api);
                return adapt_v1_api_to_v2(api_v1);
            }

            error = "abi mismatch; expected v1 or v2";
            return nullptr;
        } catch (...) {
            error = "entrypoint threw exception";
            return nullptr;
        }
    };

    out.api_validator = [](const ChatHookApiV2* api, std::string& error) {
        if (!api) {
            error = "null api";
            return false;
        }
        if (api->abi_version != CHAT_HOOK_ABI_VERSION_V2) {
            error = "abi mismatch; expected v2";
            return false;
        }
        if (!api->on_chat_send) {
            error = "api.on_chat_send is null";
            return false;
        }
        return true;
    };

    out.instance_creator = [](const ChatHookApiV2* api, std::string& error) -> void* {
        if (!api || !api->create) {
            return nullptr;
        }
        try {
            return api->create();
        } catch (...) {
            error = "api.create threw exception";
            return nullptr;
        }
    };

    out.instance_destroyer = [](const ChatHookApiV2* api, void* instance) {
        if (!api || !api->destroy) {
            return;
        }
        api->destroy(instance);
    };

    return out;
}

void clamp_and_assign(const char* data,
                      const std::size_t capacity,
                      const std::uint32_t requested_size,
                      std::string& out_str) {
    out_str.clear();
    if (!data || capacity == 0) {
        return;
    }

    std::size_t n = static_cast<std::size_t>(requested_size);
    if (n >= capacity) {
        n = capacity - 1;
    }

    out_str.assign(data, data + n);
}

struct ChainCallResult {
    HookDecisionV2 decision{HookDecisionV2::kPass};
    std::string notice;
    std::string replacement_text;
};

struct GateCallResult {
    HookDecisionV2 decision{HookDecisionV2::kPass};
    std::string notice;
    std::string deny_reason;
};

struct AdminCallResult {
    HookDecisionV2 decision{HookDecisionV2::kPass};
    std::string notice;
    std::string response_json;
    std::string deny_reason;
};

ChainCallResult call_on_chat_send(
    const std::shared_ptr<const server::core::plugin::PluginHost<ChatHookApiV2>>& host,
    std::uint32_t session_id,
    std::string_view room,
    std::string_view user,
    std::string_view text) {
    ChainCallResult result{};
    if (!host) {
        return result;
    }

    auto mod = host->current();
    if (!mod || !mod->api || !mod->api->on_chat_send) {
        return result;
    }

    ChatHookChatSendV2 in{};
    in.session_id = session_id;
    std::string room_s(room);
    std::string user_s(user);
    std::string text_s(text);
    in.room = room_s.c_str();
    in.user = user_s.c_str();
    in.text = text_s.c_str();

    std::array<char, 512> notice_buf{};
    std::array<char, 1024> replace_buf{};
    HookStrBufV2 notice_out{notice_buf.data(), static_cast<std::uint32_t>(notice_buf.size()), 0};
    HookStrBufV2 replace_out{replace_buf.data(), static_cast<std::uint32_t>(replace_buf.size()), 0};
    ChatHookChatSendOutV2 out{notice_out, replace_out};

    HookDecisionV2 decision = HookDecisionV2::kPass;
    try {
        decision = mod->api->on_chat_send(mod->instance, &in, &out);
    } catch (const std::exception& ex) {
        corelog::warn(std::string("chat_hook: exception: ") + ex.what());
        decision = HookDecisionV2::kPass;
    } catch (...) {
        corelog::warn("chat_hook: unknown exception");
        decision = HookDecisionV2::kPass;
    }

    result.decision = decision;
    clamp_and_assign(notice_buf.data(), notice_buf.size(), out.notice.size, result.notice);
    clamp_and_assign(replace_buf.data(), replace_buf.size(), out.replacement_text.size, result.replacement_text);
    return result;
}

GateCallResult call_on_login(
    const std::shared_ptr<const server::core::plugin::PluginHost<ChatHookApiV2>>& host,
    std::uint32_t session_id,
    std::string_view user) {
    GateCallResult result{};
    if (!host) {
        return result;
    }

    auto mod = host->current();
    if (!mod || !mod->api || !mod->api->on_login) {
        return result;
    }

    LoginEventV2 in{};
    in.session_id = session_id;
    std::string user_s(user);
    in.user = user_s.c_str();

    std::array<char, 512> notice_buf{};
    std::array<char, 512> deny_buf{};
    HookStrBufV2 notice_out{notice_buf.data(), static_cast<std::uint32_t>(notice_buf.size()), 0};
    HookStrBufV2 deny_out{deny_buf.data(), static_cast<std::uint32_t>(deny_buf.size()), 0};
    LoginEventOutV2 out{notice_out, deny_out};

    HookDecisionV2 decision = HookDecisionV2::kPass;
    try {
        decision = mod->api->on_login(mod->instance, &in, &out);
    } catch (const std::exception& ex) {
        corelog::warn(std::string("chat_hook(on_login): exception: ") + ex.what());
        decision = HookDecisionV2::kPass;
    } catch (...) {
        corelog::warn("chat_hook(on_login): unknown exception");
        decision = HookDecisionV2::kPass;
    }

    result.decision = decision;
    clamp_and_assign(notice_buf.data(), notice_buf.size(), out.notice.size, result.notice);
    clamp_and_assign(deny_buf.data(), deny_buf.size(), out.deny_reason.size, result.deny_reason);
    return result;
}

GateCallResult call_on_join(
    const std::shared_ptr<const server::core::plugin::PluginHost<ChatHookApiV2>>& host,
    std::uint32_t session_id,
    std::string_view user,
    std::string_view room) {
    GateCallResult result{};
    if (!host) {
        return result;
    }

    auto mod = host->current();
    if (!mod || !mod->api || !mod->api->on_join) {
        return result;
    }

    JoinEventV2 in{};
    in.session_id = session_id;
    std::string user_s(user);
    std::string room_s(room);
    in.user = user_s.c_str();
    in.room = room_s.c_str();

    std::array<char, 512> notice_buf{};
    std::array<char, 512> deny_buf{};
    HookStrBufV2 notice_out{notice_buf.data(), static_cast<std::uint32_t>(notice_buf.size()), 0};
    HookStrBufV2 deny_out{deny_buf.data(), static_cast<std::uint32_t>(deny_buf.size()), 0};
    JoinEventOutV2 out{notice_out, deny_out};

    HookDecisionV2 decision = HookDecisionV2::kPass;
    try {
        decision = mod->api->on_join(mod->instance, &in, &out);
    } catch (const std::exception& ex) {
        corelog::warn(std::string("chat_hook(on_join): exception: ") + ex.what());
        decision = HookDecisionV2::kPass;
    } catch (...) {
        corelog::warn("chat_hook(on_join): unknown exception");
        decision = HookDecisionV2::kPass;
    }

    result.decision = decision;
    clamp_and_assign(notice_buf.data(), notice_buf.size(), out.notice.size, result.notice);
    clamp_and_assign(deny_buf.data(), deny_buf.size(), out.deny_reason.size, result.deny_reason);
    return result;
}

GateCallResult call_on_leave(
    const std::shared_ptr<const server::core::plugin::PluginHost<ChatHookApiV2>>& host,
    std::uint32_t session_id,
    std::string_view user,
    std::string_view room) {
    GateCallResult result{};
    if (!host) {
        return result;
    }

    auto mod = host->current();
    if (!mod || !mod->api || !mod->api->on_leave) {
        return result;
    }

    LeaveEventV2 in{};
    in.session_id = session_id;
    std::string user_s(user);
    std::string room_s(room);
    in.user = user_s.c_str();
    in.room = room_s.c_str();

    HookDecisionV2 decision = HookDecisionV2::kPass;
    try {
        decision = mod->api->on_leave(mod->instance, &in);
    } catch (const std::exception& ex) {
        corelog::warn(std::string("chat_hook(on_leave): exception: ") + ex.what());
        decision = HookDecisionV2::kPass;
    } catch (...) {
        corelog::warn("chat_hook(on_leave): unknown exception");
        decision = HookDecisionV2::kPass;
    }

    result.decision = decision;
    return result;
}

GateCallResult call_on_session_event(
    const std::shared_ptr<const server::core::plugin::PluginHost<ChatHookApiV2>>& host,
    std::uint32_t session_id,
    SessionEventKindV2 kind,
    std::string_view user,
    std::string_view reason) {
    GateCallResult result{};
    if (!host) {
        return result;
    }

    auto mod = host->current();
    if (!mod || !mod->api || !mod->api->on_session_event) {
        return result;
    }

    SessionEventV2 in{};
    in.session_id = session_id;
    in.kind = kind;
    std::string user_s(user);
    std::string reason_s(reason);
    in.user = user_s.c_str();
    in.reason = reason_s.c_str();

    HookDecisionV2 decision = HookDecisionV2::kPass;
    try {
        decision = mod->api->on_session_event(mod->instance, &in);
    } catch (const std::exception& ex) {
        corelog::warn(std::string("chat_hook(on_session_event): exception: ") + ex.what());
        decision = HookDecisionV2::kPass;
    } catch (...) {
        corelog::warn("chat_hook(on_session_event): unknown exception");
        decision = HookDecisionV2::kPass;
    }

    result.decision = decision;
    return result;
}

AdminCallResult call_on_admin_command(
    const std::shared_ptr<const server::core::plugin::PluginHost<ChatHookApiV2>>& host,
    std::string_view command,
    std::string_view issuer,
    std::string_view payload_json) {
    AdminCallResult result{};
    if (!host) {
        return result;
    }

    auto mod = host->current();
    if (!mod || !mod->api || !mod->api->on_admin_command) {
        return result;
    }

    AdminCommandV2 in{};
    std::string command_s(command);
    std::string issuer_s(issuer);
    std::string payload_s(payload_json);
    in.command = command_s.c_str();
    in.issuer = issuer_s.c_str();
    in.payload_json = payload_s.c_str();

    std::array<char, 512> notice_buf{};
    std::array<char, 2048> response_buf{};
    std::array<char, 512> deny_buf{};
    HookStrBufV2 notice_out{notice_buf.data(), static_cast<std::uint32_t>(notice_buf.size()), 0};
    HookStrBufV2 response_out{response_buf.data(), static_cast<std::uint32_t>(response_buf.size()), 0};
    HookStrBufV2 deny_out{deny_buf.data(), static_cast<std::uint32_t>(deny_buf.size()), 0};
    AdminCommandOutV2 out{notice_out, response_out, deny_out};

    HookDecisionV2 decision = HookDecisionV2::kPass;
    try {
        decision = mod->api->on_admin_command(mod->instance, &in, &out);
    } catch (const std::exception& ex) {
        corelog::warn(std::string("chat_hook(on_admin_command): exception: ") + ex.what());
        decision = HookDecisionV2::kPass;
    } catch (...) {
        corelog::warn("chat_hook(on_admin_command): unknown exception");
        decision = HookDecisionV2::kPass;
    }

    result.decision = decision;
    clamp_and_assign(notice_buf.data(), notice_buf.size(), out.notice.size, result.notice);
    clamp_and_assign(response_buf.data(), response_buf.size(), out.response_json.size, result.response_json);
    clamp_and_assign(deny_buf.data(), deny_buf.size(), out.deny_reason.size, result.deny_reason);
    return result;
}

} // namespace

ChatHookPluginChain::ChatHookPluginChain(Config cfg)
    : host_(make_chain_host_config(std::move(cfg))) {}

void ChatHookPluginChain::poll_reload() {
    host_.poll_reload();
}

ChatHookPluginChain::Outcome ChatHookPluginChain::on_chat_send(std::uint32_t session_id,
                                                               std::string_view room,
                                                               std::string_view user,
                                                               std::string& text) const {
    Outcome out{};

    auto ordered = host_.current_chain();
    if (!ordered) {
        return out;
    }

    for (const auto& host : *ordered) {
        const auto r = call_on_chat_send(host, session_id, room, user, text);
        if (!r.notice.empty()) {
            out.notices.push_back(r.notice);
        }

        switch (r.decision) {
        case HookDecisionV2::kPass:
        case HookDecisionV2::kAllow:
            break;
        case HookDecisionV2::kHandled:
            out.stop_default = true;
            return out;
        case HookDecisionV2::kModify:
            if (!r.replacement_text.empty()) {
                text = r.replacement_text;
            }
            break;
        case HookDecisionV2::kBlock:
        case HookDecisionV2::kDeny:
            out.stop_default = true;
            return out;
        default:
            break;
        }
    }

    return out;
}

ChatHookPluginChain::GateOutcome ChatHookPluginChain::on_login(std::uint32_t session_id, std::string_view user) const {
    GateOutcome out{};

    auto ordered = host_.current_chain();
    if (!ordered) {
        return out;
    }

    for (const auto& host : *ordered) {
        const auto r = call_on_login(host, session_id, user);
        if (!r.notice.empty()) {
            out.notices.push_back(r.notice);
        }

        switch (r.decision) {
        case HookDecisionV2::kPass:
        case HookDecisionV2::kAllow:
        case HookDecisionV2::kModify:
            break;
        case HookDecisionV2::kHandled:
            out.stop_default = true;
            return out;
        case HookDecisionV2::kBlock:
        case HookDecisionV2::kDeny:
            out.stop_default = true;
            out.deny_reason = r.deny_reason;
            return out;
        default:
            break;
        }
    }

    return out;
}

ChatHookPluginChain::GateOutcome ChatHookPluginChain::on_join(std::uint32_t session_id,
                                                              std::string_view user,
                                                              std::string_view room) const {
    GateOutcome out{};

    auto ordered = host_.current_chain();
    if (!ordered) {
        return out;
    }

    for (const auto& host : *ordered) {
        const auto r = call_on_join(host, session_id, user, room);
        if (!r.notice.empty()) {
            out.notices.push_back(r.notice);
        }

        switch (r.decision) {
        case HookDecisionV2::kPass:
        case HookDecisionV2::kAllow:
        case HookDecisionV2::kModify:
            break;
        case HookDecisionV2::kHandled:
            out.stop_default = true;
            return out;
        case HookDecisionV2::kBlock:
        case HookDecisionV2::kDeny:
            out.stop_default = true;
            out.deny_reason = r.deny_reason;
            return out;
        default:
            break;
        }
    }

    return out;
}

ChatHookPluginChain::GateOutcome ChatHookPluginChain::on_leave(std::uint32_t session_id,
                                                               std::string_view user,
                                                               std::string_view room) const {
    GateOutcome out{};

    auto ordered = host_.current_chain();
    if (!ordered) {
        return out;
    }

    for (const auto& host : *ordered) {
        const auto r = call_on_leave(host, session_id, user, room);
        if (!r.notice.empty()) {
            out.notices.push_back(r.notice);
        }

        switch (r.decision) {
        case HookDecisionV2::kPass:
        case HookDecisionV2::kAllow:
        case HookDecisionV2::kModify:
            break;
        case HookDecisionV2::kHandled:
            out.stop_default = true;
            return out;
        case HookDecisionV2::kBlock:
        case HookDecisionV2::kDeny:
            out.stop_default = true;
            return out;
        default:
            break;
        }
    }

    return out;
}

ChatHookPluginChain::GateOutcome ChatHookPluginChain::on_session_event(std::uint32_t session_id,
                                                                        SessionEventKindV2 kind,
                                                                        std::string_view user,
                                                                        std::string_view reason) const {
    GateOutcome out{};

    auto ordered = host_.current_chain();
    if (!ordered) {
        return out;
    }

    for (const auto& host : *ordered) {
        const auto r = call_on_session_event(host, session_id, kind, user, reason);
        if (!r.notice.empty()) {
            out.notices.push_back(r.notice);
        }

        switch (r.decision) {
        case HookDecisionV2::kPass:
        case HookDecisionV2::kAllow:
        case HookDecisionV2::kModify:
            break;
        case HookDecisionV2::kHandled:
            out.stop_default = true;
            return out;
        case HookDecisionV2::kBlock:
        case HookDecisionV2::kDeny:
            out.stop_default = true;
            return out;
        default:
            break;
        }
    }

    return out;
}

ChatHookPluginChain::AdminOutcome ChatHookPluginChain::on_admin_command(std::string_view command,
                                                                         std::string_view issuer,
                                                                         std::string_view payload_json) const {
    AdminOutcome out{};

    auto ordered = host_.current_chain();
    if (!ordered) {
        return out;
    }

    for (const auto& host : *ordered) {
        const auto r = call_on_admin_command(host, command, issuer, payload_json);
        if (!r.notice.empty()) {
            out.notices.push_back(r.notice);
        }

        if (!r.response_json.empty()) {
            out.response_json = r.response_json;
        }

        switch (r.decision) {
        case HookDecisionV2::kPass:
        case HookDecisionV2::kAllow:
        case HookDecisionV2::kModify:
            break;
        case HookDecisionV2::kHandled:
            out.stop_default = true;
            return out;
        case HookDecisionV2::kBlock:
        case HookDecisionV2::kDeny:
            out.stop_default = true;
            out.deny_reason = r.deny_reason;
            return out;
        default:
            break;
        }
    }

    return out;
}

ChatHookPluginChain::MetricsSnapshot ChatHookPluginChain::metrics_snapshot() const {
    MetricsSnapshot out{};

    const auto host_snap = host_.metrics_snapshot();
    out.configured = host_snap.configured;
    out.mode = host_snap.mode;

    auto ordered = host_.current_chain();
    if (!ordered) {
        return out;
    }

    out.plugins.reserve(ordered->size());
    for (const auto& host : *ordered) {
        if (!host) {
            continue;
        }

        const auto m = host->metrics_snapshot();
        ChatHookPluginChain::PluginMetricsSnapshot snap{};
        snap.plugin_path = m.plugin_path;
        snap.loaded = m.loaded;
        snap.reload_attempt_total = m.reload_attempt_total;
        snap.reload_success_total = m.reload_success_total;
        snap.reload_failure_total = m.reload_failure_total;

        auto mod = host->current();
        if (mod && mod->api) {
            if (mod->api->name) {
                snap.name = mod->api->name;
            }
            if (mod->api->version) {
                snap.version = mod->api->version;
            }
        }

        out.plugins.push_back(std::move(snap));
    }

    return out;
}

} // namespace server::app::chat
