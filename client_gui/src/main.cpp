#include "client/client_app.hpp"
#include "client/client_preferences.hpp"
#include "client/gui_manager.hpp"
#include "client/net_client.hpp"

#include <filesystem>

#if defined(_MSC_VER) && (_MSC_VER >= 1900) && !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
#pragma comment(lib, "legacy_stdio_definitions")
#endif

/**
 * @brief ImGui 개발용 클라이언트 진입점입니다.
 *
 * 비비밀 로컬 환경설정을 로드하고 CLI 인자(`--host`, `--port`, `--user`, `--workbench`,
 * `--messenger`, `--auto-resume`, `--no-auto-resume`, `--config`, `--smoke-test`)를 적용한 뒤
 * `NetClient`, `ClientApp`, `GuiManager`를 조립해 실행합니다.
 */
int main(int argc, char** argv)
{
    const ClientCliOptions cli = parse_client_cli_args(argc, argv);

    std::filesystem::path persist_path;
    if (!cli.config_path_override.empty()) {
        persist_path = cli.config_path_override;
    } else if (!cli.smoke_test) {
        persist_path = default_client_preferences_path();
    }

    ClientPreferences prefs;
    if (!persist_path.empty()) {
        load_client_preferences_file(persist_path, prefs);
    }
    apply_cli_overrides(prefs, cli);

    NetClient transport;
    ClientApp app(transport);
    GuiManager gui(app, prefs.workbench_mode ? UiMode::WORKBENCH : UiMode::MESSENGER);
    gui.apply_preferences(prefs, persist_path);

    if (!gui.init(1360, 820, "Dynaxis Client")) {
        return 1;
    }

    gui.run(cli.smoke_test ? 10u : 0u);
    return 0;
}
