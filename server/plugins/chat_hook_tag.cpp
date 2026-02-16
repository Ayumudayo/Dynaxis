#include "server/chat/chat_hook_plugin_abi.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

/**
 * @brief 메시지 앞에 tag를 붙이는 예제 플러그인 구현입니다.
 *
 * 간단한 명령(`/tag`) 기반 텍스트 변환 시나리오를 통해,
 * replacement buffer 사용 규칙과 길이 제한 처리를 보여줍니다.
 */
namespace {

constexpr const char* kName = "chat_hook_tag";
constexpr const char* kVersion = "v1";

static bool starts_with(const char* s, const char* prefix) {
    if (!s || !prefix) {
        return false;
    }
    const std::size_t n = std::strlen(prefix);
    return std::strncmp(s, prefix, n) == 0;
}

static void write_buf(ChatHookStrBufV1* b, const char* s) {
    if (!b || !b->data || b->capacity == 0) {
        return;
    }
    if (!s) {
        b->size = 0;
        b->data[0] = '\0';
        return;
    }

    const std::size_t cap = static_cast<std::size_t>(b->capacity);
    std::size_t n = std::strlen(s);
    if (n >= cap) {
        n = cap - 1;
    }
    if (n != 0) {
        std::memcpy(b->data, s, n);
    }
    b->data[n] = '\0';
    b->size = static_cast<std::uint32_t>(n);
}

static void write_prefixed(ChatHookStrBufV1* b, const char* prefix, const char* text) {
    if (!b || !b->data || b->capacity == 0) {
        return;
    }
    const char* p = prefix ? prefix : "";
    const char* t = text ? text : "";

    const std::size_t cap = static_cast<std::size_t>(b->capacity);
    const int n = std::snprintf(b->data, cap, "%s%s", p, t);
    if (n < 0) {
        b->size = 0;
        b->data[0] = '\0';
        return;
    }
    std::uint32_t used = static_cast<std::uint32_t>(n);
    if (used >= b->capacity) {
        used = b->capacity - 1;
    }
    b->data[used] = '\0';
    b->size = used;
}

} // namespace

extern "C" {

static ChatHookDecisionV1 CHAT_HOOK_CALL on_chat_send(void*,
                                                      const ChatHookChatSendV1* in,
                                                      ChatHookChatSendOutV1* out) {
    if (!in || !out) {
        return ChatHookDecisionV1::kPass;
    }

    const char* text = in->text ? in->text : "";

    if (std::strcmp(text, "/tag") == 0) {
        write_buf(&out->notice, "usage: /tag <text>");
        return ChatHookDecisionV1::kHandled;
    }

    if (starts_with(text, "/tag ")) {
        const char* rest = text + std::strlen("/tag ");
        while (*rest == ' ') {
            ++rest;
        }
        if (*rest == '\0') {
            write_buf(&out->notice, "usage: /tag <text>");
            return ChatHookDecisionV1::kHandled;
        }

        write_prefixed(&out->replacement_text, "[tag] ", rest);
        return ChatHookDecisionV1::kReplaceText;
    }

    return ChatHookDecisionV1::kPass;
}

static const ChatHookApiV1 g_api{
    CHAT_HOOK_ABI_VERSION_V1,
    kName,
    kVersion,
    nullptr,
    nullptr,
    &on_chat_send,
};

CHAT_HOOK_PLUGIN_EXPORT const ChatHookApiV1* CHAT_HOOK_CALL chat_hook_api_v1() {
    return &g_api;
}

} // extern "C"
