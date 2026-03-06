#include "chat_hook_plugin_manager.hpp"

#include "server/core/util/log.hpp"

#include <array>
#include <mutex>
#include <system_error>
#include <unordered_map>

/**
 * @brief 단일 chat-hook 플러그인 로드/호출/hot-reload 구현입니다.
 *
 * 실제 로딩/리로드 메커니즘은 core::plugin::PluginHost로 위임하고,
 * 이 매니저는 chat-hook ABI 호출과 결과 변환만 담당합니다.
 */
namespace server::app::chat {

namespace corelog = server::core::log;

namespace {

std::filesystem::path make_default_lock_path(const std::filesystem::path& plugin_path) {
    const auto dir = plugin_path.parent_path();
    const auto stem = plugin_path.stem().string();
    return dir / (stem + "_LOCK");
}

server::core::plugin::PluginHost<ChatHookApiV2>::Config make_host_config(ChatHookPluginManager::Config cfg) {
    if (cfg.lock_path.has_value() && cfg.lock_path->empty()) {
        cfg.lock_path.reset();
    }
    if (cfg.cache_dir.empty()) {
        std::error_code ec;
        cfg.cache_dir = std::filesystem::temp_directory_path(ec) / "chat_hook_cache";
    }
    if (!cfg.lock_path.has_value() && !cfg.plugin_path.empty()) {
        cfg.lock_path = make_default_lock_path(cfg.plugin_path);
    }

    using OnChatSendV2Fn = HookDecisionV2 (CHAT_HOOK_CALL*)(void*, const ChatHookChatSendV2*, ChatHookChatSendOutV2*);

    const auto adapt_v1_api_to_v2 = [](const ChatHookApiV1* api_v1) -> const ChatHookApiV2* {
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
    };

    server::core::plugin::PluginHost<ChatHookApiV2>::Config host_cfg{};
    host_cfg.plugin_path = std::move(cfg.plugin_path);
    host_cfg.cache_dir = std::move(cfg.cache_dir);
    host_cfg.lock_path = std::move(cfg.lock_path);
    host_cfg.entrypoint_symbol = "chat_hook_api_v2";
    host_cfg.fallback_entrypoint_symbols = {"chat_hook_api_v1"};

    host_cfg.api_resolver = [adapt_v1_api_to_v2](void* symbol, std::string& error) -> const ChatHookApiV2* {
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

    host_cfg.api_validator = [](const ChatHookApiV2* api, std::string& error) {
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

    host_cfg.instance_creator = [](const ChatHookApiV2* api, std::string& error) -> void* {
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

    host_cfg.instance_destroyer = [](const ChatHookApiV2* api, void* instance) {
        if (!api || !api->destroy) {
            return;
        }
        api->destroy(instance);
    };

    return host_cfg;
}

} // namespace

ChatHookPluginManager::ChatHookPluginManager(Config cfg)
    : host_(make_host_config(std::move(cfg))) {}

void ChatHookPluginManager::poll_reload() {
    host_.poll_reload();
}

ChatHookPluginManager::Result ChatHookPluginManager::on_chat_send(std::uint32_t session_id,
                                                                   std::string_view room,
                                                                   std::string_view user,
                                                                   std::string_view text) const {
    Result result{};

    auto mod = host_.current();
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

    switch (decision) {
    case HookDecisionV2::kPass:
    case HookDecisionV2::kAllow:
        result.decision = ChatHookDecisionV1::kPass;
        break;
    case HookDecisionV2::kHandled:
        result.decision = ChatHookDecisionV1::kHandled;
        break;
    case HookDecisionV2::kModify:
        result.decision = ChatHookDecisionV1::kReplaceText;
        break;
    case HookDecisionV2::kBlock:
    case HookDecisionV2::kDeny:
        result.decision = ChatHookDecisionV1::kBlock;
        break;
    default:
        result.decision = ChatHookDecisionV1::kPass;
        break;
    }

    const auto clamp_and_assign = [](const char* data,
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
    };

    clamp_and_assign(notice_buf.data(), notice_buf.size(), out.notice.size, result.notice);
    clamp_and_assign(replace_buf.data(), replace_buf.size(), out.replacement_text.size, result.replacement_text);
    return result;
}

ChatHookPluginManager::MetricsSnapshot ChatHookPluginManager::metrics_snapshot() const {
    MetricsSnapshot snap{};
    const auto host_snap = host_.metrics_snapshot();
    snap.plugin_path = host_snap.plugin_path;
    snap.loaded = host_snap.loaded;
    snap.reload_attempt_total = host_snap.reload_attempt_total;
    snap.reload_success_total = host_snap.reload_success_total;
    snap.reload_failure_total = host_snap.reload_failure_total;

    auto mod = host_.current();
    if (!mod || !mod->api) {
        return snap;
    }
    if (mod->api->name) {
        snap.name = mod->api->name;
    }
    if (mod->api->version) {
        snap.version = mod->api->version;
    }
    return snap;
}

} // namespace server::app::chat
