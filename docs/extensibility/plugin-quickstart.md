# 플러그인 Quickstart (ChatHook ABI v2)

이 문서는 `server_app`용 네이티브 플러그인(ABI v2)을 30분 내에 작성/배포/롤백하는 최소 절차를 설명한다.

관련 문서:
- `server/include/server/chat/chat_hook_plugin_abi.hpp`
- `server/plugins/chat_hook_sample.cpp`
- `docs/core-api/extensions.md`
- `docs/extensibility/governance.md`
- `docs/ops/plugin-script-operations.md`

## 1) 30분 온보딩 경로

1. 스캐폴드 도구로 기본 파일을 생성한다.
   - `python tools/new_plugin.py --name spam_filter --hook on_chat_send --hook on_join`
   - 생성물: `server/plugins/<priority>_<name>.cpp`, `server/plugins/<priority>_<name>.plugin.json`
   - 수동 시작이 필요하면 `server/plugins/chat_hook_sample.cpp`를 복사해도 된다.
2. `chat_hook_api_v2()`의 `name`, `version`, 필요한 hook 포인터를 수정한다.
   - 현재 로더 validator는 `on_chat_send != nullptr`를 요구한다.
3. Docker stack을 띄우고 플러그인 디렉터리에 배포한다.
4. `/metrics`에서 reload/call 메트릭을 확인한다.
5. 이상 시 이전 바이너리로 즉시 롤백한다.

## 2) 최소 플러그인 스켈레톤

```cpp
// server/plugins/chat_hook_hello.cpp
#include "server/chat/chat_hook_plugin_abi.hpp"

#include <cstring>

namespace {

void* create_instance() {
    return nullptr;
}

void destroy_instance(void*) {
}

HookDecisionV2 on_chat_send(void*, const ChatHookChatSendV2*, ChatHookChatSendOutV2*) {
    return HookDecisionV2::kPass;
}

HookDecisionV2 on_login(void*, const LoginEventV2* in, LoginEventOutV2* out) {
    if (!in || !out) {
        return HookDecisionV2::kPass;
    }

    if (std::strcmp(in->user, "blocked_user") == 0) {
        const char* reason = "blocked by hello plugin";
        if (out->deny_reason.data && out->deny_reason.capacity > 0) {
            const std::size_t n = std::min<std::size_t>(std::strlen(reason), out->deny_reason.capacity - 1);
            std::memcpy(out->deny_reason.data, reason, n);
            out->deny_reason.data[n] = '\0';
            out->deny_reason.size = static_cast<std::uint32_t>(n);
        }
        return HookDecisionV2::kDeny;
    }

    return HookDecisionV2::kPass;
}

const ChatHookApiV2 k_api = {
    CHAT_HOOK_ABI_VERSION_V2,
    "hello_plugin",
    "1.0.0",
    &create_instance,
    &destroy_instance,
    &on_chat_send,
    &on_login,
    nullptr,   // on_join
    nullptr,   // on_leave
    nullptr,   // on_session_event
    nullptr,   // on_admin_command
};

} // namespace

extern "C" CHAT_HOOK_PLUGIN_EXPORT const ChatHookApiV2* CHAT_HOOK_CALL chat_hook_api_v2() {
    return &k_api;
}
```

## 3) 빌드/배포

1. 플러그인 타깃을 CMake에 추가한다(기존 샘플 타깃 패턴과 동일).
2. 결과 `.dll/.so`를 `CHAT_HOOK_PLUGINS_DIR` 경로에 배포한다.
3. 파일명 접두사(`10_`, `20_`)로 체인 순서를 제어한다.

Docker 기준 빠른 확인:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build
docker exec dynaxis-stack-server-1-1 sh -lc "ls -la /app/plugins"
docker logs dynaxis-stack-server-1-1 --since 5m
```

## 4) 검증 포인트

- 기능: 의도한 hook에서만 decision이 적용되는지 확인
- 관측: 아래 메트릭 증가 확인
  - `plugin_reload_attempt_total`
  - `plugin_reload_success_total`
  - `plugin_hook_calls_total`
  - `plugin_hook_errors_total`
  - `plugin_hook_duration_seconds`

## 5) 롤백

1. 이전 정상 바이너리로 동일 파일명 교체
2. reload 성공 로그/메트릭 확인
3. 장애 지속 시 플러그인 전체 우회는 `CHAT_HOOK_PLUGINS_DIR`와 `CHAT_HOOK_FALLBACK_PLUGINS_DIR`를 함께 비활성 경로로 설정 후 재기동

자세한 운영 절차는 `docs/ops/plugin-script-operations.md`를 따른다.
