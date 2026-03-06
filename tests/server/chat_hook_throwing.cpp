#include "server/chat/chat_hook_plugin_abi.hpp"

#include <stdexcept>

namespace {

void* CHAT_HOOK_CALL create_instance() {
    return nullptr;
}

void CHAT_HOOK_CALL destroy_instance(void*) {}

HookDecisionV2 CHAT_HOOK_CALL on_chat_send_throwing(void*,
                                                    const ChatHookChatSendV2*,
                                                    ChatHookChatSendOutV2*) {
    throw std::runtime_error("intentional exception from test plugin");
}

} // namespace

extern "C" CHAT_HOOK_PLUGIN_EXPORT const ChatHookApiV2* CHAT_HOOK_CALL chat_hook_api_v2() {
    static ChatHookApiV2 api{
        CHAT_HOOK_ABI_VERSION_V2,
        "chat_hook_throwing",
        "v2",
        &create_instance,
        &destroy_instance,
        &on_chat_send_throwing,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
    };
    return &api;
}
