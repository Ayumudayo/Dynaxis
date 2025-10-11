#include "client/app/application.hpp"

#include "client/app/app_state.hpp"
#include "client/app/command_processor.hpp"
#include "client/app/network_router.hpp"
#include "client/app/ui_builder.hpp"
#include "client/net_client.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <memory>
#include <string>
#include <utility>

namespace client::app {

namespace {
constexpr const char* kDefaultHost = "127.0.0.1";
constexpr unsigned short kDefaultPort = 5000;
} // namespace

class Application::Impl {
public:
    Impl();
    ~Impl() = default;

    int Run();

private:
    void AppendInitialLogs();
    void Connect();

    ftxui::ScreenInteractive screen_;
    AppState state_;
    ::NetClient net_;
    std::function<void()> request_refresh_;
    std::function<void(const std::string&)> log_sink_;
    CommandProcessor commands_;
    UiBuilder builder_;
    NetworkRouter router_;
};

Application::Impl::Impl()
    : screen_(ftxui::ScreenInteractive::Fullscreen()),
      request_refresh_([this] { screen_.PostEvent(ftxui::Event::Custom); }),
      log_sink_([this](const std::string& message) {
          state_.append_log(message);
          request_refresh_();
      }),
      commands_(state_, net_, log_sink_),
      builder_(state_, commands_, net_, request_refresh_),
      router_(state_, net_, screen_, request_refresh_, log_sink_) {}

int Application::Impl::Run() {
    state_.set_preview_room(state_.current_room());
    router_.Initialize();

    auto ui = builder_.Build();
    auto app = CatchEvent(ui.root, [this, input = ui.input](ftxui::Event event) {
        if (event == ftxui::Event::Escape || event == ftxui::Event::CtrlC) {
            screen_.ExitLoopClosure()();
            return true;
        }
        if (event == ftxui::Event::F1) {
            state_.toggle_help();
            request_refresh_();
            return true;
        }
        if (event == ftxui::Event::F5) {
            if (state_.connected()) {
                net_.send_refresh(state_.current_room());
            } else {
                log_sink_("연결되지 않았습니다.");
            }
            return true;
        }
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
        if (event == ftxui::Event::Return && input->Focused()) {
            const std::string line = state_.input_buffer();
            state_.input_buffer().clear();
            if (!line.empty()) {
                if (!commands_.Process(line)) {
                    if (state_.username() == AppState::kDefaultUser) {
                        log_sink_("[warn] 게스트는 채팅을 보낼 수 없습니다. /login 명령으로 로그인하세요.");
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

    ui.input->TakeFocus();
    screen_.Loop(app);

    net_.close();
    return 0;
}

void Application::Impl::AppendInitialLogs() {
    log_sink_("[system] FTXUI 클라이언트를 시작했습니다.");
    log_sink_("[hint] 명령어: /login <name>, /join <room> [password], /whisper <user> <message>, /leave, /refresh");
    log_sink_("[hint] 자물쇠가 표시된 방은 비밀번호가 필요합니다.");
    log_sink_("[hint] 입력 후 Enter 키로 전송합니다.");
}

void Application::Impl::Connect() {
    if (net_.connect(kDefaultHost, kDefaultPort)) {
        state_.set_connected(true);
        log_sink_(std::string("연결됨: ") + kDefaultHost + ":" + std::to_string(kDefaultPort));
        net_.send_login(state_.username(), "");
    } else {
        state_.set_connected(false);
        log_sink_("연결에 실패했습니다.");
    }
}

Application::Application()
    : impl_(std::make_unique<Impl>()) {}

Application::~Application() = default;

int Application::Run() {
    return impl_->Run();
}

} // namespace client::app

