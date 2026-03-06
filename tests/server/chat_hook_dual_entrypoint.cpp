#include "server/chat/chat_hook_plugin_abi.hpp"

#include <cstdint>
#include <cstring>

namespace {

constexpr const char* kName = "chat_hook_dual_entrypoint";

void write_buf_v1(ChatHookStrBufV1* buf, const char* text) {
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

void write_buf_v2(HookStrBufV2* buf, const char* text) {
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

HookDecisionV2 CHAT_HOOK_CALL on_chat_send_v2(void*,
                                              const ChatHookChatSendV2* in,
                                              ChatHookChatSendOutV2* out) {
    if (!in || !out) {
        return HookDecisionV2::kPass;
    }

    const char* text = in->text ? in->text : "";
    if (std::strcmp(text, "/dual") == 0) {
        write_buf_v2(&out->notice, "[chat_hook_dual_entrypoint] handled via v2 entrypoint");
        return HookDecisionV2::kHandled;
    }

    return HookDecisionV2::kPass;
}

ChatHookDecisionV1 CHAT_HOOK_CALL on_chat_send_v1(void*,
                                                  const ChatHookChatSendV1* in,
                                                  ChatHookChatSendOutV1* out) {
    if (!in || !out) {
        return ChatHookDecisionV1::kPass;
    }

    const char* text = in->text ? in->text : "";
    if (std::strcmp(text, "/dual") == 0) {
        write_buf_v1(&out->notice, "[chat_hook_dual_entrypoint] handled via v1 entrypoint");
        return ChatHookDecisionV1::kHandled;
    }

    return ChatHookDecisionV1::kPass;
}

const ChatHookApiV2 g_api_v2{
    CHAT_HOOK_ABI_VERSION_V2,
    kName,
    "v2-entrypoint",
    nullptr,
    nullptr,
    &on_chat_send_v2,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

const ChatHookApiV1 g_api_v1{
    CHAT_HOOK_ABI_VERSION_V1,
    kName,
    "v1-entrypoint",
    nullptr,
    nullptr,
    &on_chat_send_v1,
};

} // namespace

extern "C" {

CHAT_HOOK_PLUGIN_EXPORT const ChatHookApiV2* CHAT_HOOK_CALL chat_hook_api_v2() {
    return &g_api_v2;
}

CHAT_HOOK_PLUGIN_EXPORT const ChatHookApiV1* CHAT_HOOK_CALL chat_hook_api_v1() {
    return &g_api_v1;
}

} // extern "C"
