cmake_minimum_required(VERSION 3.20)

project(infra_redis_factory_consumer LANGUAGES CXX)

find_package(infra_redis_factory CONFIG QUIET)
if (TARGET infra_redis_factory::infra_redis_factory)
    set(redis_factory_target infra_redis_factory::infra_redis_factory)
else()
    find_package(server_storage_redis_factory CONFIG REQUIRED)
    set(redis_factory_target server_storage_redis_factory::server_storage_redis_factory)
endif()

add_executable(infra_redis_factory_consumer
    main.cpp
)

set_target_properties(infra_redis_factory_consumer PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_link_libraries(infra_redis_factory_consumer PRIVATE
    ${redis_factory_target}
)
