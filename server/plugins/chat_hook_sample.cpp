#include "server/chat/chat_hook_plugin_abi.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>

/**
 * @brief 예제 chat-hook 플러그인(sample) 구현입니다.
 *
 * `kPass/kHandled/kReplaceText/kBlock` 동작 예시를 제공해,
 * 운영 플러그인 작성 시 ABI 사용법을 빠르게 검증할 수 있게 합니다.
 */
namespace {

constexpr const char* kName = "chat_hook_sample";

#if defined(CHAT_HOOK_SAMPLE_V2)
constexpr const char* kVersion = "v2";
#else
constexpr const char* kVersion = "v1";
#endif

template <typename StrBuf>
static void write_buf(StrBuf* b, const char* s) {
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

static bool starts_with(const char* s, const char* prefix) {
    if (!s || !prefix) {
        return false;
    }
    const std::size_t n = std::strlen(prefix);
    return std::strncmp(s, prefix, n) == 0;
}

static bool contains_case_insensitive(const char* s, const char* needle) {
    if (!s || !needle) {
        return false;
    }
    const std::size_t n = std::strlen(needle);
    if (n == 0) {
        return true;
    }
    for (const char* p = s; *p; ++p) {
        std::size_t i = 0;
        while (i < n) {
            const unsigned char a = static_cast<unsigned char>(p[i]);
            const unsigned char b = static_cast<unsigned char>(needle[i]);
            if (a == 0) {
                break;
            }
            if (std::tolower(a) != std::tolower(b)) {
                break;
            }
            ++i;
        }
        if (i == n) {
            return true;
        }
    }
    return false;
}

template <typename StrBuf>
static void write_upper_ascii(StrBuf* b, const char* s) {
    if (!b || !b->data || b->capacity == 0) {
        return;
    }
    if (!s) {
        b->size = 0;
        b->data[0] = '\0';
        return;
    }

    const std::size_t cap = static_cast<std::size_t>(b->capacity);
    std::size_t out = 0;
    for (const char* p = s; *p && out + 1 < cap; ++p) {
        unsigned char c = static_cast<unsigned char>(*p);
        if (c >= 'a' && c <= 'z') {
            c = static_cast<unsigned char>(c - 'a' + 'A');
        }
        b->data[out++] = static_cast<char>(c);
    }
    b->data[out] = '\0';
    b->size = static_cast<std::uint32_t>(out);
}

} // namespace

extern "C" {

static ChatHookDecisionV1 CHAT_HOOK_CALL on_chat_send_v1(void*,
                                                         const ChatHookChatSendV1* in,
                                                         ChatHookChatSendOutV1* out) {
    if (!in || !out) {
        return ChatHookDecisionV1::kPass;
    }

    const char* text = in->text ? in->text : "";

    if (std::strcmp(text, "/plugin") == 0) {
        char msg[192]{};
        (void)std::snprintf(msg, sizeof(msg), "[%s] active version=%s", kName, kVersion);
        write_buf(&out->notice, msg);
        return ChatHookDecisionV1::kHandled;
    }

    if (starts_with(text, "/shout ")) {
        const char* rest = text + std::strlen("/shout ");
        write_upper_ascii(&out->replacement_text, rest);
        return ChatHookDecisionV1::kReplaceText;
    }

    if (contains_case_insensitive(text, "banana")) {
#if defined(CHAT_HOOK_SAMPLE_V2)
        write_buf(&out->notice, "[chat_hook_sample v2] filtered: banana -> apple");
        write_buf(&out->replacement_text, "apple");
        return ChatHookDecisionV1::kReplaceText;
#else
        write_buf(&out->notice, "[chat_hook_sample v1] blocked: contains 'banana'");
        return ChatHookDecisionV1::kBlock;
#endif
    }

    return ChatHookDecisionV1::kPass;
}

#if defined(CHAT_HOOK_SAMPLE_V2)
static HookDecisionV2 CHAT_HOOK_CALL on_chat_send_v2(void*,
                                                     const ChatHookChatSendV2* in,
                                                     ChatHookChatSendOutV2* out) {
    if (!in || !out) {
        return HookDecisionV2::kPass;
    }

    const char* text = in->text ? in->text : "";

    if (std::strcmp(text, "/plugin") == 0) {
        char msg[192]{};
        (void)std::snprintf(msg, sizeof(msg), "[%s] active version=%s", kName, kVersion);
        write_buf(&out->notice, msg);
        return HookDecisionV2::kHandled;
    }

    if (starts_with(text, "/shout ")) {
        const char* rest = text + std::strlen("/shout ");
        write_upper_ascii(&out->replacement_text, rest);
        return HookDecisionV2::kModify;
    }

    if (contains_case_insensitive(text, "banana")) {
        write_buf(&out->notice, "[chat_hook_sample v2] filtered: banana -> apple");
        write_buf(&out->replacement_text, "apple");
        return HookDecisionV2::kModify;
    }

    return HookDecisionV2::kPass;
}

static HookDecisionV2 CHAT_HOOK_CALL on_login_v2(void*, const LoginEventV2* in, LoginEventOutV2* out) {
    if (!in || !out) {
        return HookDecisionV2::kPass;
    }

    const char* user = in->user ? in->user : "";
    if (std::strcmp(user, "sample_deny_login") == 0) {
        write_buf(&out->notice, "[chat_hook_sample v2] login denied by sample policy");
        write_buf(&out->deny_reason, "sample policy denied login");
        return HookDecisionV2::kDeny;
    }

    if (std::strcmp(user, "sample_login_notice") == 0) {
        write_buf(&out->notice, "[chat_hook_sample v2] login hook observed user");
    }
    return HookDecisionV2::kPass;
}
#endif

static const ChatHookApiV1 g_api{
    CHAT_HOOK_ABI_VERSION_V1,
    kName,
    kVersion,
    nullptr,
    nullptr,
    &on_chat_send_v1,
};

#if defined(CHAT_HOOK_SAMPLE_V2)
static const ChatHookApiV2 g_api_v2{
    CHAT_HOOK_ABI_VERSION_V2,
    kName,
    kVersion,
    nullptr,
    nullptr,
    &on_chat_send_v2,
    &on_login_v2,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

CHAT_HOOK_PLUGIN_EXPORT const ChatHookApiV2* CHAT_HOOK_CALL chat_hook_api_v2() {
    return &g_api_v2;
}
#endif

CHAT_HOOK_PLUGIN_EXPORT const ChatHookApiV1* CHAT_HOOK_CALL chat_hook_api_v1() {
    return &g_api;
}

} // extern "C"
