# C++ development client

서버의 로그인·메시지·귓속말·차단·재접속을 확인하는 C++/Dear ImGui 클라이언트다. GUI 스택은 ImGui/GLFW/OpenGL로 통일하며 Windows와 Linux에서 같은 소스를 사용한다. `IClientTransport`, `NetClient`, `ClientApp`, `MessageTimeline`은 ImGui/GLFW를 직접 참조하지 않아 UI 교체 시 통신·세션·타임라인 병합 코드를 그대로 재사용할 수 있다. 빌드 절차와 플랫폼 준비 사항은 [공통 빌드](../docs/build.md)를 따른다.

## 🛠️ 빌드 및 실행 (Build & Run)

이 프로젝트는 루트 `CMakeLists.txt`에 포함되어 있어, 메인 빌드 시 자동으로 함께 빌드됩니다.
단독으로 빌드하고 실행하려면 다음 명령어를 참고하세요.

```powershell
# 프로젝트 루트에서 실행 (권장: client-only release)
python scripts/build.py --profile client --config Release --step all

# 기본 메신저 모드 실행
.\build\windows\client\release\client_gui\Release\client_gui.exe

# 개발 워크벤치 모드로 바로 실행 (실행 중 F12 또는 View 메뉴로도 전환 가능)
.\build\windows\client\release\client_gui\Release\client_gui.exe --workbench

# 비비밀 접속 환경설정 재정의 실행 (access_token은 디스크에 저장하지 않음)
.\build\windows\client\release\client_gui\Release\client_gui.exe --host 127.0.0.1 --port 6000 --user alice --auto-resume
```

## 🚀 사용법 (Usage)

### 1. 연결 및 로그인
기본 화면에서는 연결 상태가 상단 상태 바와 비차단 **Quick Connect 카드**로 표시되므로 연결이 끊어져도 기존 타임라인과 오류 로그를 볼 수 있습니다.
상단 **`Connection...`** 버튼 또는 **`Connection > Connection Settings...`** 메뉴에서 세부 설정을 열 수 있습니다.
-   **Host**: 서버 주소 (기본값: `127.0.0.1`)
-   **Port**: 서버 포트 (기본값: `6000` - 로컬 Docker 스택의 HAProxy 진입점)
-   **Username**: 사용할 닉네임
-   **Access token**: `signed` 모드의 발급 자격. `development` 모드에서는 비워 둔다. [인증 계약](../docs/ops/authenticated-chat.md)을 따른다.
-   **Resume previous session**: 마지막 로그인에서 보관된 재접속 자격으로 상태 복원을 요청한다(토큰 원문은 화면·로그에 표시하지 않음).
-   **비비밀 로컬 설정 저장 및 CLI 옵션**: 호스트, 포트, 닉네임, UI 모드, 텍스트 배율, 차단 숨김, `Auto-Resume`, `Auto Ping` 설정은 `%LOCALAPPDATA%\Dynaxis\client_gui_prefs.cfg` (Linux: `$XDG_CONFIG_HOME/dynaxis/client_gui_prefs.cfg`, `DYNAXIS_CLIENT_CONFIG` 또는 `--config <path>`로 변경 가능)에 자동 저장되며, `--host`, `--port`, `--user`, `--workbench`, `--messenger`, `--auto-resume`, `--no-auto-resume` 인자로 덮어쓸 수 있다(`access_token`·`resume_token`·방 비밀번호는 절대 저장하지 않음).
-   **선택형 Auto-Resume 백오프**: `Auto-Resume`를 켜 두면 예기치 않은 연결 끊김 시 최대 5회(`1s -> 2s -> 4s -> 8s -> 16s`) 지수 백오프로 자동 `Resume`을 시도하며, 수동 `Disconnect`/`Drop Socket`이나 토큰 만료 시에는 자동 재시도를 중단합니다.

### 2. 화면 모드 (`Messenger` vs `Workbench`)
-   **Messenger 모드(기본값)**: 도킹 탭바와 진단 서랍을 숨긴 3단 메신저 레이아웃(`Navigation`, `Chat`, `Participants`)을 표시합니다.
-   **Developer Workbench 모드 (`F12` 또는 `--workbench`)**: 우측 하단에 **`Diagnostics` 서랍**을 열어 **`Inspector` 탭**(연결 상태, 10초 주기 `Auto Ping` 토글과 Min/Avg/Max RTT 요약, `Resume TTL` 실시간 카운트다운, `logical_session_id`, `auth_session_id`, `room_id`, `watermark`, `history_available`, 원클릭 `Copy`, 의도적 단절/`Reconnect + Resume`) 및 **`Protocol Log` 탭**(최대 250건 송수신·수명주기 링 버퍼, 키워드 필터, `Hide PING/PONG`, 복사/초기화)을 제공합니다. 자유 도킹 및 레이아웃 초기화(`View > Reset Layout`)도 사용할 수 있습니다.
-   **텍스트 배율 및 HiDPI 표시 (`View > Text Size`)**: 모니터 DPI 스케일에 맞춰 폰트와 UI 간격을 자동 조정하며, 메뉴 또는 단축키(`Ctrl+=` 확대, `Ctrl+-` 축소, `Ctrl+0` 기본값 복원)로 런타임 텍스트 크기를 조절할 수 있습니다.

### 3. 채팅 및 명령어
입력창은 일반 대화, 귓속말 대상 고정 칩, `/` 시작 시 명령어 자동완성 제안(`Tab` 또는 제안 항목 클릭), 직전 메시지·명령어 탐색(`Up`/`Down` 히스토리), **유니코드 `1,000`자 / 로마자 계열 `2,000`자** 실시간 글자수 제한·카운터, 그리고 Windows에서 별도 조합 박스 없이 입력창 내부 커서 위치에서 자모가 바로 갱신되는 한글/CJK IME 조합 및 단일 `Enter` 전송을 지원합니다.
다음과 같은 특수 명령어를 지원합니다.

*   `/join <방이름> [비밀번호]`: 방을 만들거나, 이미 존재하는 방에 입장합니다.
    *   예: `/join myroom` (공개방)
    *   예: `/join secret 1234` (비공개방)
*   `/w <닉네임> <메시지>`: 특정 유저에게 귓속말을 보냅니다.
    *   예: `/w user1 안녕하세요`
*   `/leave`: 현재 방을 나가 로비로 돌아갑니다.
*   `/refresh`: 현재 방 스냅샷을 다시 요청합니다.
*   `/invite <닉네임> [방이름]`: 특정 유저를 방에 초대합니다. (룸 소유자/관리자)
*   `/kick <닉네임> [방이름]`: 특정 유저를 방에서 강퇴합니다. (룸 소유자/관리자)
*   `/room remove [방이름]`: 현재 방(또는 지정 방)을 제거하고 인원을 로비로 이동시킵니다. (룸 소유자/관리자)
*   `/block <닉네임>` / `/unblock <닉네임>` / `/blacklist list`: 개인 차단 목록을 관리합니다.
*   `/mute <닉네임> [초]` / `/unmute <닉네임>`: 특정 유저를 일정 시간 뮤트/해제합니다. (관리자 전용)
*   `/ban <닉네임> [초]` / `/unban <닉네임>` / `/gkick <닉네임>`: 전역 제재 및 강제 종료 (관리자 전용)

### 4. UI 조작
*   **Navigation 패널**: 내부 패딩이 적용된 방 목록과 검색·생성(`+ New`), 공개/잠금방 입장, `RECENT WHISPERS` 1:1 스레드 전환·미읽음 배지·우클릭 메뉴, `QUICK JOIN`, 우클릭 방 제거를 제공합니다.
*   **Chat 패널**: 상단 `Whispers (N)` / `Find` / `Back to #<room>` 툴바, 연속 발신자 60초 그룹화·디스코드 수준 밀도 정렬, 귓속말 좌측 액센트 카드와 한 줄 인라인 `[SYS]`/`[ERR]` 배지, `Ctrl+F`/`Find` 인라인 타임라인 검색, 차단 사용자 메시지 접기(클릭 임시 펼치기)·숨김(`View > Hide Blocked Messages`), 미연결/빈 방 온보딩 카드, 메시지 우클릭 컨텍스트 메뉴(텍스트/서식/ID 복사, `@귓속말`, 1:1 스레드 열기, 차단/해제), 입력 히스토리(`Up`/`Down`)와 글자수 제한 카운터(유니코드 `1,000`자 / 로마자 `2,000`자)를 제공합니다.
*   **Participants 패널**: 현재 방 참여자 목록, 차단 동기화(`Sync Blocklist`), 차단 사용자가 있을 때만 하단에 표시되는 `BLOCKED (N)` 목록 및 컴팩트 `Hide in chat` 토글을 표시합니다. 우클릭 메뉴로 귓속말, 1:1 스레드 열기, 초대/강퇴, 차단/해제(`Block`/`Unblock`), 관리자 전용 뮤트/밴/`gkick`을 실행할 수 있습니다.

## 🏗️ 아키텍처 (Architecture)

소스 코드는 **단일 책임 원칙(SRP)** 과 **의존성 역전(DIP)** 에 따라 전송·상태·뷰 레이어로 분리되어 있습니다.

| 컴포넌트 | 역할 | 설명 |
| :--- | :--- | :--- |
| **`IClientTransport` / `NetClient`** | **Transport** | 추상 전송 인터페이스와 Boost.Asio TCP/Protobuf/RTT 구현입니다. |
| **`ClientApp` / `MessageTimeline`** | **Model & Controller** | UI 비의존 상태(`AppData`), 구조화 타임라인, 명령 처리, 블랙리스트 동기화를 담당합니다. |
| **`GuiManager`** | **View** | GLFW 윈도우, 슬레이트 다크 테마, `Messenger`/`Workbench` 모드 전환과 ImGui 렌더링을 담당합니다. |

## 📁 디렉토리 구조

```text
client_gui/
├── include/client/
│   ├── client_app.hpp       # 상태·컨트롤러 헤더
│   ├── client_transport.hpp # UI/테스트 분리를 위한 전송 인터페이스
│   ├── gui_manager.hpp      # UI 매니저 헤더
│   ├── message_timeline.hpp # 구조화 타임라인 모델
│   └── net_client.hpp       # Boost.Asio TCP 클라이언트 헤더
├── src/
│   ├── main.cpp             # 엔트리 포인트 (--smoke-test, --workbench)
│   ├── client_app.cpp       # 로직 구현
│   ├── gui_manager.cpp      # UI 구현
│   └── net_client.cpp       # 네트워크 구현
└── CMakeLists.txt
```
