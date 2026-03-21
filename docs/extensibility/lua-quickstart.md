# Lua 빠른 시작 (Cold Hook)

이 문서는 `server_app`의 Lua cold hook을 현재 작성 모델에 맞춰 빠르게 만들고, 검증하고, 필요하면 바로 롤백하는 절차를 설명한다.

여기서 "빠른 시작"은 단순히 파일 하나를 만드는 방법만 뜻하지 않는다. Lua 스크립트는 운영 중 바로 로드되는 artifact이므로, 작성과 검증, 롤백을 한 묶음으로 이해해야 한다. 이 세 단계를 분리해서 보면 가장 흔한 실수가 생긴다. 스크립트는 만들었지만 reload가 실제로 됐는지 확인하지 않거나, 오류가 날 때 어떤 메트릭을 보고 꺼야 하는지 모른 채 배포하게 된다.

핵심 원칙은 다음과 같다.

- 기본 작성 형태는 function-style hook + `ctx`다.
- `server/scripts/`는 런타임 이미지의 builtin fallback(`/app/scripts_builtin`) 소스다.
- `docker/stack/scripts/`는 Docker stack mount(`/app/scripts`) 경로다.
- 겹치는 샘플 이름은 builtin과 stack 동작이 어긋나지 않도록 같은 내용으로 유지한다.
- directive/return-table은 호환성 유지와 테스트 보조 용도로만 남긴다.

이 원칙이 필요한 이유는, 작성 모델이 여러 개로 흩어지면 문서와 실제 런타임 동작이 빠르게 어긋나기 때문이다. 기본 모델을 `function on_<hook>(ctx)`로 고정하면, 초보자도 어느 파일을 봐야 하는지 헷갈리지 않고, host binding 확장도 일관되게 설명할 수 있다.

관련 문서:

- `server/README.md`
- `docs/configuration.md`
- `docs/extensibility/governance.md`
- `docs/ops/plugin-script-operations.md`

## 1) 준비

필수 환경 변수:

- `LUA_ENABLED=1`
- `LUA_SCRIPTS_DIR` (예: `/app/scripts`)
- `LUA_RELOAD_INTERVAL_MS` (기본 `1000`)
- `LUA_AUTO_DISABLE_THRESHOLD` (기본 `3`)

빌드/능력(capability) 기준:

- 공식 배포/개발 빌드와 Docker runtime image는 Lua capability를 항상 포함한다.
- `LUA_ENABLED`는 런타임 토글이다. 즉, 기능이 들어 있는 바이너리에서 실제 스크립트 경로를 켜고 끄는 역할을 한다.

이 구분은 꼭 이해해야 한다. "빌드에 Lua가 들어 있다"와 "현재 서버에서 Lua가 켜져 있다"는 다른 말이다. 이 둘을 섞으면, 장애 시 실제 원인이 capability 누락인지 단순 런타임 비활성화인지 잘못 판단하게 된다.

## 2) 권장 스캐폴드 생성

권장 방법은 도구로 기본 파일을 만든 뒤 function-style 본문을 채우는 것이다.

```powershell
python tools/new_script.py --name on_join_policy --hook on_join --decision deny
```

생성 템플릿도 기본적으로 `function on_<hook>(ctx)` 형태를 사용한다.

도구를 먼저 쓰는 이유는, 초보자가 처음부터 파일명, hook 이름, manifest 형태를 수동으로 맞추다 보면 아주 사소한 오타 때문에 reload가 안 되거나 inventory에 잡히지 않는 경우가 많기 때문이다. scaffold는 그런 기초 오류를 줄여 준다.

## 3) 가장 작은 권장 스크립트

```lua
local EVENT_NOTICE = "[event] spring chat event is live"

function on_login(ctx)
  if not ctx or not ctx.session_id then
    return { decision = "pass" }
  end

  local name = server.get_user_name(ctx.session_id)
  local online_count = server.get_online_count()

  if name and name ~= "" then
    server.send_notice(ctx.session_id, "welcome back, " .. name)
  else
    server.send_notice(ctx.session_id, "welcome to the server")
  end

  server.send_notice(
    ctx.session_id,
    EVENT_NOTICE .. " | online=" .. tostring(online_count)
  )

  return { decision = "pass" }
end
```

이 예제는 아주 단순하지만 중요한 구조를 모두 보여 준다.

- `ctx` 존재 여부를 먼저 확인한다.
  - hook마다 항상 같은 필드가 온다고 가정하면 nil 접근으로 바로 오류가 난다.
- host API 호출은 읽기와 action을 분리해 본다.
  - 어떤 값이 읽기 전용인지, 어떤 호출이 실제 side effect를 만드는지 구분하는 습관이 중요하다.
- 마지막에는 명시적으로 `decision`을 반환한다.
  - 암묵 동작에 기대면 hook 의미를 나중에 읽기 어렵다.

자주 쓰는 `ctx` 필드:

- `ctx.session_id`
  - 로그인/세션/관리자 경로의 대상 세션
- `ctx.user`
  - 사용자 이름 또는 닉네임
- `ctx.room`
  - 현재 대상 방
- `ctx.command`, `ctx.args`
  - 관리자 명령 컨텍스트

주요 host API 범주:

- read-only
  - `server.get_user_name`, `server.get_room_users`, `server.get_online_count`
- action
  - `server.send_notice`, `server.broadcast_room`, `server.broadcast_all`
- log/meta
  - `server.log_info`, `server.log_warn`, `server.log_debug`, `server.hook_name`, `server.script_name`

지원 decision:

- `pass`, `allow`, `modify`, `handled`, `block`, `deny`

decision을 명시적으로 나누는 이유는, 단순 성공/실패만으로는 hook 체인의 의미를 표현하기 어렵기 때문이다. 예를 들어 `modify`와 `deny`는 모두 "원래 흐름을 바꾼다"는 점에서는 같지만, 운영과 디버깅에서는 완전히 다른 사건이다.

## 4) fallback / 테스트 보조 형식

function-style hook이 기본이지만, 아래 형식은 bring-up, limit 검증, auto-disable 검증용으로 유지한다.

return-table fallback:

```lua
return {
  hook = "on_login",
  decision = "pass",
  notice = "welcome"
}
```

directive fallback:

```lua
-- hook=on_login decision=deny reason=login denied by lua scaffold
```

limit 시뮬레이션:

```lua
-- hook=on_admin_command limit=instruction
-- hook=on_admin_command limit=memory
```

각 directive는 `LUA_ERRRUN`, `LUA_ERRMEM` 경로를 강제로 통과시키는 테스트 보조 수단이다.

이 형식을 남겨 두는 이유는, 실제 운영 스크립트 작성 모델로 추천해서가 아니라, 런타임 자체가 살아 있는지 빠르게 확인해야 하는 순간이 있기 때문이다. 예를 들어 host API를 아직 붙이기 전이라도 reload, error path, auto-disable만 따로 검증하고 싶을 수 있다.

## 5) 배포와 검증

Docker 기준:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build
docker logs dynaxis-stack-server-1-1 --since 5m
```

확인 로그:

- `Lua script watcher detected changes`
- `Lua script reload complete`

확인 메트릭:

- `lua_script_calls_total`
- `lua_script_errors_total`
- `lua_instruction_limit_hits_total`
- `lua_memory_limit_hits_total`
- `hook_auto_disable_total{source="lua"}`

capability 포함 기본 빌드 확인 예시:

```powershell
pwsh scripts/build.ps1 -Config Release
ctest --preset windows-test -R "LuaRuntimeTest|LuaSandboxTest|ChatLuaBindingsTest" --output-on-failure --no-tests=error
```

Linux/Ninja 기준:

```bash
cmake --preset linux
cmake --build --preset linux-debug --target core_plugin_runtime_tests --parallel
ctest --test-dir build-linux -R 'LuaRuntimeTest|LuaSandboxTest' --output-on-failure --no-tests=error
```

검증을 로그와 메트릭, 테스트로 나눠 보는 이유는 각각 답해 주는 질문이 다르기 때문이다.

- 테스트
  - 기능이 설계대로 동작하는가
- 로그
  - watcher와 reload가 실제로 일어났는가
- 메트릭
  - 호출/오류/자원 제한 hit가 운영 중 어떻게 누적되는가

셋 중 하나만 보면 문제를 절반만 보게 된다.

### 5.1 제어면 배포 smoke

아래 절차로 "생성 -> 검증 -> 배포"를 한 번에 확인할 수 있다.

```powershell
# 1) 스캐폴드 생성 (docker/stack/scripts 경로는 admin-app inventory 기본 스캔 경로)
python tools/new_script.py --name onboarding_smoke --hook on_login --decision pass --stage side_effect --priority 42 --output-dir docker/stack/scripts

# 2) manifest 스키마 검증
python tools/ext_inventory.py --manifest docker/stack/scripts/onboarding_smoke.script.json --check --json

# 3) 제어면 즉시 배포
$cmdId = "onboarding-smoke-$([int][double]::Parse((Get-Date -UFormat %s)))"
$body = @{
  command_id = $cmdId
  artifact_id = "script:onboarding_smoke"
  selector = @{ all = $true }
  rollout_strategy = @{ type = "all_at_once" }
} | ConvertTo-Json -Depth 8

Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:39200/api/v1/ext/deployments" -ContentType "application/json" -Body $body
Invoke-RestMethod -Method Get -Uri "http://127.0.0.1:39200/api/v1/ext/deployments?limit=20"

# 4) (선택) 스모크 후 정리
Remove-Item docker/stack/scripts/onboarding_smoke.lua, docker/stack/scripts/onboarding_smoke.script.json
```

이 절차를 따로 적는 이유는, 파일만 두는 것과 "control plane이 그 artifact를 인식하고 배포까지 시켰는지"는 다른 문제이기 때문이다. inventory, precheck, deployment가 모두 이어져야 실제 운영 경로를 검증했다고 볼 수 있다.

## 6) auto-disable / 재활성화

1. 연속 실패를 `LUA_AUTO_DISABLE_THRESHOLD` 이상 유발한다.
2. `chat_lua_hook_disabled{...}=1` 및 `hook_auto_disable_total` 증가를 확인한다.
3. 정상 스크립트로 교체한 뒤 reload를 기다린다.
4. disabled 해제와 호출 재개를 확인한다.

이 절차를 일부러 문서에 포함하는 이유는, auto-disable은 장애가 났을 때만 생각하면 이미 늦기 때문이다. 운영자는 "언제 꺼지는가"뿐 아니라 "어떻게 다시 살아나는가"까지 미리 알아야 안전하게 쓸 수 있다.

## 7) 롤백

1. 이전 정상 `.lua`로 복원한다.
2. watcher/reload 로그를 확인한다.
3. 필요하면 `LUA_ENABLED=0`으로 긴급 비활성화한다.

롤백 절차를 문서 끝에 두는 이유는 단순하다. Lua는 빠르게 바꾸기 쉬운 대신, 잘못 배포해도 빠르게 되돌릴 수 있어야 운영에서 쓸 만하다. 작성법만 있고 롤백이 없으면 quickstart가 아니라 사고 유도 문서가 된다.
