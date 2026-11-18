# 프론트엔드·UI 공통 디자인 시스템 및 불변식

이 문서는 C++ 데스크톱 클라이언트(`client_gui`, Dear ImGui)와 웹 관리 콘솔(`admin_console/web`, HTML/CSS/ES Modules)을 담당하는 **모든 에이전트가 동일한 시각 언어, 정보 밀도, 안전 불변식을 유지**하도록 정의한 공통 규격이다. 세부 기능 사양은 [클라이언트 워크벤치 설계](../architecture/client-workbench-design.md)와 [웹 관리 콘솔 설계](../architecture/web-admin-console.md)를 따른다.

---

## 1. 공통 시각 정체성 — Slate Dark 팔레트

어두운 슬레이트 서피스와 단일 블루 포인트 액센트만 사용한다. **무지개색 배지 남발, 네온 보라·핑크 그라데이션, 과도한 글로우(glow) 효과를 금지**한다.

| 역할 토큰 (CSS 변수) | Hex 값 | `ImGui` (`ImVec4`) 대응 | 용도 |
|---|---|---|---|
| `--bg-app` | `#16181D` | `(0.086, 0.094, 0.114, 1.00)` | 최하단 앱 배경 및 뷰포트 캔버스 |
| `--bg-surface` | `#1E2028` | `(0.106, 0.114, 0.141, 1.00)` | 도킹 패널·사이드바·인셋 목록 서피스 |
| `--bg-elevated` | `#262A36` | `(0.137, 0.153, 0.196, 1.00)` | 입력 필드·카드·컨텍스트 드로어 표면 |
| `--bg-hover` | `#2E3444` | `(0.173, 0.192, 0.247, 1.00)` | 호버/선택 행 강조 |
| `--border-subtle` | `#2A2E39` | `(0.165, 0.180, 0.224, 1.00)` | 패널 경계선 및 구분선 |
| `--accent-primary` | `#3B82F6` | `(0.210, 0.420, 0.900, 1.00)` | 기본 CTA 버튼, 활성 필터, 포커스 링 |
| `--accent-hover` | `#2563EB` | `(0.260, 0.480, 0.960, 1.00)` | 강조 버튼 호버 |
| `--text-primary` | `#ECF0F7` | `(0.925, 0.941, 0.969, 1.00)` | 본문 및 제목 텍스트 |
| `--text-secondary` | `#9AA4B8` | `(0.600, 0.640, 0.720, 1.00)` | 보조 설명 및 메타 라벨 |
| `--text-muted` | `#6E788C` | `(0.560, 0.610, 0.700, 1.00)` | 비활성 힌트·플레이스홀더 |

---

## 2. 5종 운영 상태 시맨틱 (색상 + 아이콘 + 라벨 동시 표기)

상태는 절대 색상(점·테두리)만으로 구분하지 않는다. 모든 상태 배지·셀은 **색상 + 기호 아이콘 + 텍스트 라벨** 3요소를 함께 렌더링한다.

| 상태 키 | Hex | 필수 표시 | 의미 | 오판 방지 포인트 |
|---|---|---|---|---|
| `ready` | `#22C55E` | `● Ready` | 상시 서비스 `/healthz` 및 `/readyz` 통과 | - |
| `degraded` | `#F59E0B` | `▲ Degraded` | 일부 인스턴스 비준비 또는 DLQ/지연 임계 초과 | 전체 불통과 구분 |
| `not_ready` | `#EF4444` | `✕ Not Ready` | `/healthz` 또는 `/readyz` 실패, 필수 의존성 단절 | 즉시 조치 대상 |
| `completed` | `#94A3B8` | `✓ Completed` | `migrator` 등 `one-shot` 작업이 정상 종료됨 | 종료를 장애 빨강으로 오표시 금지 |
| `unknown` | `#6B7280` | `? Unknown / Stale` | 타임아웃·연결 실패 또는 관측 TTL 만료 | 오래된 `ready` 캐시 재사용 금지 |

---

## 3. 타이포그래피·고정폭 숫자·밀도 규약

1. **한국어 가독성 및 폰트 스택**:
   - 웹: `-apple-system, BlinkMacSystemFont, "Pretendard", "Malgun Gothic", "Noto Sans KR", "Segoe UI", sans-serif`
   - 데스크톱 (`client_gui`): `segoeui.ttf` + `malgun.ttf` 병합(`PixelSnapH`, `OversampleH=1`, `OversampleV=1`, `RasterizerMultiply=1.15f`).
2. **고정폭 식별자 및 숫자 (`tabular-nums`)**:
   - `room_id`, `principal_id`(`subject`), `logical_session_id`, `operation_id`, `idempotency_key`, 포트, 지연(`ms`), `watermark`, 시각은 반드시 `monospace`와 `font-variant-numeric: tabular-nums`를 사용한다. 수 초 간격 자동 폴링에도 테이블 열 폭이 흔들리지 않아야 한다.
3. **레이아웃 및 단일 스크롤 영역 원칙**:
   - 바깥 뷰포트 셸과 도킹 패널(`Navigation`, `Chat`, `Participants`, 웹 앱 셸) 자체에는 스크롤바가 생기지 않게 한다(`NoScrollbar`, `overflow: hidden`). 실제 타임라인, 목록, 드로어 본문 영역만 내부 스크롤을 가진다.
   - 어두운 인셋 목록 박스에는 반드시 내부 패딩(`ImGuiChildFlags_AlwaysUseWindowPadding` 또는 CSS 패딩 `8px~12px`)을 적용해 빈 상태 문구가 박스 모서리에 들러붙지 않게 한다.
4. **디스코드 수준 정보 밀도**:
   - 대화·프로토콜 로그 연속 행 간격은 `2px`(DPI 스케일 반영) 수준으로 촘촘하게 유지하고, `[SYS]`/`[ERR]`/프로토콜 이벤트는 2줄 대형 카드 대신 1줄 인라인 배지로 렌더링한다.
   - 사이드바 최상단에 대형 라디오/체크박스 컨트롤을 배치하지 않는다. 헤더의 컴팩트 토글이나 좁은 패딩의 조건부 컨트롤로 공간을 아낀다.
5. **사용자 중심 단위 & CJK 입력 UX**:
   - 입력 제한은 원시 바이트(`B`)가 아니라 글자 수(유니코드 `1,000`자 / 로마자 계열 `2,000`자)로, 토큰 만료 시각은 원시 epoch ms가 아니라 남은 TTL(`14m 05s`)로 표시한다.
   - 데스크톱 한글 입력은 글자를 가리는 플로팅 조합 박스를 띄우지 않고 입력 버퍼 커서 위치에서 자모가 직접 조합되도록 유지한다.

---

## 4. 프론트엔드 스택 및 보안·프라이버시 경계

- **`client_gui` (C++20 / ImGui / GLFW / OpenGL)**: UI 비의존 상태 계층(`MessageTimeline`, `ClientApp`, `ClientPreferences`)이 ImGui/GLFW 헤더를 절대 참조하지 않는다.
- **`admin_console/web` (무빌드 Vanilla ES Modules + CSS 디자인 토큰)**:
  - 동적 서버 문자열을 렌더링할 때 `innerHTML` 문자열 조합을 금지하고 `document.createElement` + `textContent`로 DOM을 안전하게 조립한다(XSS 원천 차단).
  - 단계적 기능 노출(`v1` Overview 활성, `v2`/`v3` 메뉴는 도입 전까지 비활성/잠금 사유 표시)과 **우측 컨텍스트 슬라이드오버 드로어** 패턴을 유지한다.
- **절대 비노출 원칙**:
  - `access_token`, `resume_token`, `CHAT_AUTH_SIGNING_SECRET`, DB/Redis 비밀번호, `.env` 원문은 UI·로컬 파일·URL 쿼리·로그 어디에도 노출/저장하지 않는다. (관리 콘솔 Bearer 토큰 입력은 메모리/세션 저장소인 `sessionStorage` 범위에서만 취급한다.)
  - 관리 콘솔은 일반 채팅 메시지 본문 평문 뷰어를 제공하지 않는다.
