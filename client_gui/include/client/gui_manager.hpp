#pragma once

#include "client/client_app.hpp"
#include <string>

// 이미 ImGui 헤더가 필요하지만, 헤더 의존성을 줄이기 위해 전방 선언을 사용할 수도 있습니다.
// 여기서는 편의상 GuiManager.cpp에서 imgui 헤더를 포함하도록 유도하거나 필요한 경우 포함합니다.
struct GLFWwindow;

/**
 * @brief UI 상태 (UI State)
 * 
 * 데이터 상태(AppData)와 달리, 오직 "화면 표시"를 위해 일시적으로 필요한 값들입니다.
 * 예: 입력창의 버퍼, 모달 창의 열림 여부 등
 */
struct GuiState {
    // 로그인 입력값
    char host_buffer[128] = "127.0.0.1";
    int port = 36000;
    char username_buffer[128] = "guest";
    
    // 채팅 입력값
    char chat_input[256] = "";
    bool focus_chat_input = false;
    bool auto_scroll = true;

    // 방 만들기/입장 관련 상태
    bool show_password_modal = false;
    std::string target_room_to_join; // 목표 방 이름
    char password_buffer[64] = "";   // 비밀번호 입력 버퍼
};

/**
 * @brief GUI 매니저 클래스 (View)
 * 
 * - **역할 (Role)**:
 *   1. GLFW 윈도우 생성 및 OpenGL/ImGui 컨텍스트 초기화
 *   2. 매 프레임 UI를 그리기 (Render Loop)
 *   3. 사용자 입력을 받아 `ClientApp`에게 명령을 전달 (Delegation)
 * 
 * - **SOLID 원칙 적용**:
 *   - **SRP (단일 책임 원칙)**: 비즈니스 로직(네트워크, 데이터 관리)을 전혀 모르며, 오직 "보여주는 것"에만 집중합니다.
 *   - ClientApp을 참조로 받아 사용하므로, 의존성이 명확합니다.
 */
class GuiManager {
public:
    GuiManager(ClientApp& app);
    ~GuiManager();

    // 초기화 (윈도우 생성 등)
    bool init(int width, int height, const std::string& title);
    
    // 메인 루프 실행 (블로킹)
    void run();

    // 정리
    void shutdown();

private:
    // --- UI 컴포넌트 렌더링 메서드 ---
    
    // 전체 레이아웃 (DockSpace) 설정
    void render_dockspace();
    
    // 로그인 모달
    void render_login_modal();
    
    // 방 목록 패널
    void render_rooms_panel();
    
    // 유저 목록 패널
    void render_users_panel();
    
    // 채팅 패널
    void render_chat_panel();
    
    // 메뉴바
    void render_menubar();

private:
    ClientApp& app_; // 로직 및 데이터를 담당하는 객체 참조
    GuiState state_; // UI 전용 상태

    GLFWwindow* window_ = nullptr;
    bool layout_initialized_ = false;
};
