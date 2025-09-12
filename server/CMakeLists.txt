add_executable(server_app
    src/main.cpp
    src/app/bootstrap.cpp
    src/app/router.cpp
    src/chat/chat_service_core.cpp
    src/chat/handlers_login.cpp
    src/chat/session_events.cpp
    src/chat/handlers_join.cpp
    src/chat/handlers_leave.cpp
    src/chat/handlers_chat.cpp
)
target_include_directories(server_app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(server_app PRIVATE server_core wire_proto)
# protobuf 생성 헤더 경로 추가
target_include_directories(server_app PRIVATE ${CMAKE_BINARY_DIR}/proto)

# vcpkg DLL 자동 복사
if(WIN32)
    add_custom_command(TARGET server_app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/$<$<CONFIG:Debug>:debug/>bin/libprotobuf$<$<CONFIG:Debug>:d>.dll"
        "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/$<$<CONFIG:Debug>:debug/>bin/libprotoc$<$<CONFIG:Debug>:d>.dll"
        "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/$<$<CONFIG:Debug>:debug/>bin/abseil_dll.dll"
        $<TARGET_FILE_DIR:server_app>)
endif()
