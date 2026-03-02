cmake_minimum_required(VERSION 3.20)

# ImGui Client
project(client_gui)

# ---------------------------------------------------------------------------
# ImGui Submodule Configuration
# ---------------------------------------------------------------------------
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/external/imgui)

# ImGui Sources (Core + Backends)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

# ---------------------------------------------------------------------------
# Executable Definition
# ---------------------------------------------------------------------------
add_executable(client_gui WIN32
    src/main.cpp
    src/net_client.cpp
    src/client_app.cpp
    src/gui_manager.cpp
    ${IMGUI_SOURCES}
)

if (MSVC)
    # GUI 앱에서 main()을 진입점으로 사용하기 위한 링커 옵션
    target_link_options(client_gui PRIVATE "/ENTRY:mainCRTStartup")
endif()

# ---------------------------------------------------------------------------
# Include Directories
# ---------------------------------------------------------------------------
target_include_directories(client_gui PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${CMAKE_SOURCE_DIR}/server/include # for server_core headers
)

# ---------------------------------------------------------------------------
# Dependencies & Linking
# ---------------------------------------------------------------------------
find_package(glfw3 CONFIG REQUIRED)
find_package(OpenGL REQUIRED)
find_package(OpenSSL REQUIRED)

# Link Libraries
set(CLIENT_GUI_LINK_LIBS
    glfw
    OpenGL::GL
    wire_proto
    server_core
    OpenSSL::SSL
    OpenSSL::Crypto
)

if (NOT SERVER_USE_HEADER_ONLY_BOOST_SYSTEM)
    list(APPEND CLIENT_GUI_LINK_LIBS Boost::system)
endif()

target_link_libraries(client_gui PRIVATE ${CLIENT_GUI_LINK_LIBS})

# Windows Specifics
if (WIN32)
    target_link_libraries(client_gui PRIVATE ws2_32 mswsock imm32) # imm32 is needed for ImGui IME support
    target_compile_definitions(client_gui PRIVATE UNICODE _UNICODE)
endif()

# ---------------------------------------------------------------------------
# Post-Build (Copy DLLs if needed)
# ---------------------------------------------------------------------------
if (WIN32)
    # Copy runtime DLLs resolved by CMake target metadata.
    add_custom_command(TARGET client_gui POST_BUILD
        COMMAND ${CMAKE_COMMAND}
        -Druntime_dlls=$<TARGET_RUNTIME_DLLS:client_gui>
        -Dtarget_dir=$<TARGET_FILE_DIR:client_gui>
        -P ${CMAKE_SOURCE_DIR}/cmake/copy_runtime_dlls_if_any.cmake
        COMMAND_EXPAND_LISTS)
endif()
