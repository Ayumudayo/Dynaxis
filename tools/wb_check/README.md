# 적재 확인 도구(wb_check)

`wb_check`는 특정 `event_id`가 PostgreSQL `session_events` 테이블에 적재되었는지 빠르게 확인하는 CLI 도구다. write-behind 파이프라인을 테스트하거나 DLQ 재처리 이후 검증할 때 사용한다.

이 도구가 필요한 이유는, 운영에서 "이 이벤트가 정말 DB까지 갔는가"를 가장 빠르게 확인하는 질문이 자주 나오기 때문이다. 로그만 보면 발행 여부와 소비 여부가 섞여 보일 수 있는데, `wb_check`는 최종 적재 결과만 바로 확인하게 해 준다.

```text
tools/wb_check/
├─ main.cpp
└─ README.md
```

## 사용법

가장 기본적인 사용 흐름은 다음과 같다.

1. `wb_emit` 또는 실제 `server_app`이 이벤트를 발행한다
2. `event_id`를 확보한다
3. `wb_check`로 DB 존재 여부를 확인한다

```powershell
scripts/build.ps1 -Config Debug -Target wb_check
.\build-windows\tools\Debug\wb_check.exe <event_id>
```
- 존재하면 `found`를 출력하고 종료 코드 0을 반환한다.
- 존재하지 않으면 `not found`와 함께 종료 코드 5를 반환한다.
- DB 접속 오류 등 예외 발생 시 종료 코드 1~4를 반환하며, 구체적인 오류 메시지를 stderr에 남긴다.

## 환경 변수
| 이름 | 설명 | 기본값 |
| --- | --- | --- |
| `DB_URI` | PostgreSQL 연결 문자열 | (필수) |

`.env`는 개발 편의용 예시 파일이며, 애플리케이션이 자동으로 로드하지 않는다.
로컬에서는 쉘/스크립트에서 `.env`를 로드한 뒤 실행하거나, OS 환경 변수로 직접 주입해야 한다.

## 활용 시나리오
- `wb_emit`로 발행한 테스트 이벤트가 DB까지 적재됐는지 즉시 확인.
- `wb_worker`, `wb_dlq_replayer`가 처리한 실제 이벤트를 샘플링해 존재 여부를 점검.
- 운영 중 특정 이벤트 ID가 중복 처리되었는지 여부를 빠르게 판단.

즉, `wb_check`는 성능 측정 도구가 아니라 "최종 상태 확인 도구"다. 적재 지연이 문제인지, 적재 실패가 문제인지를 구분할 때 특히 유용하다.
