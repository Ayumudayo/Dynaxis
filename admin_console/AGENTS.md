# admin_console/AGENTS.md

`admin_console/`는 Dynaxis 스택을 관측·제어하는 독립 웹 관리 콘솔 제어면(`admin_console_core`, `admin_app`)과 무빌드 정적 웹 UI(`admin_console/web/`)를 소유한다.

## 경계와 규칙
- **독립성 유지**: `core/`(`server_core`)의 Stable 헤더(`build_info.hpp`, `log.hpp` 등)만 참조하며, `core/src`나 `server/`/`gateway/` 내부 구현에 의존하지 않는다.
- **SSRF·컨테이너 소켓 금지**: 대상 서비스 주소는 기동 시 서버 카탈로그·토폴로지(`ADMIN_TOPOLOGY_PATH`)에서만 고정 결정하며, 브라우저 요청으로 임의 URL을 프록시하거나 `/var/run/docker.sock`을 마운트하지 않는다.
- **프론트엔드 규약**: `admin_console/web/`은 무빌드 Vanilla ES Modules + CSS 토큰으로 유지하며 `docs/agent-guides/frontend-design-system.md`를 따른다. 동적 서버 문자열 렌더링에 `innerHTML`을 쓰지 말고 `document.createElement`와 `textContent`만 사용한다.
- **상태 판정 및 보안**: `completed`(`migrator` 등 `one_shot`)와 `unknown`(관측 TTL 만료/타임아웃)을 `not_ready`와 엄격히 구분한다. Bearer 토큰·DB/Redis 비밀번호·채팅 본문 평문은 UI·로그·API 응답에 노출하지 않는다.
- **명시적 소스 목록**: `admin_console/CMakeLists.txt`에 `file(GLOB)`를 사용하지 않고 소스 파일을 명시적으로 나열한다.
- **Doxygen 검증**: `admin_console/include/admin/*.hpp` 공개 심볼 변경 시 한국어 Doxygen 주석을 유지하고 `python tools/check_doxygen_coverage.py`로 검증한다.
