cmake_minimum_required(VERSION 3.20)

# FTXUI 기반 개발용 클라이언트
add_executable(dev_chat_cli
    src/main.cpp
    src/app/app_state.cpp
    src/app/application.cpp
    src/app/command_processor.cpp
    src/app/network_router.cpp
    src/app/ui_builder.cpp
    src/net_client.cpp
    src/ui/status_bar.cpp
)
target_include_directories(dev_chat_cli PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
target_include_directories(dev_chat_cli PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)
target_include_directories(dev_chat_cli PRIVATE ${CMAKE_SOURCE_DIR}/server/include)

# 서버 공용 헤더 재사용
target_link_libraries(dev_chat_cli PRIVATE wire_proto server_core)
target_include_directories(dev_chat_cli PRIVATE ${CMAKE_BINARY_DIR}/proto)

find_package(ftxui CONFIG REQUIRED)
target_link_libraries(dev_chat_cli PRIVATE ftxui::screen ftxui::dom ftxui::component)

if (SERVER_USE_HEADER_ONLY_BOOST_SYSTEM)
    target_compile_definitions(dev_chat_cli PUBLIC BOOST_ERROR_CODE_HEADER_ONLY)
    target_include_directories(dev_chat_cli PUBLIC ${Boost_INCLUDE_DIRS})
else()
    target_link_libraries(dev_chat_cli PRIVATE Boost::system)
endif()

if (WIN32)
    target_link_libraries(dev_chat_cli PRIVATE ws2_32 mswsock)
endif()

if (MSVC)
    target_compile_definitions(dev_chat_cli PUBLIC _WIN32_WINNT=0x0602)
    set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)
endif()

if (WIN32)
    add_custom_command(TARGET dev_chat_cli POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/$<$<CONFIG:Debug>:debug/>bin/libprotobuf$<$<CONFIG:Debug>:d>.dll"
        "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/$<$<CONFIG:Debug>:debug/>bin/libprotoc$<$<CONFIG:Debug>:d>.dll"
        "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/$<$<CONFIG:Debug>:debug/>bin/abseil_dll.dll"
        $<TARGET_FILE_DIR:dev_chat_cli>)
endif()
