cmake_minimum_required(VERSION 3.20)

if (BUILD_GTEST_TESTS)
    find_package(GTest CONFIG REQUIRED)
    include(GoogleTest)

    set(GTEST_DISCOVERY_TIMEOUT_SEC 30)

    function(discover_labeled_gtests target labels)
        gtest_discover_tests(${target}
            DISCOVERY_TIMEOUT ${GTEST_DISCOVERY_TIMEOUT_SEC})
        # GoogleTest의 인수 직렬화에 list를 넣지 않고, 발견된 test list에 직접 붙인다.
        set(label_script "${CMAKE_CURRENT_BINARY_DIR}/${target}_labels.cmake")
        file(WRITE "${label_script}"
            "if(DEFINED ${target}_TESTS AND NOT \"\${${target}_TESTS}\" STREQUAL \"\")\n"
            "  set_tests_properties(\${${target}_TESTS} PROPERTIES LABELS \"${labels}\")\n"
            "endif()\n")
        set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${label_script}")
    endfunction()
endif()

add_subdirectory(core)
if (BUILD_CORE_EXTENSIONS)
    if (BUILD_GTEST_TESTS AND TARGET admin_http_client)
        add_subdirectory(admin)
    endif()
    if (TARGET gateway_app AND TARGET server_app)
        add_subdirectory(gateway)
    endif()
    if (TARGET server_app)
        add_subdirectory(server)
    endif()
    add_subdirectory(contracts)
    if (TARGET gateway_app AND TARGET server_app)
        add_subdirectory(policy)
    endif()
endif()

if (NOT BUILD_GTEST_TESTS AND NOT BUILD_CONTRACT_TESTS)
    message(STATUS "No test suites selected (BUILD_GTEST_TESTS=OFF, BUILD_CONTRACT_TESTS=OFF)")
endif()
