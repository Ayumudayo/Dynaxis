#include "server/chat/chat_hook_plugin_abi.hpp"

#include <cstdint>
#include <cstring>

namespace {

constexpr const char* kName = "chat_hook_v2_only";
constexpr const char* kVersion = "v2";

void write_buf(HookStrBufV2* buf, const char* text) {
    if (!buf || !buf->data || buf->capacity == 0) {
        return;
    }

    if (!text) {
        buf->size = 0;
        buf->data[0] = '\0';
        return;
    }

    const std::size_t cap = static_cast<std::size_t>(buf->capacity);
    std::size_t n = std::strlen(text);
    if (n >= cap) {
        n = cap - 1;
    }

    if (n != 0) {
        std::memcpy(buf->data, text, n);
    }
    buf->data[n] = '\0';
    buf->size = static_cast<std::uint32_t>(n);
}

HookDecisionV2 CHAT_HOOK_CALL on_chat_send(void*,
                                           const ChatHookChatSendV2* in,
                                           ChatHookChatSendOutV2* out) {
    if (!in || !out) {
        return HookDecisionV2::kPass;
    }

    const char* text = in->text ? in->text : "";
    if (std::strcmp(text, "/v2only") == 0) {
        write_buf(&out->notice, "[chat_hook_v2_only] handled via v2 entrypoint");
        return HookDecisionV2::kHandled;
    }

    return HookDecisionV2::kPass;
}

HookDecisionV2 CHAT_HOOK_CALL on_login(void*, const LoginEventV2* in, LoginEventOutV2* out) {
    if (!in || !out) {
        return HookDecisionV2::kPass;
    }

    const char* user = in->user ? in->user : "";
    if (std::strcmp(user, "deny_login") == 0) {
        write_buf(&out->deny_reason, "login blocked by v2-only test plugin");
        write_buf(&out->notice, "[chat_hook_v2_only] login blocked");
        return HookDecisionV2::kDeny;
    }

    return HookDecisionV2::kPass;
}

HookDecisionV2 CHAT_HOOK_CALL on_join(void*, const JoinEventV2* in, JoinEventOutV2* out) {
    if (!in || !out) {
        return HookDecisionV2::kPass;
    }

    const char* room = in->room ? in->room : "";
    if (std::strcmp(room, "forbidden_room") == 0) {
        write_buf(&out->deny_reason, "join blocked by v2-only test plugin");
        write_buf(&out->notice, "[chat_hook_v2_only] join blocked");
        return HookDecisionV2::kDeny;
    }

    return HookDecisionV2::kPass;
}

HookDecisionV2 CHAT_HOOK_CALL on_leave(void*, const LeaveEventV2* in) {
    if (!in) {
        return HookDecisionV2::kPass;
    }

    const char* room = in->room ? in->room : "";
    if (std::strcmp(room, "locked_leave") == 0) {
        return HookDecisionV2::kDeny;
    }
    return HookDecisionV2::kPass;
}

HookDecisionV2 CHAT_HOOK_CALL on_session_event(void*, const SessionEventV2* in) {
    if (!in) {
        return HookDecisionV2::kPass;
    }
    return HookDecisionV2::kPass;
}

HookDecisionV2 CHAT_HOOK_CALL on_admin_command(void*,
                                               const AdminCommandV2* in,
                                               AdminCommandOutV2* out) {
    if (!in || !out) {
        return HookDecisionV2::kPass;
    }

    const char* command = in->command ? in->command : "";
    if (std::strcmp(command, "disconnect_users") == 0) {
        write_buf(&out->deny_reason, "disconnect denied by v2-only test plugin");
        write_buf(&out->notice, "[chat_hook_v2_only] admin deny");
        return HookDecisionV2::kDeny;
    }

    return HookDecisionV2::kPass;
}

const ChatHookApiV2 g_api{
    CHAT_HOOK_ABI_VERSION_V2,
    kName,
    kVersion,
    nullptr,
    nullptr,
    &on_chat_send,
    &on_login,
    &on_join,
    &on_leave,
    &on_session_event,
    &on_admin_command,
};

} // namespace

extern "C" {

CHAT_HOOK_PLUGIN_EXPORT const ChatHookApiV2* CHAT_HOOK_CALL chat_hook_api_v2() {
    return &g_api;
}

} // extern "C"
