cmake_minimum_required(VERSION 3.20)

project(server_core_installed_consumer LANGUAGES CXX)

find_package(server_core CONFIG REQUIRED)

add_executable(server_core_installed_consumer
    main.cpp
)

add_executable(server_core_extensibility_consumer
    extensibility_main.cpp
)

set_target_properties(server_core_installed_consumer PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

set_target_properties(server_core_extensibility_consumer PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_link_libraries(server_core_installed_consumer PRIVATE
    server_core::server_core
)

target_link_libraries(server_core_extensibility_consumer PRIVATE
    server_core::server_core
)
