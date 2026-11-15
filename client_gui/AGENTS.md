# client_gui

Cross-platform ImGui/GLFW/OpenGL developer client (connects to gateway/HAProxy).

## Entry Points
- `client_gui/src/main.cpp`: app entry.
- `client_gui/src/client_app.cpp`: state + command handling.
- `client_gui/src/net_client.cpp`: TCP client.
- `client_gui/src/gui_manager.cpp`: ImGui/GLFW rendering.
- `client_gui/include/client/*.hpp`: public headers.

## Build / Run
```powershell
python scripts/build.py --profile client --config Release --step all
.\build\windows\client\release\client_gui\Release\client_gui.exe
```

## Notes
- Same UI source on Windows/Linux. Font override: DYNAXIS_FONT_PATH; defaults use Malgun Gothic/Noto Sans CJK/Apple SD Gothic Neo.
- --smoke-test renders ten frames and exits; Linux CI uses Xvfb/software OpenGL.
