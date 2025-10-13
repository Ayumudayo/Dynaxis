cmake_minimum_required(VERSION 3.20)

find_package(GTest CONFIG REQUIRED)

add_executable(storage_basic_tests
    test_storage_basic.cpp
)
target_include_directories(storage_basic_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/server/include
)
target_link_libraries(storage_basic_tests PRIVATE
    GTest::gtest_main
    server_core
    server_storage_pg
)

add_executable(core_concurrency_tests
    task_scheduler_tests.cpp
    db_worker_pool_tests.cpp
    job_thread_manager_tests.cpp
)
target_include_directories(core_concurrency_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/server/include
)
target_link_libraries(core_concurrency_tests PRIVATE
    GTest::gtest_main
    server_core
)

add_executable(server_state_tests
    state_instance_registry_tests.cpp
)
target_include_directories(server_state_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/server/include
)
target_link_libraries(server_state_tests PRIVATE
    GTest::gtest_main
    server_state
)

include(GoogleTest)
gtest_discover_tests(storage_basic_tests)
gtest_discover_tests(core_concurrency_tests)
gtest_discover_tests(server_state_tests)
