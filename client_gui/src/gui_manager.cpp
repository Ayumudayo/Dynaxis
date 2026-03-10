#include "client/gui_manager.hpp"
#include "client/client_app.hpp"

#include "imgui.h"
#include "imgui_internal.h"
#include "misc/cpp/imgui_stdlib.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include <cstdio>
#include <shlobj.h> // SHGetFolderPath

/**
 * @brief ImGui/GLFW 기반 UI 렌더링 및 입력 처리 구현입니다.
 *
 * 도킹 레이아웃, 로그인 모달, 채팅 패널 렌더링을 한곳에서 관리해
 * `ClientApp`의 상태를 시각적으로 표현하는 뷰 계층 책임을 분리합니다.
 */

// GLFW 오류 콜백
static void glfw_error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

GuiManager::GuiManager(ClientApp& app) : app_(app) {}

GuiManager::~GuiManager() {
    shutdown();
}

bool GuiManager::init(int width, int height, const std::string& title) {
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) return false;

    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    window_ = glfwCreateWindow(width, height, title.c_str(), NULL, NULL);
    if (window_ == NULL) return false;
    
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1); // V-Sync 활성화

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // 한글 폰트 로드
    ImFontConfig config;
    config.MergeMode = false;
    
    // 시스템 폰트 경로 동적 탐색 (SHGetFolderPath)
    char font_dir[MAX_PATH];
    std::string font_path;
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_FONTS, NULL, 0, font_dir))) {
        font_path = std::string(font_dir) + "\\malgun.ttf";
    }

    FILE* f = fopen(font_path.c_str(), "rb");
    if (f) {
        fclose(f);
        io.Fonts->AddFontFromFileTTF(font_path.c_str(), 18.0f, NULL, io.Fonts->GetGlyphRangesKorean());
    } else {
        // 폰트가 없으면 기본 폰트 사용 (한글 깨질 수 있음)
        io.Fonts->AddFontDefault();
    }

    return true;
}

void GuiManager::shutdown() {
    if (window_) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window_);
        glfwTerminate();
        window_ = nullptr;
    }
}

void GuiManager::run() {
    if (!window_) return;

    // 클리어 색상
    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
    
    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();

        // 1. 로직 업데이트 (이벤트 처리)
        app_.update();

        // 2. UI 렌더링 준비
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // 3. UI 컴포넌트 그리기
        render_dockspace();
        
        // 연결되지 않았다면 로그인 모달 띄우기
        const auto& data = app_.get_data();
        if (!data.is_connected) {
            ImGui::OpenPopup("Login");
        }
        
        render_login_modal();
        
        // 패널들 그리기
        // DockSpace 내부에 도킹되도록 하기 위해 각 Window 함수 호출
        render_rooms_panel();
        render_users_panel();
        render_chat_panel();

        // 데모 윈도우 (필요 시)
        // ImGui::ShowDemoWindow();

        // 4. 화면 출력 (Render)
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window_, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window_);
    }
}

void GuiManager::render_dockspace() {
    // 전체 화면을 덮는 DockSpace 생성
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    
    ImGui::Begin("MainDockSpace", nullptr, window_flags);
    ImGui::PopStyleVar(3);

    ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");

    // 최초 1회 레이아웃 초기화 (VS 스타일)
    if (!layout_initialized_ || ImGui::DockBuilderGetNode(dockspace_id) == NULL) {
        if (viewport->WorkSize.x > 0 && viewport->WorkSize.y > 0) {
            ImGui::DockBuilderRemoveNode(dockspace_id); 
            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_NoWindowMenuButton);
            ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

            ImGuiID dock_main_id = dockspace_id;
            ImGuiID dock_id_left = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Left, 0.25f, NULL, &dock_main_id);
            ImGuiID dock_id_left_bottom = ImGui::DockBuilderSplitNode(dock_id_left, ImGuiDir_Down, 0.50f, NULL, &dock_id_left);
            
            // 각 윈도우를 특정 독(Dock) ID에 할당
            ImGui::DockBuilderDockWindow("Rooms", dock_id_left);
            ImGui::DockBuilderDockWindow("Users", dock_id_left_bottom);
            ImGui::DockBuilderDockWindow("Chat", dock_main_id);

            ImGui::DockBuilderFinish(dockspace_id);
            layout_initialized_ = true;
        }
    }

    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    render_menubar(); // 메뉴바 렌더링

    ImGui::End();
}

void GuiManager::render_menubar() {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Exit")) glfwSetWindowShouldClose(window_, true);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Reset Layout")) { 
                layout_initialized_ = false; 
                ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
                ImGui::DockBuilderRemoveNode(dockspace_id); 
            } 
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}

void GuiManager::render_login_modal() {
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    // 항상 팝업이 열리도록 플래그 설정
    bool open = true;
    if (ImGui::BeginPopupModal("Login", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::Text("Connect to Dynaxis Server");
        ImGui::Separator();
        
        ImGui::InputText("Host", state_.host_buffer, sizeof(state_.host_buffer));
        ImGui::InputInt("Port", &state_.port);
        ImGui::InputText("Username", state_.username_buffer, sizeof(state_.username_buffer));
        ImGui::Separator();

        if (ImGui::Button("Connect", ImVec2(120, 0))) {
            if (app_.connect(state_.host_buffer, state_.port)) {
                // 연결 시도 성공 시 모달 닫기
                ImGui::CloseCurrentPopup();
                // 로그인 요청 전송
                app_.login(state_.username_buffer);
            } else {
                // 연결 실패 시 로그는 ClientApp 콜백에서 처리되지만, 여기서도 간단히 알림 가능
                // 현재는 ClientApp에 의존
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Exit", ImVec2(120, 0))) {
            glfwSetWindowShouldClose(window_, true);
        }
        ImGui::EndPopup();
    }
}

void GuiManager::render_rooms_panel() {
    ImGui::Begin("Rooms", NULL, ImGuiWindowFlags_NoCollapse);
    
    const auto& data = app_.get_data();

    if (data.is_connected) {
        ImGui::Text("Current: %s", data.current_room.c_str());

        // 로비가 아닐 때만 나가기 버튼 표시
        if (data.current_room != "lobby") {
            ImGui::SameLine();
            if (ImGui::Button("Leave")) {
                app_.leave_current_room();
            }
        }

        // 방 만들기 버튼
        if (ImGui::Button("Make Room", ImVec2(-1, 0))) {
             ImGui::OpenPopup("Create Room");
             state_.target_room_to_join = ""; 
             state_.password_buffer[0] = '\0';
        }

        // 방 만들기 모달
        if (ImGui::BeginPopupModal("Create Room", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
             static char new_room_name[64] = "";
             ImGui::InputText("Room Name", new_room_name, sizeof(new_room_name));
             ImGui::InputText("Password (Optional)", state_.password_buffer, sizeof(state_.password_buffer), ImGuiInputTextFlags_Password);
             
             if (ImGui::Button("Create", ImVec2(120, 0))) {
                 if (strlen(new_room_name) > 0) {
                     app_.join_room(new_room_name, state_.password_buffer);
                     ImGui::CloseCurrentPopup();
                     new_room_name[0] = '\0'; 
                 }
             }
             ImGui::SameLine();
             if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                 ImGui::CloseCurrentPopup();
             }
             ImGui::EndPopup();
        }

        ImGui::Separator();

        // 방 목록 리스트박스
        for (const auto& r : data.room_list) {
            std::string label = r.name + (r.locked ? " [L]" : "");
            
            // 선택 가능 항목
            if (ImGui::Selectable(label.c_str(), r.name == data.current_room)) {
                // 더블클릭 대신 클릭 시 바로 입장? 아니면 클릭은 선택만?
                // 여기서는 기존 로직대로 클릭 시 입장 시도
                if (r.locked) {
                    state_.target_room_to_join = r.name;
                    state_.show_password_modal = true;
                } else {
                    app_.join_room(r.name, "");
                }
            }
            // 우클릭 컨텍스트 메뉴
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Join")) {
                    if (r.locked) {
                        state_.target_room_to_join = r.name;
                        state_.show_password_modal = true;
                    } else {
                        app_.join_room(r.name, "");
                    }
                }
                ImGui::EndPopup();
            }
        }
        
        if (data.room_list.empty()) ImGui::TextDisabled("No rooms listed");
        
        ImGui::Separator();
        
        // 빠른 입장
        static char join_buf[64] = "";
        ImGui::InputText("Join...", join_buf, sizeof(join_buf));
        ImGui::SameLine();
        if (ImGui::Button("Go") && join_buf[0] != 0) {
            app_.join_room(join_buf, "");
            join_buf[0] = 0;
        }

    } else {
        ImGui::Text("Not connected.");
    }
    ImGui::End();

    // 비밀번호 입력 모달 (비동기적으로 열릴 수 있음)
    if (state_.show_password_modal) {
        ImGui::OpenPopup("Join Locked Room");
        state_.show_password_modal = false;
        state_.password_buffer[0] = '\0';
    }
    
    if (ImGui::BeginPopupModal("Join Locked Room", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter password for: %s", state_.target_room_to_join.c_str());
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(0);
        
        ImGui::InputText("Password", state_.password_buffer, sizeof(state_.password_buffer), ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
        
        if (ImGui::Button("Join", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            app_.join_room(state_.target_room_to_join, state_.password_buffer);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void GuiManager::render_users_panel() {
    ImGui::Begin("Users", NULL, ImGuiWindowFlags_NoCollapse);
    const auto& data = app_.get_data();

    if (data.is_connected) {
        const auto issue_command = [this, &data](const std::string& cmd) {
            if (data.is_logged_in) {
                app_.process_command(cmd);
            }
        };

        ImGui::Text("Online Users (%d)", (int)data.user_list.size());
        ImGui::Separator();
        for (const auto& u : data.user_list) {
            ImGui::Selectable(u.c_str());
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Whisper")) {
                    // 채팅창에 귓속말 명령어 자동 완성
                    snprintf(state_.chat_input, sizeof(state_.chat_input), "/w %s ", u.c_str());
                    state_.focus_chat_input = true;
                }
                const bool room_action_enabled = (data.current_room != "lobby");
                if (ImGui::MenuItem("Invite To Current Room", nullptr, false, room_action_enabled)) {
                    issue_command("/invite " + u + " " + data.current_room);
                }
                if (ImGui::MenuItem("Kick From Current Room", nullptr, false, room_action_enabled)) {
                    issue_command("/kick " + u + " " + data.current_room);
                }
                if (data.is_admin && ImGui::BeginMenu("Admin Mute")) {
                    if (ImGui::MenuItem("30s")) {
                        issue_command("/mute " + u + " 30");
                    }
                    if (ImGui::MenuItem("5m")) {
                        issue_command("/mute " + u + " 300");
                    }
                    if (ImGui::MenuItem("30m")) {
                        issue_command("/mute " + u + " 1800");
                    }
                    ImGui::EndMenu();
                }
                if (data.is_admin && ImGui::BeginMenu("Admin Ban")) {
                    if (ImGui::MenuItem("10m")) {
                        issue_command("/ban " + u + " 600");
                    }
                    if (ImGui::MenuItem("1h")) {
                        issue_command("/ban " + u + " 3600");
                    }
                    if (ImGui::MenuItem("24h")) {
                        issue_command("/ban " + u + " 86400");
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem("Block")) {
                    // 차단 기능 (예시)
                }
                ImGui::EndPopup();
            }
        }
    }
    ImGui::End();
}

void GuiManager::render_chat_panel() {
    ImGui::Begin("Chat");
    const auto& data = app_.get_data();

    // 1. 채팅 로그 스크롤 영역
    ImGui::BeginChild("ScrollingRegion", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), false, ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& item : data.logs) {
        ImGui::TextUnformatted(item.c_str());
    }
    // 자동 스크롤
    if (state_.auto_scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    
    ImGui::Separator();
    
    // 2. 채팅 입력창
    ImGui::PushItemWidth(-1);
    if (state_.focus_chat_input) {
        ImGui::SetKeyboardFocusHere();
        state_.focus_chat_input = false;
    }
    if (ImGui::InputText("##Input", state_.chat_input, sizeof(state_.chat_input), ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (data.is_connected && data.is_logged_in) {
             app_.process_command(state_.chat_input);
             state_.chat_input[0] = '\0';
             ImGui::SetKeyboardFocusHere(-1); // 포커스 유지
        }
    }
    ImGui::PopItemWidth();
    if (data.is_admin) {
        ImGui::TextDisabled("/invite /kick /mute /ban /unmute /unban /gkick");
    } else {
        ImGui::TextDisabled("/invite /kick /block /unblock /blacklist");
    }
    
    ImGui::End();
}
