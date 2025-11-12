# wb_check

`wb_check`는 특정 `event_id`가 PostgreSQL `session_events` 테이블에 적재되었는지 빠르게 확인하는 CLI 도구다. write-behind 파이프라인을 테스트하거나 DLQ 재처리 이후 검증할 때 사용한다.

```text
tools/wb_check/
├─ main.cpp
└─ README.md
```

## 사용법
```powershell
cmake --build build-msvc --target wb_check
.\build-msvc\tools\Debug\wb_check.exe <event_id>
```
- 존재하면 `found`를 출력하고 종료 코드 0을 반환한다.
- 존재하지 않으면 `not found`와 함께 종료 코드 5를 반환한다.
- DB 접속 오류 등 예외 발생 시 종료 코드 1~4를 반환하며, 구체적인 오류 메시지를 stderr에 남긴다.

## 환경 변수
| 이름 | 설명 | 기본값 |
| --- | --- | --- |
| `DB_URI` | PostgreSQL 연결 문자열 | (필수) |

`.env` 파일이 있으면 우선 로드한 뒤, OS 환경 변수가 있으면 동일한 키로 덮어쓴다.

## 활용 시나리오
- `wb_emit`로 발행한 테스트 이벤트가 DB까지 적재됐는지 즉시 확인.
- `wb_worker`, `wb_dlq_replayer`가 처리한 실제 이벤트를 샘플링해 존재 여부를 점검.
- 운영 중 특정 이벤트 ID가 중복 처리되었는지 여부를 빠르게 판단.
