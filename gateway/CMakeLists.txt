find_package(gRPC CONFIG REQUIRED)
add_executable(gateway_app
    src/gateway_connection.cpp
    src/gateway_app.cpp
    src/main.cpp
)

target_include_directories(gateway_app PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/gateway/include
    ${CMAKE_BINARY_DIR}/proto
)

target_link_libraries(gateway_app PRIVATE
    server_core
    gateway_lb_proto
    gRPC::grpc++
)
