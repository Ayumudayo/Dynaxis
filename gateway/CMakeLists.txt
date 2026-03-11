add_library(gateway_session_directory STATIC
    src/session_directory.cpp
)
target_include_directories(gateway_session_directory PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(gateway_session_directory PUBLIC server_core)

add_library(gateway_backends STATIC
    src/registry_backend_factory.cpp
)
target_include_directories(gateway_backends PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(gateway_backends PUBLIC server_core)
target_link_libraries(gateway_backends PRIVATE server_state_redis)

add_executable(gateway_app
    src/gateway_connection.cpp
    src/gateway_app.cpp
    src/main.cpp
)

target_include_directories(gateway_app PRIVATE
    ${CMAKE_SOURCE_DIR}/gateway/include
)

target_link_libraries(gateway_app PRIVATE
    gateway_backends
    gateway_session_directory
    server_core
)

if (TARGET generate_game_opcodes)
    add_dependencies(gateway_app generate_game_opcodes)
endif()
