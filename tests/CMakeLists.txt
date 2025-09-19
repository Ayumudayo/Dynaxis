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

include(GoogleTest)
gtest_discover_tests(storage_basic_tests)

