# 엔지니어링 워크플로우 및 작업 기조

이 문서는 루트 `AGENTS.md`에서 분리한 **상세 작업 기조, 주석·문서 기준, 완료 검증 절차**다. 코드베이스를 다루는 모든 에이전트(Codex, Antigravity, Claude Code, Cursor 등)는 언어·모델과 무관하게 동일한 기준을 따른다.

---

## 1. 엔지니어링 원칙

- **단순성 우선 (Simplicity First)**: 가장 단순하고 국소적인 구조로 문제를 해결한다. 불필요한 추상화 계층, 가상의 2호 앱용 프레임워크, 과도한 특수 분기를 추가하지 않는다.
- **근본 원인 해결 (No Laziness)**: 증상 회피나 임시 패치로 덮지 않는다. 스크롤, 타이밍, 레이스, 링크 오류 모두 재현 경로와 근본 원인을 확인한 뒤 수정한다.
- **최소 영향 범위 (Minimal Impact)**: 변경 목적이 요구하는 파일과 계층만 수정한다. `core/`는 채팅·운영 UI·배포 세부사항에 오염시키지 않으며, `server_core` 외곽 계층(`gateway`, `server`, `client_gui`, `admin_console`)이 단방향으로만 `core/`에 의존하도록 유지한다.
- **검증 완결 단위 (Proof-Complete Batching)**: 중간 잔음을 남기지 말고, 설계 → 구현 → 테스트 → Doxygen/계약·링크 검증 → 문서/작업 노트 최신화까지 한 단위로 닫은 뒤 보고한다.
- **우아한 설계 점검 (Balanced Elegance)**: 비자명한 변경 전 “더 명료한 공통 헬퍼나 상태 기계로 풀 수 있는가?”를 한 번 점검하되, 단순 수정은 과설계하지 않는다.

---

## 2. 주석 및 문서 하우스 스타일

1. **한국어 설명형 서술 (`docs/` 및 Doxygen)**:
   - 단순 직역이나 동작 나열에 그치지 않고 **무엇인지**, **왜 이 경계가 필요한지**, **이 경계를 어기면 어떤 장애·보안·유지보수 문제가 생기는지**를 설명한다.
   - 식별자, 클래스/함수/구조체 이름, 환경변수, 메트릭 이름, opcode, CLI 명령, 코드 블록은 원문 ASCII 표기와 백틱(`` ` ``)을 그대로 유지해 검색성과 계약 정확도를 보존한다.
2. **Doxygen 일관 유지**:
   - `core/include`, `server/include`, `gateway/include`, `client_gui/include`, `admin_console/include`의 공개 타입·함수와 핵심 런타임 소스를 변경할 때는 **같은 패치 안에서** `@brief`, `@param`, `@return` Doxygen 블록을 작성한다.
   - Doxygen 정비는 나중으로 미루지 않고 완료 게이트에서 `python tools/check_doxygen_coverage.py`로 반드시 확인한다.
3. **경로·문서 위생**:
   - 추적되는 `docs/`와 로컬 `tasks/` 마크다운에는 머신 종속 절대 경로 또는 `file://` URI를 커밋·기록하지 않는다. 저장소 상대 경로만 사용한다.

---

## 3. 완료 전 필수 검증 게이트

변경 범위에 맞춰 아래 명령으로 실제 통과를 확인한 뒤에만 완료로 표시한다.

- **전체 네이티브 빌드 및 테스트 (`Windows/Linux/macOS`)**:
  - `python scripts/build.py --profile full --config Debug --step configure` (타깃/파일 추가 시)
  - `python scripts/build.py --profile full --config Debug --step build`
  - `python scripts/build.py --profile full --config Debug --step test`
- **최소 코어 경계 확인 (`core` 또는 빌드 옵션 변경 시 필수)**:
  - `python scripts/build.py --profile core --config Debug --step all`
  - `python scripts/build.py --profile core --config Debug --step test`
- **클라이언트 단독 빌드 및 GUI 스모크 (`client_gui` 변경 시 필수)**:
  - `python scripts/build.py --profile client --config Release --step build`
  - `client_gui.exe --smoke-test` 및 `--workbench --smoke-test` 종료 코드 `0` 확인 (Linux CI는 `LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a`)
- **계약·Doxygen·문서·형상 게이트**:
  - `python tools/check_doxygen_coverage.py`
  - `python tools/check_core_api_contracts.py --check-boundary`
  - `python tools/gen_opcode_docs.py --check`
  - `python tools/check_markdown_links.py`
  - `git diff --check`

---

## 4. 로컬 작업 관리 및 자기 개선 루프

- **`tasks/` 작업 공간**: `.gitignore` 대상인 로컬 전용 계획·증적 공간이다([문서 인덱스](../README.md)에 적힌 추적 문서의 진실 원천을 대체하지 않는다). `tasks/README.md`를 인덱스로 두고 `tasks/<domain>/<topic>/todo.md` 및 `notes.md` 형태로 계획·진행·검증 결과를 기록한다.
- **교정 학습 (`tasks/_shared/lessons.md`)**: 사용자로부터 UX·아키텍처·절차 관련 지적을 받으면 즉시 재발 방지 패턴과 구체적 수치/규칙을 `tasks/_shared/lessons.md`에 추가하고, 관련 작업 시작 시 다시 확인한다.
- **작업트리/보존 경계**: `.worktrees/`와 참조 저장소는 일반 정리 대상에서 건드리지 않는다.
