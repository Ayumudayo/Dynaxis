# Plugin/Script Operations Runbook

이 문서는 Docker stack 기준으로 chat hook 플러그인(native)과 Lua 스크립트(cold-hook) 운영 절차를 정리한다.

관련 파일:
- `docker/stack/docker-compose.yml`
- `docker/stack/plugins/`
- `docker/stack/scripts/`
- `server/README.md`

## 1. 기본 경로와 전제

- 플러그인 디렉터리: `/app/plugins` (`docker/stack/plugins` read-only mount)
- 플러그인 fallback 디렉터리: `/app/plugins_builtin` (`CHAT_HOOK_FALLBACK_PLUGINS_DIR`)
- 스크립트 디렉터리: `/app/scripts` (`docker/stack/scripts` read-only mount)
- 스크립트 fallback 디렉터리: `/app/scripts_builtin` (`LUA_FALLBACK_SCRIPTS_DIR`)
- built-in fallback 이미지는 `server/scripts/`를 복사해 만든다.
- 겹치는 샘플 이름은 `server/scripts/`와 `docker/stack/scripts/`에서 같은 내용으로 유지해 mount/fallback 드리프트를 줄인다.
- 기본 런타임 토글: `CHAT_HOOK_ENABLED=0`, `LUA_ENABLED=0`
- 서버 컨테이너 예시: `knights-stack-server-1-1`, `knights-stack-server-2-1`

기동/재기동:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build
```

## 2. 네이티브 플러그인 교체 절차 (swap)

권장: 교체 대상 `.so`를 같은 파일명으로 준비해 원자적으로 rename/copy한다.

1) 교체 대상 준비
- 호스트 `docker/stack/plugins/`에 새 바이너리 준비
- 파일명 순서(`10_`, `20_`)가 실행 순서를 결정한다.

2) 컨테이너 반영 확인

```powershell
docker exec knights-stack-server-1-1 sh -lc "ls -la /app/plugins"
docker exec knights-stack-server-2-1 sh -lc "ls -la /app/plugins"
```

3) reload 확인
- 서버 로그에서 reload 성공/실패를 확인한다.
- 관련 메트릭: `chat_hook_plugin_reload_attempt_total`, `chat_hook_plugin_reload_success_total`, `chat_hook_plugin_reload_failure_total`

```powershell
docker logs knights-stack-server-1-1 --since 5m
docker logs knights-stack-server-2-1 --since 5m
```

참고:
- 단일 플러그인 모드(`CHAT_HOOK_PLUGIN_PATH`)에서는 `CHAT_HOOK_LOCK_PATH` sentinel로 reload를 지연할 수 있다.
- 디렉터리 체인 모드(`CHAT_HOOK_PLUGINS_DIR`)는 파일 교체를 짧은 구간에 끝내고, 손상 파일 배포를 피하는 것이 핵심이다.

## 3. Lua 스크립트 교체 절차 (hot-reload)

1) 호스트 스크립트 수정
- 공용 샘플이면 `server/scripts/*.lua`와 `docker/stack/scripts/*.lua`를 함께 갱신한다.
- 우선 작성 모델은 function-style hook + `ctx`이며, directive/return-table은 fallback 테스트에만 사용한다.

2) watcher 감지/재로드 확인
- 기대 로그:
  - `Lua script watcher detected changes`
  - `Lua script reload complete`

```powershell
docker logs knights-stack-server-1-1 --since 5m
docker logs knights-stack-server-2-1 --since 5m
```

3) 동작 확인
- 로그인/입장/관리자 명령 경로에서 의도한 notice/deny 동작을 스모크로 확인한다.

## 4. 롤백 절차

### 4.1 플러그인 롤백

- 이전 정상 `.so`를 동일 파일명으로 되돌린다.
- reload 성공 메트릭 증가를 확인한다.

### 4.2 Lua 롤백

- 이전 정상 `.lua` 내용으로 되돌리거나, 문제 스크립트를 제거한다.
- watcher/reload 로그를 확인한다.

### 4.3 긴급 우회

- Lua 전체 비활성화:
  - compose env에서 `LUA_ENABLED=0` 적용 후 서버 재기동
- 플러그인 전체 우회:
  - compose env에서 `CHAT_HOOK_ENABLED=0` 적용 후 서버 재기동
  - (레거시 우회) 필요 시 `CHAT_HOOK_PLUGINS_DIR`/`CHAT_HOOK_FALLBACK_PLUGINS_DIR`를 비활성 경로로 재지정 가능

## 5. 장애 대응 체크리스트

1) 서비스 생존 확인
- `pwsh scripts/deploy_docker.ps1 -Action ps`

2) 최근 에러 확인
- `docker logs knights-stack-server-1-1 --since 10m`
- `docker logs knights-stack-server-2-1 --since 10m`

3) 메트릭 확인
- plugin reload failure 급증 여부
- chat dispatch error 급증 여부

4) 즉시 완화
- 문제 플러그인/스크립트 롤백
- 필요 시 Lua 비활성화 후 재기동

5) 사후 조치
- 실패 원인(손상 바이너리, 문법 오류, 정책 충돌) 기록
- 재배포 전 스테이징 검증 추가

추가 참고:
- 서버는 poll 주기마다 `LUA_SCRIPTS_DIR`와 `LUA_FALLBACK_SCRIPTS_DIR`를 다시 평가한다. `docker/stack/scripts`가 비면 `/app/scripts_builtin`으로 자동 전환되고, 다시 `.lua`가 생기면 `/app/scripts`로 자동 복귀한다.
