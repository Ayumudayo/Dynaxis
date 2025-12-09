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
    src/main_entry.cpp
    src/net_client.cpp
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
target_link_libraries(client_gui PRIVATE 
    glfw
    OpenGL::GL
    wire_proto 
    server_core
    Boost::system
    OpenSSL::SSL 
    OpenSSL::Crypto
)

# Windows Specifics
if (WIN32)
    target_link_libraries(client_gui PRIVATE ws2_32 mswsock imm32) # imm32 is needed for ImGui IME support
    target_compile_definitions(client_gui PRIVATE UNICODE _UNICODE)
endif()

# ---------------------------------------------------------------------------
# Post-Build (Copy DLLs if needed)
# ---------------------------------------------------------------------------
if (WIN32)
    # Copy Protobuf/Abseil/OpenSSL/Zlib/Boost DLLs
    add_custom_command(TARGET client_gui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/$<$<CONFIG:Debug>:debug/>bin/libprotobuf$<$<CONFIG:Debug>:d>.dll"
        "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/$<$<CONFIG:Debug>:debug/>bin/libprotoc$<$<CONFIG:Debug>:d>.dll"
        "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/$<$<CONFIG:Debug>:debug/>bin/abseil_dll.dll"
        "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/$<$<CONFIG:Debug>:debug/>bin/libssl-3-x64.dll"
        "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/$<$<CONFIG:Debug>:debug/>bin/libcrypto-3-x64.dll"
        "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/$<$<CONFIG:Debug>:debug/>bin/zlib$<$<CONFIG:Debug>:d>1.dll"
        $<TARGET_FILE_DIR:client_gui>)
endif()
