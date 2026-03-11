cmake_minimum_required(VERSION 3.20)

project(server_storage_pg_factory_consumer LANGUAGES CXX)

find_package(server_storage_pg_factory CONFIG REQUIRED)

add_executable(server_storage_pg_factory_consumer
    main.cpp
)

set_target_properties(server_storage_pg_factory_consumer PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_link_libraries(server_storage_pg_factory_consumer PRIVATE
    server_storage_pg_factory::server_storage_pg_factory
)
