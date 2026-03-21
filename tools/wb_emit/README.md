# 이벤트 발행기(wb_emit)

`wb_emit`은 Redis Streams 기반 write-behind 파이프라인을 스모크 테스트하기 위한 간단한 이벤트 발행기다. `server_app`을 띄우지 않고도 임의의 이벤트를 스트림에 추가할 수 있어 `wb_worker`, `wb_dlq_replayer` 동작을 검증하거나 대시보드/알람을 점검할 때 유용하다.

이 도구가 필요한 이유는, 파이프라인 전체가 문제인지 아니면 `server_app` 생산 경로만 문제인지를 분리해서 보고 싶을 때가 많기 때문이다. `wb_emit`을 쓰면 "서버 본체 없이도 스트림에 이벤트를 넣을 수 있는가"를 먼저 확인할 수 있다.

즉, 이 도구는 다음 질문에 답할 때 유용하다.

- Redis Stream 쓰기 자체가 되는가
- `wb_worker`가 임의 이벤트를 읽고 DB까지 반영하는가
- DLQ/알람/대시보드가 최소 입력에서도 반응하는가

반대로 이 도구만 통과했다고 해서 `server_app`의 실제 이벤트 매핑이 맞다는 뜻은 아니다. `wb_emit`은 생산 경로를 단순화해 점검하는 도구이지, 전체 제품 동작을 대체하는 것은 아니다.

```text
tools/wb_emit/
├─ main.cpp
└─ README.md
```

## 사용 방법

가장 흔한 사용 순서는 다음과 같다.

1. `wb_worker`를 띄운다
2. `wb_emit`으로 테스트 이벤트를 발행한다
3. 필요하면 `wb_check`로 DB 적재 여부를 확인한다
4. 실패 시 `wb_dlq_replayer` 또는 DLQ 메트릭을 본다

```powershell
scripts/build.ps1 -Config Debug -Target wb_emit
.\build-windows\tools\Debug\wb_emit.exe            # 기본 이벤트(session_login) 발행
.\build-windows\tools\Debug\wb_emit.exe room_join  # type 필드를 덮어써 커스텀 이벤트 발행
```

간단한 smoke 용도에서는 충분하지만, 실제 운영 payload를 정밀하게 흉내 내야 한다면 `main.cpp`를 수정하거나 실제 `server_app` 경로를 함께 검증하는 편이 더 안전하다.

## 환경 변수
| 이름 | 설명 | 기본값 |
| --- | --- | --- |
| `REDIS_URI` | Redis 연결 문자열 | (필수) |
| `REDIS_STREAM_KEY` | 이벤트를 쓸 스트림 이름 | `session_events` |

`.env`는 개발 편의용 예시 파일이며, 애플리케이션이 자동으로 로드하지 않는다.
로컬에서는 쉘/스크립트에서 `.env`를 로드한 뒤 실행하거나, OS 환경 변수로 직접 주입해야 한다.

## 필드 레이아웃

이 필드는 "최소한 worker가 받아서 흐름을 테스트할 수 있는 형태"에 가깝다. 즉 운영 이벤트의 완전한 재현이라기보다 파이프라인 연결성을 점검하기 위한 샘플 payload다.

- `type`: 명령줄 인자(또는 `session_login`)
- `ts_ms`: 현재 시각(UTC) 밀리초
- `session_id`, `user_id`, `room_id`: 샘플 UUID(테스트 전용)
- `payload`: 간단한 JSON 문자열 (`{"note":"smoke test"}`)

필요하면 `main.cpp`를 수정해 추가 필드를 넣은 뒤, 동일한 빌드·실행 방법으로 재사용하면 된다.
