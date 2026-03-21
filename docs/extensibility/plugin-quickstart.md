# 플러그인 빠른 시작 (ChatHook ABI v2)

이 문서는 `server_app`용 네이티브 플러그인(ABI v2)을 짧은 시간 안에 작성, 배포, 검증, 롤백하는 최소 절차를 설명한다. 네이티브 플러그인은 Lua보다 빠르고 hot-path에 더 가깝게 붙을 수 있지만, 그만큼 ABI 의미와 배포 순서를 더 엄격하게 지켜야 한다. 따라서 "코드만 돌아가면 된다"보다 "기존 체인과 충돌 없이 안전하게 들어가고 빠질 수 있느냐"가 더 중요하다.

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
3. Docker stack을 띄운 뒤 플러그인 디렉터리에 배포한다.
4. `/metrics`에서 reload와 call 메트릭을 확인한다.
5. 이상이 있으면 이전 바이너리로 즉시 롤백한다.

이 순서를 지키는 이유는 플러그인이 "로드되었다"와 "의도한 hook 의미로 안전하게 동작한다"가 전혀 다른 문제이기 때문이다. 스캐폴드, ABI 확인, 배포, 관측, 롤백 순서를 분리해 두면 장애 원인을 더 빠르게 좁힐 수 있다.

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

이 스켈레톤이 작은 이유는 의도적이다. 처음부터 여러 hook과 상태를 한 번에 얹으면, ABI 문제와 정책 문제와 기능 문제를 동시에 디버그해야 한다. 최소 스켈레톤으로 loader, validator, 기본 decision 경로부터 확인하는 편이 유지보수에 훨씬 낫다.

## 3) 빌드와 배포

1. 플러그인 타깃을 CMake에 추가한다. 기존 샘플 타깃 패턴을 그대로 따르는 것이 안전하다.
2. 결과 `.dll/.so`를 `CHAT_HOOK_PLUGINS_DIR` 경로에 배포한다.
3. 파일명 접두사(`10_`, `20_`)로 체인 순서를 제어한다.

Docker 기준 빠른 확인:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build
docker exec dynaxis-stack-server-1-1 sh -lc "ls -la /app/plugins"
docker logs dynaxis-stack-server-1-1 --since 5m
```

파일명 접두사로 순서를 제어하는 이유는 체인 순서가 곧 정책 우선순위이기 때문이다. 순서 규칙을 느슨하게 두면 플러그인이 늘어날수록 "왜 이번엔 다른 결과가 나왔는가"를 설명하기 어려워진다.

## 4) 검증 포인트

- 기능: 의도한 hook에서만 decision이 적용되는지 확인한다.
- 관측: 아래 메트릭이 기대한 시점에 증가하는지 확인한다.
  - `plugin_reload_attempt_total`
  - `plugin_reload_success_total`
  - `plugin_hook_calls_total`
  - `plugin_hook_errors_total`
  - `plugin_hook_duration_seconds`

메트릭을 같이 보는 이유는 기능 테스트만으로는 reload 실패, 반복 오류, 지연 증가를 놓치기 쉽기 때문이다. 플러그인은 코드가 짧아도 운영 영향은 클 수 있으므로, 기능과 관측을 함께 확인해야 한다.

## 5) 롤백

1. 이전 정상 바이너리로 동일 파일명을 교체한다.
2. reload 성공 로그와 메트릭을 확인한다.
3. 장애가 계속되면 `CHAT_HOOK_PLUGINS_DIR`와 `CHAT_HOOK_FALLBACK_PLUGINS_DIR`를 함께 비활성 경로로 바꾼 뒤 재기동해 전체 우회한다.

롤백에서 "동일 파일명"을 유지하는 이유는 체인 순서와 inventory 해석을 함께 보존하기 위해서다. 파일명을 바꾸며 되돌리면 문제를 되돌린 것인지 순서를 바꿔 버린 것인지 구분이 흐려진다.

자세한 운영 절차는 `docs/ops/plugin-script-operations.md`를 따른다.
