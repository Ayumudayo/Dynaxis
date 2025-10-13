add_executable(gateway_app
    src/gateway_app.cpp
    src/main.cpp
)

target_include_directories(gateway_app PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/gateway/include
)

target_link_libraries(gateway_app PRIVATE
    server_core
)
