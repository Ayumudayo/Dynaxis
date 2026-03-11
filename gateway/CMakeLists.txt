add_executable(gateway_app
    src/gateway_connection.cpp
    src/gateway_app.cpp
    src/registry_backend_factory.cpp
    src/session_directory.cpp
    src/main.cpp
)

target_include_directories(gateway_app PRIVATE
    ${CMAKE_SOURCE_DIR}/gateway/include
)

target_link_libraries(gateway_app PRIVATE
    server_core
    server_state
    server_storage_redis
)

if (TARGET generate_game_opcodes)
    add_dependencies(gateway_app generate_game_opcodes)
endif()
