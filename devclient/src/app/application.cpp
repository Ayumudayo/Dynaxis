#include "client/app/application.hpp"

#include "client/app/app_state.hpp"
#include "client/app/command_processor.hpp"
#include "client/app/network_router.hpp"
#include "client/app/ui_builder.hpp"
#include "client/net_client.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include "server/core/util/paths.hpp"

namespace client::app {

namespace {
constexpr const char* kDefaultHost = "127.0.0.1";
constexpr unsigned short kDefaultPort = 6000;
constexpr const char* kEnvHost = "DEVCLIENT_HOST";
constexpr const char* kEnvPort = "DEVCLIENT_PORT";
} // namespace

// -----------------------------------------------------------------------------
// Application::Impl 클래스
// -----------------------------------------------------------------------------
// UI 구성 요소, 명령 처리기, 네트워크 클라이언트를 묶어 운영하는 내부 구현체입니다.
// Pimpl 관용구를 사용하여 헤더 의존성을 줄입니다.
class Application::Impl {
public:
    Impl(std::string host, unsigned short port, bool allow_env_override);
    ~Impl() = default;

    // 메인 루프 실행
    int Run();

private:
    void AppendInitialLogs();
    void Connect();
    void LoadEnvironment();
    static unsigned short ParsePort(const char* value, unsigned short fallback);

    // FTXUI 화면 객체 (전체 화면 모드)
    ftxui::ScreenInteractive screen_;
    
    // 애플리케이션 상태 (채팅 로그, 현재 방, 사용자 정보 등)
    AppState state_;
    
    // 네트워크 클라이언트 (TCP 소켓 통신)
    ::NetClient net_;
    
    // UI 갱신 요청 콜백 (스레드 안전하게 UI 이벤트를 발생시킴)
    std::function<void()> request_refresh_;
    
    // 로그 출력 콜백 (AppState에 로그 추가 후 UI 갱신)
    std::function<void(const std::string&)> log_sink_;
    
    // 사용자 명령어 처리기 (/join, /login 등)
    CommandProcessor commands_;
    
    // UI 레이아웃 빌더
    UiBuilder builder_;
    
    // 네트워크 메시지 라우터 (수신된 패킷을 적절한 핸들러로 연결)
    NetworkRouter router_;
    
    std::string host_;
    unsigned short port_;
    bool allow_env_override_{true};
};

Application::Impl::Impl(std::string host, unsigned short port, bool allow_env_override)
    : screen_(ftxui::ScreenInteractive::Fullscreen()),
      request_refresh_([this] { screen_.PostEvent(ftxui::Event::Custom); }),
      log_sink_([this](const std::string& message) {
          state_.append_log(message);
          // Debug logging
          std::ofstream debug_log("client_debug.log", std::ios::app);
          if (debug_log) {
              debug_log << message << std::endl;
          }
          request_refresh_();
      }),
      commands_(state_, net_, log_sink_),
      builder_(state_, commands_, net_, request_refresh_),
      router_(state_, net_, screen_, request_refresh_, log_sink_),
      host_(std::move(host)),
      port_(port),
      allow_env_override_(allow_env_override) {}

// -----------------------------------------------------------------------------
// 메인 실행 루프
// -----------------------------------------------------------------------------
int Application::Impl::Run() {
    LoadEnvironment(); // 환경 변수 로드
    state_.set_preview_room(state_.current_room());
    router_.Initialize(); // 네트워크 콜백 등록

    // UI 레이아웃 빌드
    auto ui = builder_.Build();
    
    // 키보드 이벤트 핸들러 등록
    auto app = CatchEvent(ui.root, [this, input = ui.input](ftxui::Event event) {
        // ESC 또는 Ctrl+C: 종료
        if (event == ftxui::Event::Escape || event == ftxui::Event::CtrlC) {
            if (state_.connected()) {
                net_.send_leave(state_.current_room());
            }
            screen_.ExitLoopClosure()();
            return true;
        }
        // F1: 도움말 토글
        if (event == ftxui::Event::F1) {
            state_.toggle_help();
            request_refresh_();
            return true;
        }
        // F5: 현재 방의 스냅샷을 다시 요청 (새로고침)
        if (event == ftxui::Event::F5) {
            if (state_.connected()) {
                net_.send_refresh(state_.current_room());
            } else {
                log_sink_("연결되어 있지 않습니다.");
            }
            return true;
        }
        // 좌우 화살표: 왼쪽 패널 너비 조절
        if (event == ftxui::Event::ArrowLeft) {
            state_.set_left_panel_width(state_.left_panel_width() - 1);
            request_refresh_();
            return true;
        }
        if (event == ftxui::Event::ArrowRight) {
            state_.set_left_panel_width(state_.left_panel_width() + 1);
            request_refresh_();
            return true;
        }
        // Enter 키: 명령어 실행 또는 채팅 전송
        if (event == ftxui::Event::Return && input->Focused()) {
            const std::string line = state_.input_buffer();
            state_.input_buffer().clear();
            if (!line.empty()) {
                // 명령어로 처리되지 않으면 일반 채팅으로 간주
                if (!commands_.Process(line)) {
                    if (state_.username() == AppState::kDefaultUser) {
                        log_sink_("[warn] 게스트는 채팅을 보낼 수 없습니다. /login 으로 로그인하세요.");
                    } else {
                        net_.send_chat(state_.current_room(), line);
                    }
                }
            }
            request_refresh_();
            return true;
        }
        return false;
    });

    AppendInitialLogs();
    Connect();

    // 입력창에 포커스 설정 후 UI 루프 시작
    ui.input->TakeFocus();
    screen_.Loop(app);

    net_.close();
    return 0;
}

void Application::Impl::AppendInitialLogs() {
    // UI 시작 시 기본 안내 메시지를 출력한다.
    log_sink_("[system] FTXUI 클라이언트를 시작했습니다.");
    log_sink_("[hint] 명령어: /login <name>, /join <room> [password], /whisper <user> <message>, /leave, /refresh");
    log_sink_("[hint] 방이 잠겨 있으면 비밀번호가 필요합니다.");
    log_sink_("[hint] 입력 후 Enter 키를 누르세요.");
}

void Application::Impl::Connect() {
    // 현재 host/port로 TCP 연결을 시도하고 실패 시 로그만 남긴다.
    if (net_.connect(host_, port_)) {
        state_.set_connected(true);
        log_sink_(std::string("연결됨: ") + host_ + ":" + std::to_string(port_));
        // 연결 즉시 기본 게스트 로그인 시도
        net_.send_login(state_.username(), "");
    } else {
        state_.set_connected(false);
        log_sink_("연결에 실패했습니다.");
    }
}

void Application::Impl::LoadEnvironment() {
    if (allow_env_override_) {
        if (const char* host_env = std::getenv(kEnvHost); host_env && *host_env) {
            host_ = host_env;
        }
        if (const char* port_env = std::getenv(kEnvPort); port_env && *port_env) {
            port_ = ParsePort(port_env, kDefaultPort);
        }
    }
}

unsigned short Application::Impl::ParsePort(const char* value, unsigned short fallback) {
    if (!value || !*value) {
        return fallback;
    }
    try {
        auto parsed = std::stoul(value);
        if (parsed > 0 && parsed <= 65535) {
            return static_cast<unsigned short>(parsed);
        }
    } catch (...) {
    }
    return fallback;
}

Application::Application(std::string host, unsigned short port, bool allow_env_override)
    : impl_(std::make_unique<Impl>(std::move(host), port, allow_env_override)) {}

Application::Application()
    : Application(kDefaultHost, kDefaultPort, true) {}

Application::~Application() = default;

int Application::Run() {
    return impl_->Run();
}

} // namespace client::app
