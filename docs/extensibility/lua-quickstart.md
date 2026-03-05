# Lua Quickstart (Cold Hook)

이 문서는 `server_app`의 Lua cold hook 스크립트를 빠르게 작성/검증/롤백하는 절차를 정리한다.

참고:
- 현재 저장소의 Lua 경로는 스캐폴드 모드(주석 directive/return table 파싱) 검증을 포함한다.
- 로컬 기본 빌드는 `BUILD_LUA_SCRIPTING=OFF`일 수 있으므로, Lua 테스트/검증 시 Lua 빌드 경로를 사용한다.

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

## 2) 가장 작은 스크립트

권장: 스캐폴드 도구로 기본 파일을 생성한 뒤 내용을 조정한다.

```powershell
python tools/new_script.py --name on_join_policy --hook on_join --decision deny
```

```lua
-- scripts/on_login_welcome.lua
return {
  hook = "on_login",
  decision = "pass",
  notice = "welcome from lua"
}
```

지원 decision:
- `pass`, `allow`, `modify`, `handled`, `block`, `deny`

지원 필드:
- `hook`, `decision`, `reason`, `notice`

## 3) 제한 오류 시뮬레이션 (스캐폴드)

현재 스캐폴드 모드에서는 아래 directive로 제한 오류 경로를 재현할 수 있다.

```lua
-- hook=on_admin_command limit=instruction
```

```lua
-- hook=on_admin_command limit=memory
```

각각 `LUA_ERRRUN`, `LUA_ERRMEM` 경로를 트리거하며, 서버는 호출 실패를 격리하고 계속 운행해야 한다.

## 4) 배포/검증

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

### 4.1 제어면 배포 smoke (신규 artifact)

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

## 5) auto-disable / 재활성화

1. 연속 실패를 `LUA_AUTO_DISABLE_THRESHOLD` 이상 유발
2. `chat_lua_hook_disabled{...}=1` 및 `hook_auto_disable_total` 증가 확인
3. 정상 스크립트로 교체 후 reload
4. disabled 해제 및 호출 재개 확인

## 6) 롤백

1. 이전 정상 `.lua`로 복원
2. watcher/reload 로그 확인
3. 필요 시 `LUA_ENABLED=0`으로 긴급 비활성화
