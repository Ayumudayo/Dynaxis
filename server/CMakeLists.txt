add_executable(server_app
    src/main.cpp
    src/chat/chat_service.cpp
)
target_include_directories(server_app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(server_app PRIVATE server_core wire_proto)
