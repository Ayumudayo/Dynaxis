cmake_minimum_required(VERSION 3.20)

if (BUILD_GTEST_TESTS)
    find_package(GTest CONFIG REQUIRED)
    include(GoogleTest)

    set(GTEST_DISCOVERY_TIMEOUT_SEC 30)

    function(discover_labeled_gtests target labels)
        string(REPLACE ";" "\\;" escaped_labels "${labels}")
        gtest_discover_tests(${target}
            DISCOVERY_TIMEOUT ${GTEST_DISCOVERY_TIMEOUT_SEC}
            PROPERTIES LABELS "${escaped_labels}")
    endfunction()
endif()

add_subdirectory(core)
add_subdirectory(gateway)
add_subdirectory(server)
add_subdirectory(contracts)
add_subdirectory(policy)

if (NOT BUILD_GTEST_TESTS AND NOT BUILD_CONTRACT_TESTS)
    message(STATUS "No test suites selected (BUILD_GTEST_TESTS=OFF, BUILD_CONTRACT_TESTS=OFF)")
endif()
