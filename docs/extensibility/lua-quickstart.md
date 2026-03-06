# Lua Quickstart (Cold Hook)

이 문서는 `server_app`의 Lua cold hook을 Phase 16 기준 작성 모델에 맞춰 빠르게 작성, 검증, 롤백하는 절차를 정리한다.

핵심 원칙:

- 기본 작성 형태는 function-style hook + `ctx`다.
- `server/scripts/`는 런타임 이미지 builtin fallback(`/app/scripts_builtin`)의 소스다.
- `docker/stack/scripts/`는 Docker stack mount(`/app/scripts`) 경로다.
- 겹치는 샘플 이름은 builtin과 stack 동작이 어긋나지 않도록 같은 내용으로 유지한다.
- directive/return-table은 호환성 fallback 및 테스트 보조 수단으로만 유지한다.

관련 문서:

- `server/README.md`
- `docs/configuration.md`
- `docs/ops/plugin-script-operations.md`

## 1) 준비

필수 환경 변수:

- `LUA_ENABLED=1`
- `LUA_SCRIPTS_DIR` (예: `/app/scripts`)
- `LUA_RELOAD_INTERVAL_MS` (기본 `1000`)
- `LUA_AUTO_DISABLE_THRESHOLD` (기본 `3`)

빌드/능력(capability) 기준:

- 공식 배포/개발 빌드와 Docker runtime image는 `BUILD_LUA_SCRIPTING=ON`을 전제로 한다.
- `LUA_ENABLED`는 런타임 토글이며, `BUILD_LUA_SCRIPTING=OFF` 커스텀 빌드에서는 `1`이어도 Lua 경로가 활성화되지 않는다.
- OFF 호환성 회귀는 `windows-lua-off` 또는 `linux-lua-off` preset으로 분리 점검한다.

## 2) 권장 스캐폴드 생성

권장: 도구로 기본 파일을 만든 뒤 function-style 본문을 채운다.

```powershell
python tools/new_script.py --name on_join_policy --hook on_join --decision deny
```

생성 템플릿도 function-style `function on_<hook>(ctx)`를 기본으로 사용한다.

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
    server.send_notice(ctx.session_id, "welcome to Knights")
  end

  server.send_notice(
    ctx.session_id,
    EVENT_NOTICE .. " | online=" .. tostring(online_count)
  )

  return { decision = "pass" }
end
```

자주 쓰는 `ctx` 필드:

- `ctx.session_id`: 로그인/세션/관리자 경로의 대상 세션
- `ctx.user`: 사용자 이름 또는 닉네임
- `ctx.room`: 현재 대상 방
- `ctx.command`, `ctx.args`: 관리자 명령 컨텍스트

주요 host API 범주:

- read-only: `server.get_user_name`, `server.get_room_users`, `server.get_online_count`
- action: `server.send_notice`, `server.broadcast_room`, `server.broadcast_all`
- log/meta: `server.log_info`, `server.log_warn`, `server.log_debug`, `server.hook_name`, `server.script_name`

지원 decision:

- `pass`, `allow`, `modify`, `handled`, `block`, `deny`

## 4) fallback/testing aid

function-style hook이 기본이지만, 아래 형식은 bring-up/limit/auto-disable 검증용으로 유지한다.

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

## 5) 배포/검증

Docker 기준:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build
docker logs knights-stack-server-1-1 --since 5m
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

build-toggle 확인 예시:

```powershell
python tools/check_lua_build_toggle.py --build-dir build-windows --expect on
python tools/check_lua_build_toggle.py --build-dir build-windows-lua-off --expect off
```

Linux/Ninja 기준:

```bash
cmake --preset linux
python3 tools/check_lua_build_toggle.py --build-dir build-linux --expect on --require-source-check
cmake --preset linux-lua-off
python3 tools/check_lua_build_toggle.py --build-dir build-linux-lua-off --expect off --require-source-check
```

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

## 6) auto-disable / 재활성화

1. 연속 실패를 `LUA_AUTO_DISABLE_THRESHOLD` 이상 유발한다.
2. `chat_lua_hook_disabled{...}=1` 및 `hook_auto_disable_total` 증가를 확인한다.
3. 정상 스크립트로 교체 후 reload를 기다린다.
4. disabled 해제와 호출 재개를 확인한다.

## 7) 롤백

1. 이전 정상 `.lua`로 복원한다.
2. watcher/reload 로그를 확인한다.
3. 필요 시 `LUA_ENABLED=0`으로 긴급 비활성화한다.
