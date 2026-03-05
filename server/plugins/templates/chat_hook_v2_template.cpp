#include "server/chat/chat_hook_plugin_abi.hpp"

namespace {

constexpr const char* kPluginName = "__PLUGIN_NAME__";
constexpr const char* kPluginVersion = "__PLUGIN_VERSION__";

HookDecisionV2 CHAT_HOOK_CALL on_chat_send(
    void*,
    const ChatHookChatSendV2*,
    ChatHookChatSendOutV2*) {
    return HookDecisionV2::kPass;
}

// __HOOK_STUBS__

static const ChatHookApiV2 kApi{
    CHAT_HOOK_ABI_VERSION_V2,
    kPluginName,
    kPluginVersion,
    nullptr,
    nullptr,
    &on_chat_send,
    /* __ON_LOGIN_PTR__ */ nullptr,
    /* __ON_JOIN_PTR__ */ nullptr,
    /* __ON_LEAVE_PTR__ */ nullptr,
    /* __ON_SESSION_EVENT_PTR__ */ nullptr,
    /* __ON_ADMIN_COMMAND_PTR__ */ nullptr,
};

} // namespace

extern "C" CHAT_HOOK_PLUGIN_EXPORT const ChatHookApiV2* CHAT_HOOK_CALL chat_hook_api_v2() {
    return &kApi;
}
