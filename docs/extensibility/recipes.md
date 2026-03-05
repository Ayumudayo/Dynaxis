# Extensibility Recipes

자주 사용하는 플러그인/Lua 운영 패턴을 레시피 형태로 정리한다.

## Recipe 1: 로그인 환영 메시지 (Lua)

목표: 로그인 시 공지 1줄을 빠르게 추가.

1. `LUA_ENABLED=1` 확인
2. `LUA_SCRIPTS_DIR`에 아래 파일 추가

```lua
return {
  hook = "on_login",
  decision = "pass",
  notice = "welcome to Knights"
}
```

3. watcher/reload 로그 확인
4. 로그인 스모크로 notice 수신 확인

## Recipe 2: on_join 차단 정책 (Lua 스캐폴드)

목표: on_join 경로에서 deny 동작/클라이언트 reason 전달을 빠르게 검증.

```lua
return {
  hook = "on_join",
  decision = "deny",
  reason = "vip room requires approval"
}
```

검증: `MSG_ERR(FORBIDDEN)` + reason 전달 확인.

참고: 현재 스캐폴드 모드에서는 room/user 입력 기반 분기 대신 고정 정책 검증이 중심이다.

## Recipe 3: 고빈도 채팅 필터 (Native Plugin)

목표: hot path에서 금칙어/스팸 필터를 낮은 지연으로 처리.

1. `ChatHookApiV2` 플러그인 작성 (`on_chat_send` 중심)
2. 파일명으로 순서 지정(`10_`, `20_`)
3. 배포 후 메트릭 확인
   - `plugin_hook_calls_total`
   - `plugin_hook_errors_total`
   - `plugin_hook_duration_seconds`

## Recipe 4: auto-disable 장애 완화

목표: 스크립트 오류 급증 시 안전하게 완화.

1. `hook_auto_disable_total{source="lua"}` 증가 확인
2. 문제 스크립트 파일 즉시 롤백
3. reload 완료 로그 확인
4. 필요 시 `LUA_ENABLED=0`로 전체 우회

## Recipe 5: 플러그인 swap + 롤백

1. 새 바이너리를 staging에 준비
2. 동일 파일명으로 교체
3. reload success 메트릭 확인
4. 이상 시 이전 바이너리로 즉시 복구

## Recipe 6: 관측성 점검 루틴

배포 직후 5분 점검:

1. 에러율: `plugin_hook_errors_total`, `lua_script_errors_total`
2. 제한 히트: `lua_instruction_limit_hits_total`, `lua_memory_limit_hits_total`
3. 자동 비활성화: `hook_auto_disable_total`
4. 기본 지표 회귀: `chat_frame_total`, `chat_dispatch_total`

대시보드: `chat-server-runtime`의 `Extensibility` row를 사용한다.

## Recipe 7: 배포 전 manifest precheck

목표: 배포 전에 manifest 필드 누락/형식 오류를 빠르게 차단.

```powershell
python tools/ext_inventory.py --plugins-dir server/plugins --scripts-dir server/scripts --check
```

템플릿 스키마 검증 전용:

```powershell
python tools/ext_inventory.py --manifest server/plugins/templates/plugin_manifest.template.json --manifest server/scripts/templates/script_manifest.template.json --allow-missing-artifact --check --json
```
