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

/** @brief Chat hook ABI v1 버전 식별자입니다(레거시 폴백). */
static constexpr std::uint32_t CHAT_HOOK_ABI_VERSION_V1 = 1u;

/** @brief Chat hook ABI v2 버전 식별자입니다. */
static constexpr std::uint32_t CHAT_HOOK_ABI_VERSION_V2 = 2u;

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
 * @brief Chat hook v2에서 사용하는 통합 결정값입니다.
 *
 * hook별 유효 의미:
 * - `on_chat_send`: `kPass`, `kHandled`, `kModify`, `kBlock`(또는 `kDeny`)
 * - `on_login`/`on_join`: `kPass`/`kAllow`/`kHandled`/`kBlock`/`kDeny`
 * - `on_leave`/`on_session_event`: 관측 성격이므로 일반적으로 `kPass` 권장
 * - `on_admin_command`: `kPass`/`kHandled`/`kBlock`/`kDeny`
 */
enum class HookDecisionV2 : std::uint32_t {
    kPass = 0,
    kHandled = 1,
    kModify = 2,
    kBlock = 3,
    kAllow = 4,
    kDeny = 5,
};

/** @brief v2 문자열 출력 버퍼(view) 구조체입니다. */
struct HookStrBufV2 {
    char* data;
    std::uint32_t capacity;
    std::uint32_t size;
};

/** @brief v2 on_chat_send 입력 페이로드입니다. */
struct ChatHookChatSendV2 {
    std::uint32_t session_id;
    const char* room;
    const char* user;
    const char* text;
};

/** @brief v2 on_chat_send 출력 페이로드입니다. */
struct ChatHookChatSendOutV2 {
    HookStrBufV2 notice;
    HookStrBufV2 replacement_text;
};

/** @brief v2 on_login 입력 페이로드입니다. */
struct LoginEventV2 {
    std::uint32_t session_id;
    const char* user;
};

/** @brief v2 on_login 출력 페이로드입니다. */
struct LoginEventOutV2 {
    HookStrBufV2 notice;
    HookStrBufV2 deny_reason;
};

/** @brief v2 on_join 입력 페이로드입니다. */
struct JoinEventV2 {
    std::uint32_t session_id;
    const char* user;
    const char* room;
};

/** @brief v2 on_join 출력 페이로드입니다. */
struct JoinEventOutV2 {
    HookStrBufV2 notice;
    HookStrBufV2 deny_reason;
};

/** @brief v2 on_leave 입력 페이로드입니다. */
struct LeaveEventV2 {
    std::uint32_t session_id;
    const char* user;
    const char* room;
};

/** @brief v2 session 이벤트 타입입니다. */
enum class SessionEventKindV2 : std::uint32_t {
    kOpen = 0,
    kClose = 1,
};

/** @brief v2 on_session_event 입력 페이로드입니다. */
struct SessionEventV2 {
    std::uint32_t session_id;
    SessionEventKindV2 kind;
    const char* user;
    const char* reason;
};

/** @brief v2 on_admin_command 입력 페이로드입니다. */
struct AdminCommandV2 {
    const char* command;
    const char* issuer;
    const char* payload_json;
};

/** @brief v2 on_admin_command 출력 페이로드입니다. */
struct AdminCommandOutV2 {
    HookStrBufV2 notice;
    HookStrBufV2 response_json;
    HookStrBufV2 deny_reason;
};

/**
 * @brief Chat hook 플러그인 v2 API 함수 테이블입니다.
 *
 * 로더는 `chat_hook_api_v2()`를 우선 탐색합니다.
 * 개별 hook 포인터가 `nullptr`이면 해당 hook은 "미구현(=pass)"으로 취급됩니다.
 */
struct ChatHookApiV2 {
    std::uint32_t abi_version;
    const char* name;
    const char* version;

    void* (CHAT_HOOK_CALL* create)();
    void (CHAT_HOOK_CALL* destroy)(void* instance);

    HookDecisionV2 (CHAT_HOOK_CALL* on_chat_send)(
        void* instance,
        const ChatHookChatSendV2* in,
        ChatHookChatSendOutV2* out);

    HookDecisionV2 (CHAT_HOOK_CALL* on_login)(
        void* instance,
        const LoginEventV2* in,
        LoginEventOutV2* out);

    HookDecisionV2 (CHAT_HOOK_CALL* on_join)(
        void* instance,
        const JoinEventV2* in,
        JoinEventOutV2* out);

    HookDecisionV2 (CHAT_HOOK_CALL* on_leave)(
        void* instance,
        const LeaveEventV2* in);

    HookDecisionV2 (CHAT_HOOK_CALL* on_session_event)(
        void* instance,
        const SessionEventV2* in);

    HookDecisionV2 (CHAT_HOOK_CALL* on_admin_command)(
        void* instance,
        const AdminCommandV2* in,
        AdminCommandOutV2* out);
};

/**
 * @brief 레거시 폴백 엔트리포인트 함수입니다.
 * @return ChatHookApiV1 함수 테이블 포인터
 */
CHAT_HOOK_PLUGIN_EXPORT const ChatHookApiV1* CHAT_HOOK_CALL chat_hook_api_v1();

/**
 * @brief v2 우선 로더에서 먼저 탐색하는 엔트리포인트 함수입니다.
 * @return ChatHookApiV2 함수 테이블 포인터
 */
CHAT_HOOK_PLUGIN_EXPORT const ChatHookApiV2* CHAT_HOOK_CALL chat_hook_api_v2();

} // extern "C"
