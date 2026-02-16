#pragma once

#include <cstdint>

#if defined(_WIN32)
#  define CHAT_HOOK_CALL __cdecl
#else
#  define CHAT_HOOK_CALL
#endif

#if defined(_WIN32)
#  if defined(CHAT_HOOK_PLUGIN_BUILD)
#    define CHAT_HOOK_PLUGIN_EXPORT __declspec(dllexport)
#  else
#    define CHAT_HOOK_PLUGIN_EXPORT
#  endif
#else
#  define CHAT_HOOK_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {

/** @brief Chat hook ABI v1 버전 식별자입니다. */
static constexpr std::uint32_t CHAT_HOOK_ABI_VERSION_V1 = 1u;

/**
 * @brief Chat hook 플러그인이 기본 경로에 대해 내릴 수 있는 결정값입니다.
 */
enum class ChatHookDecisionV1 : std::uint32_t {
    kPass = 0,
    kHandled = 1,
    kReplaceText = 2,
    kBlock = 3,
};

/**
 * @brief 플러그인 문자열 출력 버퍼(view) 구조체입니다.
 */
struct ChatHookStrBufV1 {
    char* data;
    std::uint32_t capacity;
    std::uint32_t size;
};

/** @brief on_chat_send 입력 페이로드입니다. */
struct ChatHookChatSendV1 {
    std::uint32_t session_id;
    const char* room;
    const char* user;
    const char* text;
};

/** @brief on_chat_send 출력 페이로드입니다. */
struct ChatHookChatSendOutV1 {
    ChatHookStrBufV1 notice;
    ChatHookStrBufV1 replacement_text;
};

/**
 * @brief Chat hook 플러그인 v1 API 함수 테이블입니다.
 */
struct ChatHookApiV1 {
    std::uint32_t abi_version;
    const char* name;
    const char* version;

    void* (CHAT_HOOK_CALL* create)();
    void (CHAT_HOOK_CALL* destroy)(void* instance);

    ChatHookDecisionV1 (CHAT_HOOK_CALL* on_chat_send)(
        void* instance,
        const ChatHookChatSendV1* in,
        ChatHookChatSendOutV1* out);
};

/**
 * @brief 플러그인 엔트리포인트 함수입니다.
 * @return ChatHookApiV1 함수 테이블 포인터
 */
CHAT_HOOK_PLUGIN_EXPORT const ChatHookApiV1* CHAT_HOOK_CALL chat_hook_api_v1();

} // extern "C"
