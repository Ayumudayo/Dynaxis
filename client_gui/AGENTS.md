# client_gui

Cross-platform ImGui/GLFW/OpenGL client with default Messenger mode and optional Developer Workbench mode (connects to gateway/HAProxy).

## Entry Points
- `client_gui/src/main.cpp`: app entry (CLI overrides `--host`, `--port`, `--user`, `--workbench`, `--messenger`, `--auto-resume`, `--no-auto-resume`, `--config`, `--smoke-test`).
- `client_gui/src/client_app.cpp`: UI-independent state, timeline, blacklist sync, `Auto-Resume` backoff, RTT stats, and command/protocol-event handling.
- `client_gui/src/net_client.cpp`: `IClientTransport` TCP/Protobuf/RTT implementation.
- `client_gui/src/gui_manager.cpp`: ImGui/GLFW rendering (Messenger default + `F12` Workbench toggle + Win32 box-less IME hook).
- `client_gui/include/client/*.hpp`: public headers (`client_transport.hpp`, `client_preferences.hpp`, `message_timeline.hpp`, `client_app.hpp`, `net_client.hpp`, `gui_manager.hpp`).

## Build / Run
```powershell
python scripts/build.py --profile client --config Release --step all
.\build\windows\client\release\client_gui\Release\client_gui.exe
.\build\windows\client\release\client_gui\Release\client_gui.exe --workbench
```

## Notes
- Follow `docs/agent-guides/frontend-design-system.md` (Slate Dark tokens, Discord-density timeline spacing, outer `NoScrollbar`, `tabular-nums`, character-unit counter, secret non-persistence) and `docs/architecture/client-workbench-design.md`.
- Same UI source on Windows/Linux. Font override: `DYNAXIS_FONT_PATH`; defaults use Segoe UI + Malgun Gothic / Noto Sans CJK / Apple SD Gothic Neo.
- `--smoke-test` renders ten frames and exits without modifying default user preferences; Linux CI uses Xvfb/software OpenGL.
