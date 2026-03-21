# 확장성 레시피

이 문서는 자주 사용하는 플러그인과 Lua 운영 패턴을 짧은 레시피 형태로 정리한다. 빠른 시작 문서가 "어떻게 첫 번째 artifact를 만드는가"에 집중한다면, 이 문서는 실제 운영에서 반복되는 작은 작업을 안전하게 수행하는 방법에 더 가깝다.

레시피를 따로 두는 이유는 모든 변경이 새 설계 문서를 필요로 하지는 않기 때문이다. 하지만 반복 작업이라고 해서 즉흥적으로 처리하면, 같은 문제를 만날 때마다 다른 절차를 밟게 되고 장애 대응도 매번 새로 생각해야 한다.

## 레시피 1: 로그인 환영 메시지 추가 (Lua)

목표: 로그인 시 공지 한 줄을 빠르게 추가한다.

1. `LUA_ENABLED=1`인지 확인한다.
2. shared sample이라면 `server/scripts/on_login_welcome.lua`와 `docker/stack/scripts/on_login_welcome.lua`를 같은 의미로 유지한다.
3. `LUA_SCRIPTS_DIR`에 아래 파일을 추가한다.

```lua
function on_login(ctx)
  if not ctx or not ctx.session_id then
    return { decision = "pass" }
  end

  server.send_notice(ctx.session_id, "welcome to the server")
  return { decision = "pass" }
end
```

4. watcher와 reload 로그를 확인한다.
5. 로그인 스모크로 notice 수신을 확인한다.

이 레시피는 가장 작은 성공 사례를 만드는 데 적합하다. 먼저 단순 공지 경로로 reload와 notice 동작을 확인해 두면, 이후 정책성 스크립트를 올릴 때 실패 범위를 더 잘 좁힐 수 있다.

## 레시피 2: `on_join` 차단 정책 추가 (Lua + `ctx`)

목표: `on_join` 경로에서 deny 동작과 클라이언트 reason 전달을 빠르게 검증한다.

```lua
local RESTRICTED_ROOMS = {
  vip_lounge = true,
}

function on_join(ctx)
  if not ctx or not RESTRICTED_ROOMS[ctx.room] then
    return { decision = "pass" }
  end

  return {
    decision = "deny",
    reason = "vip room requires approval",
  }
end
```

검증: `MSG_ERR(FORBIDDEN)`과 reason 전달을 함께 확인한다.

이 레시피가 중요한 이유는 deny 동작이 단순 boolean이 아니기 때문이다. 클라이언트에 어떤 이유가 전달되는지까지 확인해야 실제 운영에서 "정책이 막은 것인지", "서버 오류로 실패한 것인지"를 구분할 수 있다.

## 레시피 3: fallback 결정 경로 점검

목표: host API rollout 전후에 reload와 decision 배관만 빠르게 점검한다.

```lua
return {
  hook = "on_join",
  decision = "deny",
  reason = "vip room requires approval"
}
```

또는:

```lua
-- hook=on_join decision=deny reason=vip room requires approval
```

참고: 이 형식은 함수 스타일 hook의 대체재가 아니라 fallback/testing aid다.

이 레시피는 실제 정책 작성용이 아니라, "hook wiring은 붙었는가"만 빠르게 확인할 때 쓴다. 운영용 스크립트를 계속 이 형식으로 유지하면 작성 모델이 다시 분산되므로 장기 유지보수에는 불리하다.

## 레시피 4: 고빈도 채팅 필터 (네이티브 플러그인)

목표: hot path에서 금칙어 또는 스팸 필터를 낮은 지연으로 처리한다.

1. `ChatHookApiV2` 플러그인을 작성한다. 중심 hook은 `on_chat_send`다.
2. 파일명 접두사(`10_`, `20_`)로 체인 순서를 지정한다.
3. 배포 뒤 아래 메트릭을 확인한다.
   - `plugin_hook_calls_total`
   - `plugin_hook_errors_total`
   - `plugin_hook_duration_seconds`

hot path 필터를 네이티브 플러그인으로 두는 이유는 성능 때문이다. 같은 정책이라도 호출 빈도가 높아지면 Lua의 편의성보다 지연 비용과 오류 반경이 더 중요해진다.

## 레시피 5: auto-disable 완화 절차

목표: 스크립트 오류가 급증할 때 안전하게 완화한다.

1. `hook_auto_disable_total{source="lua"}` 증가를 확인한다.
2. 문제 스크립트를 즉시 롤백한다.
3. reload 완료 로그를 확인한다.
4. 필요하면 `LUA_ENABLED=0`으로 전체 경로를 우회한다.

auto-disable은 최후의 안전장치다. 이 카운터가 오르는 상황을 무시하면 실패 스크립트가 계속 재시도되면서 hook 경로 전체를 불안정하게 만들 수 있다.

## 레시피 6: 플러그인 교체와 롤백

1. 새 바이너리를 staging에 준비한다.
2. 동일 파일명으로 교체한다.
3. reload success 메트릭을 확인한다.
4. 이상이 있으면 이전 바이너리로 즉시 복구한다.

동일 파일명 교체를 권장하는 이유는 체인 순서와 inventory 해석을 그대로 유지하기 위해서다. 이름까지 함께 바꾸면 "코드가 바뀌었기 때문인지 순서가 바뀌었기 때문인지"를 바로 구분하기 어렵다.

## 레시피 7: 관측 점검 루틴

배포 직후 5분 동안 아래 항목을 본다.

1. 에러율: `plugin_hook_errors_total`, `lua_script_errors_total`
2. 제한 hit: `lua_instruction_limit_hits_total`, `lua_memory_limit_hits_total`
3. 자동 비활성화: `hook_auto_disable_total`
4. 기본 지표 회귀: `chat_frame_total`, `chat_dispatch_total`

대시보드는 `chat-server-runtime`의 `Extensibility` 행을 사용한다.

이 점검 루틴이 필요한 이유는 확장성 배포의 회귀가 플러그인 메트릭에만 나타나지 않기 때문이다. hook은 붙었는데 전체 chat 처리량과 dispatch 지연이 나빠질 수도 있으므로 기본 지표까지 같이 봐야 한다.

## 레시피 8: 배포 전 manifest 검증

목표: 배포 전에 manifest 필드 누락과 형식 오류를 빠르게 차단한다.

```powershell
python tools/ext_inventory.py --plugins-dir server/plugins --scripts-dir server/scripts --check
```

템플릿 스키마만 검증하고 싶다면:

```powershell
python tools/ext_inventory.py --manifest server/plugins/templates/plugin_manifest.template.json --manifest server/scripts/templates/script_manifest.template.json --allow-missing-artifact --check --json
```

manifest 검증을 배포 전에 분리해 두는 이유는 가장 값싼 오류를 가장 앞단에서 끊기 위해서다. 파일 형식 문제를 런타임에서 발견하기 시작하면, loader 오류인지 artifact 오류인지 운영자가 다시 추적해야 한다.
