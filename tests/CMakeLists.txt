cmake_minimum_required(VERSION 3.20)

if (BUILD_GTEST_TESTS)
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
        ${CMAKE_SOURCE_DIR}/server/src/chat/chat_hook_plugin_manager.cpp
        ${CMAKE_SOURCE_DIR}/server/src/chat/chat_hook_plugin_chain.cpp
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

    # Linux dynamic loader symbols (dlopen/dlsym)
    target_link_libraries(chat_history_tests PRIVATE ${CMAKE_DL_LIBS})

    add_executable(core_general_tests
        core/test_core_memory.cpp
        core/test_core_util.cpp
        core/test_core_metrics.cpp
        core/test_core_paths.cpp
        core/test_core_net.cpp
        core/test_udp_bind_abuse_guard.cpp
        core/test_udp_sequenced_metrics.cpp
        core/test_security_compression.cpp
    )
    target_include_directories(core_general_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/server/include
        ${CMAKE_SOURCE_DIR}/gateway/include
    )
    target_link_libraries(core_general_tests PRIVATE
        GTest::gtest_main
        server_core
    )

    add_executable(server_general_tests
        server/test_server_chat.cpp
        ${CMAKE_SOURCE_DIR}/server/src/chat/chat_service_core.cpp
        ${CMAKE_SOURCE_DIR}/server/src/chat/chat_hook_plugin_manager.cpp
        ${CMAKE_SOURCE_DIR}/server/src/chat/chat_hook_plugin_chain.cpp
        ${CMAKE_SOURCE_DIR}/server/src/chat/handlers_login.cpp
        ${CMAKE_SOURCE_DIR}/server/src/chat/handlers_join.cpp
        ${CMAKE_SOURCE_DIR}/server/src/chat/handlers_leave.cpp
        ${CMAKE_SOURCE_DIR}/server/src/chat/handlers_chat.cpp
        ${CMAKE_SOURCE_DIR}/server/src/chat/handlers_ping.cpp
        ${CMAKE_SOURCE_DIR}/server/src/chat/session_events.cpp
    )
    target_include_directories(server_general_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/server/include
        ${CMAKE_SOURCE_DIR}/server
    )
    target_link_libraries(server_general_tests PRIVATE
        GTest::gtest_main
        server_core
        server_storage_redis
        wire_proto
    )

    # Linux dynamic loader symbols (dlopen/dlsym)
    target_link_libraries(server_general_tests PRIVATE ${CMAKE_DL_LIBS})

    include(GoogleTest)
    gtest_discover_tests(storage_basic_tests)
    gtest_discover_tests(core_concurrency_tests)
    gtest_discover_tests(server_state_tests)
    gtest_discover_tests(chat_history_tests)
    gtest_discover_tests(core_general_tests)
    gtest_discover_tests(server_general_tests)
endif()

if (BUILD_CONTRACT_TESTS)
    add_executable(core_public_api_smoke
        core/public_api_smoke.cpp
    )
    target_include_directories(core_public_api_smoke PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
    )
    target_link_libraries(core_public_api_smoke PRIVATE
        server_core
    )
    add_test(NAME CorePublicApiSmoke COMMAND core_public_api_smoke)

    add_executable(core_public_api_headers_compile
        core/public_api_headers_compile.cpp
    )
    target_include_directories(core_public_api_headers_compile PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
    )
    target_link_libraries(core_public_api_headers_compile PRIVATE
        server_core
    )
    add_test(NAME CorePublicApiHeadersCompile COMMAND core_public_api_headers_compile)

    add_executable(core_public_api_stable_header_scenarios
        core/public_api_stable_header_scenarios.cpp
    )
    target_include_directories(core_public_api_stable_header_scenarios PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
    )
    target_link_libraries(core_public_api_stable_header_scenarios PRIVATE
        server_core
    )
    add_test(NAME CorePublicApiStableHeaderScenarios COMMAND core_public_api_stable_header_scenarios)

    if (Python3_Interpreter_FOUND)
      add_test(
        NAME CoreApiBoundaryFixtures
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/check_core_api_contracts.py" --check-boundary-fixtures
      )
      add_test(
        NAME DoxygenCoverageToolTests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/test_check_doxygen_coverage.py"
      )
      add_test(
        NAME CoreApiStableGovernanceFixtures
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/check_core_api_contracts.py" --check-stable-governance-fixtures
      )
    else()
      message(WARNING "Python3 not found: CoreApiBoundaryFixtures test disabled")
    endif()
endif()

if (NOT BUILD_GTEST_TESTS AND NOT BUILD_CONTRACT_TESTS)
    message(STATUS "No test suites selected (BUILD_GTEST_TESTS=OFF, BUILD_CONTRACT_TESTS=OFF)")
endif()
