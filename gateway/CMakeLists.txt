add_executable(gateway_app
    src/gateway_connection.cpp
    src/gateway_app.cpp
    src/session_directory.cpp
    src/main.cpp
)

target_include_directories(gateway_app PRIVATE
    ${CMAKE_SOURCE_DIR}/gateway/include
    ${CMAKE_SOURCE_DIR}/server/include
)

if (KNIGHTS_ENABLE_GATEWAY_UDP_INGRESS)
    target_compile_definitions(gateway_app PRIVATE KNIGHTS_ENABLE_GATEWAY_UDP_INGRESS=1)
else()
    target_compile_definitions(gateway_app PRIVATE KNIGHTS_ENABLE_GATEWAY_UDP_INGRESS=0)
endif()

target_link_libraries(gateway_app PRIVATE
    server_core
    server_state
    server_storage_redis
)
