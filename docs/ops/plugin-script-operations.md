# 플러그인/스크립트 운영 절차

이 문서는 Docker stack 기준으로 chat hook 플러그인(네이티브)과 Lua 스크립트(cold-hook) 운영 절차를 정리한다. 목표는 새 artifact를 올리는 방법만 적는 것이 아니다. 교체, reload 확인, rollback, 긴급 우회까지 같은 문서에서 읽히게 해 운영자가 장애 중에도 즉흥적으로 판단하지 않도록 만드는 것이 더 중요하다.

관련 파일:

- `docker/stack/docker-compose.yml`
- `docker/stack/plugins/`
- `docker/stack/scripts/`
- `server/README.md`

## 1. 기본 경로와 전제

- 플러그인 디렉터리: `/app/plugins` (`docker/stack/plugins` 읽기 전용 mount)
- 플러그인 fallback 디렉터리: `/app/plugins_builtin` (`CHAT_HOOK_FALLBACK_PLUGINS_DIR`)
- 스크립트 디렉터리: `/app/scripts` (`docker/stack/scripts` 읽기 전용 mount)
- 스크립트 fallback 디렉터리: `/app/scripts_builtin` (`LUA_FALLBACK_SCRIPTS_DIR`)
- 내장 fallback 이미지는 `server/scripts/`를 복사해 만든다.
- 겹치는 샘플 이름은 `server/scripts/`와 `docker/stack/scripts/`에서 같은 내용으로 유지해 mount와 fallback 드리프트를 줄인다.
- 기본 런타임 토글: `CHAT_HOOK_ENABLED=0`, `LUA_ENABLED=0`
- 서버 컨테이너 예시: `dynaxis-stack-server-1-1`, `dynaxis-stack-server-2-1`

이 경로를 먼저 고정하는 이유는 운영 실수의 상당수가 "어느 디렉터리가 실제로 우선되는가"를 잘못 이해해서 생기기 때문이다. 특히 mount 경로와 builtin fallback 경로가 함께 있을 때는, 어떤 파일이 실제로 읽히는지 먼저 알아야 reload 결과도 해석할 수 있다.

기동/재기동:

```powershell
pwsh scripts/deploy_docker.ps1 -Action up -Detached -Build
```

## 2. 네이티브 플러그인 교체 절차

권장: 교체 대상 `.so`를 같은 파일명으로 준비해 원자적으로 rename 또는 copy 한다.

1) 교체 대상 준비
- 호스트 `docker/stack/plugins/`에 새 바이너리를 준비한다.
- 파일명 순서(`10_`, `20_`)가 실행 순서를 결정한다.

2) 컨테이너 반영 확인

```powershell
docker exec dynaxis-stack-server-1-1 sh -lc "ls -la /app/plugins"
docker exec dynaxis-stack-server-2-1 sh -lc "ls -la /app/plugins"
```

3) reload 확인
- 서버 로그에서 reload 성공과 실패를 확인한다.
- 관련 메트릭: `chat_hook_plugin_reload_attempt_total`, `chat_hook_plugin_reload_success_total`, `chat_hook_plugin_reload_failure_total`

```powershell
docker logs dynaxis-stack-server-1-1 --since 5m
docker logs dynaxis-stack-server-2-1 --since 5m
```

같은 파일명 교체를 권장하는 이유는 체인 순서와 inventory 해석을 함께 보존하기 위해서다. 파일명까지 바꾸면 코드가 달라진 것인지 순서가 달라진 것인지 원인을 곧바로 구분하기 어렵다.

참고:

- 단일 플러그인 모드(`CHAT_HOOK_PLUGIN_PATH`)에서는 `CHAT_HOOK_LOCK_PATH` sentinel로 reload를 지연할 수 있다.
- 디렉터리 체인 모드(`CHAT_HOOK_PLUGINS_DIR`)에서는 파일 교체를 짧은 구간에 끝내고 손상 파일 배포를 피하는 것이 핵심이다.

## 3. Lua 스크립트 교체 절차

1) 호스트 스크립트 수정
- 공용 샘플이면 `server/scripts/*.lua`와 `docker/stack/scripts/*.lua`를 함께 갱신한다.
- 기본 작성 모델은 함수 스타일 hook + `ctx`이며, directive/return-table은 fallback 테스트에만 사용한다.

2) watcher 감지와 재로드 확인
- 기대 로그:
  - `Lua script watcher detected changes`
  - `Lua script reload complete`

```powershell
docker logs dynaxis-stack-server-1-1 --since 5m
docker logs dynaxis-stack-server-2-1 --since 5m
```

3) 동작 확인
- 로그인, 입장, 관리자 명령 경로에서 의도한 notice 또는 deny 동작을 스모크로 확인한다.

Lua 경로에서 가장 흔한 실패는 "파일은 바뀌었는데 실제 런타임은 새 스크립트를 쓰지 않는 상태"다. 그래서 파일 교체만 끝내지 말고 watcher와 reload 로그까지 꼭 확인해야 한다.

## 4. 롤백 절차

### 4.1 플러그인 롤백

- 이전 정상 `.so`를 동일 파일명으로 되돌린다.
- reload 성공 메트릭 증가를 확인한다.

### 4.2 Lua 롤백

- 이전 정상 `.lua` 내용으로 되돌리거나 문제 스크립트를 제거한다.
- watcher와 reload 로그를 확인한다.

### 4.3 긴급 우회

- Lua 전체 비활성화:
  - compose env에서 `LUA_ENABLED=0` 적용 뒤 서버 재기동
- 플러그인 전체 우회:
  - compose env에서 `CHAT_HOOK_ENABLED=0` 적용 뒤 서버 재기동
  - (기존 우회 경로) 필요 시 `CHAT_HOOK_PLUGINS_DIR`, `CHAT_HOOK_FALLBACK_PLUGINS_DIR`를 비활성 경로로 재지정 가능

긴급 우회를 문서에 남기는 이유는 rollback만으로는 충분하지 않은 상황이 있기 때문이다. 손상 artifact가 반복 배포되거나 root cause가 아직 불분명하면, 전체 경로를 잠시 끄는 편이 서비스 안정성에는 더 낫다.

## 5. 장애 대응 체크리스트

1) 서비스 생존 확인
- `pwsh scripts/deploy_docker.ps1 -Action ps`

2) 최근 에러 확인
- `docker logs dynaxis-stack-server-1-1 --since 10m`
- `docker logs dynaxis-stack-server-2-1 --since 10m`

3) 메트릭 확인
- plugin reload failure 급증 여부
- chat dispatch error 급증 여부

4) 즉시 완화
- 문제 플러그인 또는 스크립트 롤백
- 필요 시 Lua 비활성화 뒤 재기동

5) 사후 조치
- 실패 원인(손상 바이너리, 문법 오류, 정책 충돌) 기록
- 재배포 전 스테이징 검증 추가

체크리스트를 이런 순서로 두는 이유는 장애 대응에서 가장 먼저 해야 할 일이 "왜 실패했는가"보다 "지금도 계속 망가지고 있는가"를 확인하는 것이기 때문이다. 생존, 로그, 메트릭, 완화, 사후 조치를 분리해 두면 현장에서 판단 순서를 잃지 않게 된다.

추가 참고:

- 서버는 poll 주기마다 `LUA_SCRIPTS_DIR`와 `LUA_FALLBACK_SCRIPTS_DIR`를 다시 평가한다.
- `docker/stack/scripts`가 비면 `/app/scripts_builtin`으로 자동 전환되고, 다시 `.lua`가 생기면 `/app/scripts`로 자동 복귀한다.

이 동작을 문서로 남기는 이유는 fallback이 "기동 시 한 번만 정해지는 경로"가 아니기 때문이다. 운영자는 파일이 비었을 때 자동 전환되는지, 다시 채워졌을 때 원래 경로로 복귀하는지를 알고 있어야 reload 결과를 올바르게 해석할 수 있다.
