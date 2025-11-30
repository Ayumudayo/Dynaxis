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

add_executable(chat_history_tests
    chat_history_tests.cpp
    ${CMAKE_SOURCE_DIR}/server/src/chat/chat_service_core.cpp
)
target_include_directories(chat_history_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/server/include
    ${CMAKE_SOURCE_DIR}/server
)
target_link_libraries(chat_history_tests PRIVATE
    GTest::gtest_main
    server_core
    server_storage_redis
    wire_proto
)

include(GoogleTest)
gtest_discover_tests(storage_basic_tests)
gtest_discover_tests(core_concurrency_tests)
gtest_discover_tests(server_state_tests)
gtest_discover_tests(chat_history_tests)

add_executable(core_general_tests
    core/test_core_memory.cpp
    core/test_core_util.cpp
    core/test_core_metrics.cpp
    core/test_core_paths.cpp
    core/test_core_net.cpp
)
target_include_directories(core_general_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/server/include
)
target_link_libraries(core_general_tests PRIVATE
    GTest::gtest_main
    server_core
)
gtest_discover_tests(core_general_tests)
