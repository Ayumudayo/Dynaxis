#include "client/client_app.hpp"
#include "client/gui_manager.hpp"

#if defined(_MSC_VER) && (_MSC_VER >= 1900) && !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
#pragma comment(lib, "legacy_stdio_definitions")
#endif

/**
 * @brief ImGui 개발용 클라이언트 진입점입니다.
 *
 * UI 수명주기(`GuiManager`)와 로직 수명주기(`ClientApp`)를 분리해,
 * 네트워크/상태 로직과 렌더링 계층을 독립적으로 유지합니다.
 */
int main(int, char**)
{
    // 1. 비즈니스 로직 & 데이터 상태 관리자 생성
    ClientApp app;

    // 2. UI 매니저 생성 (앱과 연결)
    GuiManager gui(app);

    // 3. UI 초기화 (윈도우 생성 등)
    if (!gui.init(1280, 720, "Knights ImGui Client (Refactored)")) {
        return 1;
    }

    // 4. 메인 루프 실행
    gui.run();

    return 0;
}
