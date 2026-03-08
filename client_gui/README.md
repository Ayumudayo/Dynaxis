# Client GUI (ImGui Client)

**Dynaxis** 프로젝트를 위한 차세대 그래픽 채팅 클라이언트입니다.
**Dear ImGui** 라이브러리를 기반으로 제작되었으며, 직관적인 UI와 한국어 입력 지원을 특징으로 합니다.

## ✨ 주요 기능 (Key Features)

-   **Modern UI**: VS 스타일의 Docking Layout을 지원하여 사용자가 원하는 대로 창 배치를 변경할 수 있습니다.
-   **Korean Support**: `Malgun Gothic`(맑은 고딕) 폰트를 시스템에서 동적으로 로드하여 완벽한 한국어 입출력을 지원합니다.
-   **Real-time Synchronization**: 서버의 상태 변경(유저 입장/퇴장, 방 변경 등)을 실시간으로 반영합니다.
-   **Clean Architecture**: Model(Data/Logic)과 View(UI)를 철저히 분리하여 유지보수성을 극대화했습니다.

## 🛠️ 빌드 방법 (Build Guide)

이 프로젝트는 루트 `CMakeLists.txt`에 포함되어 있어, 메인 빌드 시 자동으로 함께 빌드됩니다.
단독으로 빌드하려면 다음 명령어를 참고하세요.

```powershell
# 프로젝트 루트에서 실행 (권장: client-only release)
pwsh scripts/build.ps1 -ClientOnly -Target client_gui

# (직접 CMake 사용 시)
cmake --build build-windows-client --config Release --target client_gui
```

## 🚀 사용법 (Usage)

### 1. 연결 및 로그인
앱 실행 시 **로그인 모달**이 나타납니다.
-   **Host**: 서버 주소 (기본값: `127.0.0.1`)
-   **Port**: 서버 포트 (기본값: `36000` - 게이트웨이 포트)
-   **Username**: 사용할 닉네임

### 2. 채팅 및 명령어
기본적으로 입력한 텍스트는 현재 방의 모든 유저에게 전송됩니다.
다음과 같은 특수 명령어를 지원합니다.

*   `/join <방이름> [비밀번호]`: 방을 만들거나, 이미 존재하는 방에 입장합니다.
    *   예: `/join myroom` (공개방)
    *   예: `/join secret 1234` (비공개방)
*   `/w <닉네임> <메시지>`: 특정 유저에게 귓속말을 보냅니다.
    *   예: `/w user1 안녕하세요`
*   `/invite <닉네임> [방이름]`: 특정 유저를 방에 초대합니다. (룸 소유자/관리자)
*   `/kick <닉네임> [방이름]`: 특정 유저를 방에서 강퇴합니다. (룸 소유자/관리자)
*   `/room remove [방이름]`: 현재 방(또는 지정 방)을 제거하고 인원을 로비로 이동시킵니다. (룸 소유자/관리자)
*   `/block <닉네임>` / `/unblock <닉네임>` / `/blacklist list`: 개인 차단 목록을 관리합니다.
*   `/mute <닉네임> [초]`: 특정 유저를 일정 시간 뮤트합니다. (관리자 전용)
*   `/ban <닉네임> [초]`: 특정 유저를 일정 시간 밴합니다. (관리자 전용)
*   `/unmute <닉네임>` / `/unban <닉네임>` / `/gkick <닉네임>`: 전역 제재 해제/강제 종료 (관리자 전용)

알 수 없는 slash 명령은 서버 측 커맨드로 그대로 전달됩니다.

### 3. UI 조작
*   **Rooms 패널**: 현재 방 목록을 보여줍니다. 클릭하여 입장하거나, 우클릭 메뉴를 사용할 수 있습니다.
*   **Users 패널**: 현재 방에 있는 유저 목록입니다. 우클릭 컨텍스트 메뉴로 귓속말, 방 초대/강퇴를 바로 실행할 수 있고, 관리자 계정일 때만 뮤트/밴 메뉴가 표시됩니다.
*   **Leave 버튼**: 현재 방을 나가고 로비로 돌아갑니다.

## 🏗️ 아키텍처 (Architecture)

소스 코드는 **SRP(단일 책임 원칙)** 에 따라 두 가지 주요 레이어로 분리되어 있습니다.

| 클래스 | 역할 | 설명 |
| :--- | :--- | :--- |
| **`ClientApp`** | **Model & Controller** | 데이터 상태(`AppData`), 네트워크 통신(`NetClient`), 비즈니스 로직을 담당합니다. UI 코드와 독립적입니다. |
| **`GuiManager`** | **View** | GLFW 윈도우 관리, ImGui 렌더링, 사용자 입력을 담당합니다. `ClientApp`에 명령을 위임합니다. |

## 📁 디렉토리 구조

```
client_gui/
├── include/client/
│   ├── client_app.hpp  # 로직 헤더
│   ├── gui_manager.hpp # UI 헤더
│   └── net_client.hpp  # 네트워크 헤더
├── src/
│   ├── main.cpp        # 엔트리 포인트
│   ├── client_app.cpp  # 로직 구현
│   ├── gui_manager.cpp # UI 구현
│   └── net_client.cpp  # 네트워크 구현
└── CMakeLists.txt
```
