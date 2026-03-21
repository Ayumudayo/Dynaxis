# 도구 인덱스

이 디렉터리는 Dynaxis 스택을 운영, 검증, 계측할 때 사용하는 도구를 모아 둔 곳이다. 각 도구는 역할이 다르고 실패 시 영향도도 다르기 때문에, “그냥 실행 파일 모음”으로 보면 유지보수가 어려워진다. 그래서 이 문서는 어떤 도구가 왜 존재하는지와 어디서 더 자세히 봐야 하는지를 먼저 정리한다.

## 핵심 도구 진입점

| 도구 | 경로 | 역할 |
| --- | --- | --- |
| 부하 생성기 | `tools/loadgen/` | transport-aware soak, latency, proof 시나리오를 실행한다 |
| 관리자 앱 | `tools/admin_app/` | admin/control-plane API와 UI를 제공한다 |
| 마이그레이션 도구 | `tools/migrations/` | 스키마 migration runner를 제공한다 |
| 초기 설정 도구 | `tools/provisioning/` | DB/bootstrap SQL helper를 제공한다 |
| 쓰기 지연 워커 | `tools/wb_worker/` | Redis Streams -> Postgres 영속화 워커 |
| 쓰기 지연 발행기 | `tools/wb_emit/` | write-behind 경로에 테스트/런타임 이벤트를 넣는다 |
| 적재 확인 도구 | `tools/wb_check/` | write-behind 영속화 결과를 검증한다 |
| DLQ 재처리기 | `tools/wb_dlq_replayer/` | dead-lettered write-behind 이벤트를 재처리한다 |

## 보조 스크립트/검증기

- `tools/gen_opcodes.py`
  - opcode 헤더를 생성한다
- `tools/gen_wire_codec.py`
  - wire codec 헤더를 생성한다
- `tools/gen_opcode_docs.py`
  - opcode 문서를 다시 생성한다
- `tools/check_core_api_contracts.py`
  - core API governance/boundary 검증
- `tools/check_doxygen_coverage.py`
  - Doxygen coverage 검증
- `tools/check_markdown_links.py`
  - 저장소 문서의 로컬 링크 스모크 검증

이 스크립트들이 중요한 이유는, 코드 생성과 문서 검증을 사람이 매번 수동으로 맞추면 drift가 쉽게 생기기 때문이다. 생성기와 검증기를 별도 도구로 유지하면 “어떤 파일이 source of truth인지”를 더 분명하게 지킬 수 있다.

## 관련 문서

- `docs/README.md`
  - 문서 전체 진입점
- `docs/tests.md`
  - 저장소 검증 진입점
