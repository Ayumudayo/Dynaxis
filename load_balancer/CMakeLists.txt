add_executable(load_balancer_app
    src/load_balancer_app.cpp
    src/main.cpp
)

target_include_directories(load_balancer_app PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/load_balancer/include
    ${CMAKE_SOURCE_DIR}/server/include
)

target_link_libraries(load_balancer_app PRIVATE
    server_core
    server_state
)
