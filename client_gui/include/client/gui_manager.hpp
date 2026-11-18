#pragma once

#include "client/client_app.hpp"
#include "client/client_preferences.hpp"

#include <cstddef>
#include <filesystem>
#include <string>

struct GLFWwindow;

/**
 * @brief 클라이언트 화면의 표현 모드를 정의합니다.
 *
 * 기본 `MESSENGER` 모드는 도킹 탭바와 진단 패널을 숨긴 깔끔한 대화 화면을 제공하고,
 * `WORKBENCH` 모드는 세션 연속성·스냅샷·RTT 진단 서랍과 자유 도킹을 활성화합니다.
 */
enum class UiMode {
    MESSENGER,
    WORKBENCH
};

/**
 * @brief 타임라인에 표시할 메시지 종류 필터입니다.
 */
enum class TimelineFilter {
    ALL,
    WHISPERS,
    WHISPER_PEER
};

/**
 * @brief 화면 입력 버퍼와 모달·스크롤 상태를 보관하는 UI 전용 상태 구조체입니다.
 *
 * `AppData`와 달리 서버 계약과 무관한 일시적 입력값, 팝업 열림 여부, 필터 상태만 유지합니다.
 */
struct GuiState {
    char host_buffer[128] = "127.0.0.1";
    int port = 6000;
    char username_buffer[128] = "guest";
    char access_token_buffer[4096] = "";
    bool resume_session = false;
    bool show_connection_popup = false;

    char room_search_buffer[64] = "";
    char quick_join_buffer[64] = "";
    char new_room_buffer[64] = "";
    bool show_create_room_modal = false;
    bool show_password_modal = false;
    std::string target_room_to_join;
    char password_buffer[64] = "";
    TimelineFilter timeline_filter = TimelineFilter::ALL;
    std::string selected_whisper_peer;
    bool hide_blocked_messages = false;
    int revealed_blocked_line_index = -1;
    bool show_timeline_search = false;
    bool focus_timeline_search = false;
    char timeline_search_buffer[128] = "";

    char chat_input[kChatInputBufferBytes] = "";
    std::string pending_chat_input;
    bool move_cursor_to_end = false;
    std::string active_whisper_target;
    bool focus_chat_input = false;
    bool auto_scroll = true;
    bool has_unread_below = false;
    std::size_t last_seen_line_count = 0;
    InputHistory input_history;
    std::string copied_feedback_key;
    double copied_feedback_until = 0.0;
    char protocol_filter_buffer[64] = "";
    bool hide_ping_pong_logs = true;
    bool protocol_auto_scroll = true;
};

/**
 * @brief ImGui/GLFW/OpenGL 기반의 메신저 및 개발 워크벤치 뷰 매니저입니다.
 *
 * `ClientApp`의 UI 독립 상태를 받아 4-영역 레이아웃(상단 상태 바, 좌측 내비게이션,
 * 중앙 구조화 타임라인, 우측 참여자 및 조건부 진단 서랍)으로 렌더링합니다.
 */
class GuiManager {
public:
    /**
     * @brief `ClientApp` 참조와 초기 UI 모드를 받아 `GuiManager`를 생성합니다.
     * @param app 상태와 명령 실행을 담당하는 애플리케이션 컨트롤러 참조
     * @param initial_mode 시작 시 적용할 UI 모드(기본값 `UiMode::MESSENGER`)
     */
    explicit GuiManager(ClientApp& app, UiMode initial_mode = UiMode::MESSENGER);

    /** @brief 열려 있는 GLFW/ImGui 자원을 정리합니다. */
    ~GuiManager();

    /**
     * @brief GLFW 윈도우, OpenGL 컨텍스트, ImGui 스타일 및 폰트를 초기화합니다.
     * @param width 초기 창 너비(px)
     * @param height 초기 창 높이(px)
     * @param title 창 제목 문자열
     * @return 초기화에 성공하면 true, 실패하면 false
     */
    bool init(int width, int height, const std::string& title);

    /**
     * @brief 메인 렌더링 루프를 실행합니다.
     * @param frame_limit 0이면 창이 닫힐 때까지 실행하고, 양수이면 해당 프레임 수 뒤 반환합니다.
     */
    void run(unsigned int frame_limit = 0);

    /**
     * @brief ImGui 백엔드와 GLFW 창을 안전하게 종료합니다.
     */
    void shutdown();

    /**
     * @brief 화면 표시 모드를 전환하고 도킹 레이아웃 재구성을 예약합니다.
     * @param mode 적용할 `UiMode`
     */
    void set_mode(UiMode mode);

    /**
     * @brief `MESSENGER` 모드와 `WORKBENCH` 모드를 상호 전환합니다.
     */
    void toggle_mode();

    /**
     * @brief 현재 활성화된 UI 모드를 반환합니다.
     * @return 현재 적용 중인 `UiMode`
     */
    UiMode mode() const { return mode_; }

    /**
     * @brief 파일 또는 CLI에서 로드된 비비밀 환경설정과 저장 경로를 적용합니다.
     * @param prefs 적용할 비비밀 `ClientPreferences`
     * @param persist_path 변경 시 저장할 설정 파일 경로(비어 있으면 디스크 저장 비활성화)
     */
    void apply_preferences(const ClientPreferences& prefs, const std::filesystem::path& persist_path = {});

    /**
     * @brief 현재 UI·애플리케이션 상태에서 비비밀 설정 스냅샷을 수집합니다.
     * @return 현재 설정값을 담은 `ClientPreferences`
     */
    ClientPreferences current_preferences() const;

    /**
     * @brief 현재 비비밀 설정 스냅샷을 설정된 경로(`prefs_path_`)에 저장합니다.
     */
    void save_preferences() const;

private:
    void apply_slate_dark_theme(float dpi_scale);
    void load_fonts();
    void set_text_zoom(float zoom);
    void render_root_layout();
    void render_menubar();
    void render_top_bar();
    void render_connection_popup();
    void render_navigation_panel();
    void render_chat_panel();
    void render_empty_timeline_card(bool has_search);
    void render_timeline_line_context_menu(const ChatLogLine& item,
                                           std::size_t line_idx,
                                           bool is_self,
                                           bool is_blocked);
    void render_participants_panel();
    void render_diagnostics_panel();

    ClientApp& app_;
    GuiState state_;
    UiMode mode_{UiMode::MESSENGER};
    std::filesystem::path prefs_path_;

    GLFWwindow* window_{nullptr};
    bool layout_initialized_{false};
    float dpi_scale_{1.0f};
    float text_zoom_{1.0f};
};
