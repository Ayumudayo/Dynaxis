# 이벤트 발행기(wb_emit)

`wb_emit`은 Redis Streams 기반 write-behind 파이프라인을 스모크 테스트하기 위한 간단한 이벤트 발행기다. `server_app`을 띄우지 않고도 임의의 이벤트를 스트림에 추가할 수 있어 `wb_worker`, `wb_dlq_replayer` 동작을 검증하거나 대시보드/알람을 점검할 때 유용하다.

```text
tools/wb_emit/
├─ main.cpp
└─ README.md
```

## 사용 방법
```powershell
scripts/build.ps1 -Config Debug -Target wb_emit
.\build-windows\tools\Debug\wb_emit.exe            # 기본 이벤트(session_login) 발행
.\build-windows\tools\Debug\wb_emit.exe room_join  # type 필드를 덮어써 커스텀 이벤트 발행
```

## 환경 변수
| 이름 | 설명 | 기본값 |
| --- | --- | --- |
| `REDIS_URI` | Redis 연결 문자열 | (필수) |
| `REDIS_STREAM_KEY` | 이벤트를 쓸 스트림 이름 | `session_events` |

`.env`는 개발 편의용 예시 파일이며, 애플리케이션이 자동으로 로드하지 않는다.
로컬에서는 쉘/스크립트에서 `.env`를 로드한 뒤 실행하거나, OS 환경 변수로 직접 주입해야 한다.

## 필드 레이아웃
- `type`: 명령줄 인자(또는 `session_login`)
- `ts_ms`: 현재 시각(UTC) 밀리초
- `session_id`, `user_id`, `room_id`: 샘플 UUID(테스트 전용)
- `payload`: 간단한 JSON 문자열 (`{"note":"smoke test"}`)

필요하면 `main.cpp`를 수정해 추가 필드를 넣은 뒤, 동일한 빌드·실행 방법으로 재사용하면 된다.
