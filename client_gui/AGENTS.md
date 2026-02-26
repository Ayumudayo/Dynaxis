# client_gui

Windows-only ImGui developer client (connects to gateway/HAProxy).

## Entry Points
- `client_gui/src/main.cpp`: app entry.
- `client_gui/src/client_app.cpp`: state + command handling.
- `client_gui/src/net_client.cpp`: TCP client.
- `client_gui/src/gui_manager.cpp`: ImGui/GLFW rendering.
- `client_gui/include/client/*.hpp`: public headers.

## Build / Run (Windows)
```powershell
pwsh scripts/build.ps1 -ClientOnly -Target client_gui
.\build-windows-client\client_gui\Release\client_gui.exe
```

## Notes
- Uses system font `Malgun Gothic` for Korean IME support (see `client_gui/README.md`).
