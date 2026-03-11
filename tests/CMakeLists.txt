cmake_minimum_required(VERSION 3.20)

add_executable(protocol_fuzz_harness
    core/protocol_fuzz_harness.cpp
)
target_include_directories(protocol_fuzz_harness PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
)
target_link_libraries(protocol_fuzz_harness PRIVATE
    server_core
)
add_test(NAME ProtocolFuzzHarness COMMAND protocol_fuzz_harness)
set_tests_properties(ProtocolFuzzHarness PROPERTIES LABELS "fuzz")

if (BUILD_GTEST_TESTS)
    find_package(GTest CONFIG REQUIRED)

    set(GTEST_DISCOVERY_TIMEOUT_SEC 30)

    function(discover_labeled_gtests target labels)
        string(REPLACE ";" "\\;" escaped_labels "${labels}")
        gtest_discover_tests(${target}
            DISCOVERY_TIMEOUT ${GTEST_DISCOVERY_TIMEOUT_SEC}
            PROPERTIES LABELS "${escaped_labels}")
    endfunction()

    add_executable(storage_basic_tests
        test_storage_basic.cpp
    )
    target_include_directories(storage_basic_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
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
    )
    target_link_libraries(core_concurrency_tests PRIVATE
        GTest::gtest_main
        server_core
    )

    add_executable(server_state_tests
        state_instance_registry_tests.cpp
        gateway_session_directory_tests.cpp
        ${CMAKE_SOURCE_DIR}/gateway/src/session_directory.cpp
    )
    target_include_directories(server_state_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/gateway/include
    )
    target_link_libraries(server_state_tests PRIVATE
        GTest::gtest_main
        server_state
    )

    add_executable(chat_history_tests
        chat_history_tests.cpp
    )
    target_link_libraries(chat_history_tests PRIVATE
        GTest::gtest_main
        server_chat_runtime
    )

    # Linux dynamic loader symbols (dlopen/dlsym)
    target_link_libraries(chat_history_tests PRIVATE ${CMAKE_DL_LIBS})

    add_executable(core_general_tests
        core/test_core_memory.cpp
        core/test_core_util.cpp
        core/test_core_metrics.cpp
        core/test_core_paths.cpp
        core/test_app_host_lifecycle.cpp
        core/test_core_net.cpp
        core/test_gateway_resilience_controls.cpp
        core/test_trace_context.cpp
        core/test_udp_bind_abuse_guard.cpp
        core/test_udp_sequenced_metrics.cpp
        core/test_rudp_ack_window.cpp
        core/test_rudp_retransmit.cpp
        core/test_rudp_handshake.cpp
        core/test_rudp_flow_control.cpp
        core/test_rudp_fallback.cpp
        core/test_rudp_engine.cpp
        core/test_rudp_rollout_policy.cpp
        core/test_security_compression.cpp
        core/test_admin_command_auth.cpp
    )
    target_include_directories(core_general_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/gateway/include
    )
    target_link_libraries(core_general_tests PRIVATE
        GTest::gtest_main
        server_core
    )

    add_library(test_plugin_runtime_v1 MODULE
        core/plugin_runtime_v1.cpp
    )
    target_include_directories(test_plugin_runtime_v1 PRIVATE
        ${CMAKE_SOURCE_DIR}/tests/core
    )
    set_target_properties(test_plugin_runtime_v1 PROPERTIES
        PREFIX ""
        OUTPUT_NAME "test_plugin_runtime_v1"
    )

    add_library(test_plugin_runtime_v2 MODULE
        core/plugin_runtime_v2.cpp
    )
    target_include_directories(test_plugin_runtime_v2 PRIVATE
        ${CMAKE_SOURCE_DIR}/tests/core
    )
    set_target_properties(test_plugin_runtime_v2 PROPERTIES
        PREFIX ""
        OUTPUT_NAME "test_plugin_runtime_v2"
    )

    add_library(test_plugin_runtime_invalid MODULE
        core/plugin_runtime_invalid.cpp
    )
    target_include_directories(test_plugin_runtime_invalid PRIVATE
        ${CMAKE_SOURCE_DIR}/tests/core
    )
    set_target_properties(test_plugin_runtime_invalid PROPERTIES
        PREFIX ""
        OUTPUT_NAME "test_plugin_runtime_invalid"
    )

    add_executable(core_plugin_runtime_tests
        core/test_shared_library.cpp
        core/test_plugin_host.cpp
        core/test_plugin_chain_host.cpp
        core/test_script_watcher.cpp
        core/test_lua_runtime.cpp
        core/test_lua_sandbox.cpp
    )
    target_include_directories(core_plugin_runtime_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/tests/core
    )
    target_compile_definitions(core_plugin_runtime_tests PRIVATE
        TEST_PLUGIN_RUNTIME_V1_FILE="$<TARGET_FILE_NAME:test_plugin_runtime_v1>"
        TEST_PLUGIN_RUNTIME_V2_FILE="$<TARGET_FILE_NAME:test_plugin_runtime_v2>"
        TEST_PLUGIN_RUNTIME_INVALID_FILE="$<TARGET_FILE_NAME:test_plugin_runtime_invalid>"
    )
    target_link_libraries(core_plugin_runtime_tests PRIVATE
        GTest::gtest_main
        server_core
        ${CMAKE_DL_LIBS}
    )
    add_dependencies(core_plugin_runtime_tests
        test_plugin_runtime_v1
        test_plugin_runtime_v2
        test_plugin_runtime_invalid
    )

    add_executable(server_general_tests
        server/test_server_chat.cpp
        server/test_server_config.cpp
        server/test_chat_lua_bindings.cpp
        server/test_lua_hook_integration.cpp
        server/test_hook_auto_disable.cpp
    )
    target_link_libraries(server_general_tests PRIVATE
        GTest::gtest_main
        server_app_config
        server_chat_runtime
        server_core
    )
    if (TARGET generate_game_opcodes)
        add_dependencies(server_general_tests generate_game_opcodes)
    endif()

    # Linux dynamic loader symbols (dlopen/dlsym)
    target_link_libraries(server_general_tests PRIVATE ${CMAKE_DL_LIBS})

    if (TARGET chat_hook_sample AND TARGET chat_hook_sample_v2 AND TARGET chat_hook_tag)
        add_library(test_chat_hook_v2_only MODULE
            server/chat_hook_v2_only.cpp
        )
        target_include_directories(test_chat_hook_v2_only PRIVATE
            ${CMAKE_SOURCE_DIR}/server/include
        )
        target_compile_definitions(test_chat_hook_v2_only PRIVATE
            CHAT_HOOK_PLUGIN_BUILD
        )
        set_target_properties(test_chat_hook_v2_only PROPERTIES
            PREFIX ""
            OUTPUT_NAME "10_chat_hook_v2_only"
        )

        target_compile_definitions(server_general_tests PRIVATE
            TEST_CHAT_HOOK_V2_ONLY_PATH="$<TARGET_FILE:test_chat_hook_v2_only>"
            TEST_CHAT_HOOK_THROWING_PATH="$<TARGET_FILE:test_chat_hook_throwing>"
        )
        add_dependencies(server_general_tests
            test_chat_hook_v2_only
            test_chat_hook_throwing
        )

        add_library(test_chat_hook_dual_entrypoint MODULE
            server/chat_hook_dual_entrypoint.cpp
        )
        target_include_directories(test_chat_hook_dual_entrypoint PRIVATE
            ${CMAKE_SOURCE_DIR}/server/include
        )
        target_compile_definitions(test_chat_hook_dual_entrypoint PRIVATE
            CHAT_HOOK_PLUGIN_BUILD
        )
        set_target_properties(test_chat_hook_dual_entrypoint PROPERTIES
            PREFIX ""
            OUTPUT_NAME "10_chat_hook_dual_entrypoint"
        )

        add_library(test_chat_hook_throwing MODULE
            server/chat_hook_throwing.cpp
        )
        target_include_directories(test_chat_hook_throwing PRIVATE
            ${CMAKE_SOURCE_DIR}/server/include
        )
        target_compile_definitions(test_chat_hook_throwing PRIVATE
            CHAT_HOOK_PLUGIN_BUILD
        )
        set_target_properties(test_chat_hook_throwing PROPERTIES
            PREFIX ""
            OUTPUT_NAME "10_chat_hook_throwing"
        )

        add_executable(server_plugin_chain_tests
            server/test_chat_plugin_chain.cpp
        )
        target_compile_definitions(server_plugin_chain_tests PRIVATE
            TEST_CHAT_HOOK_SAMPLE_PATH="$<TARGET_FILE:chat_hook_sample>"
            TEST_CHAT_HOOK_SAMPLE_V2_PATH="$<TARGET_FILE:chat_hook_sample_v2>"
            TEST_CHAT_HOOK_TAG_PATH="$<TARGET_FILE:chat_hook_tag>"
        )
        target_link_libraries(server_plugin_chain_tests PRIVATE
            GTest::gtest_main
            server_chat_runtime
            server_core
            ${CMAKE_DL_LIBS}
        )
        add_dependencies(server_plugin_chain_tests
            chat_hook_sample
            chat_hook_sample_v2
            chat_hook_tag
        )

        add_executable(server_plugin_chain_v2_tests
            server/test_chat_plugin_chain_v2.cpp
        )
        target_compile_definitions(server_plugin_chain_v2_tests PRIVATE
            TEST_CHAT_HOOK_SAMPLE_V2_PATH="$<TARGET_FILE:chat_hook_sample_v2>"
            TEST_CHAT_HOOK_TAG_PATH="$<TARGET_FILE:chat_hook_tag>"
            TEST_CHAT_HOOK_V2_ONLY_PATH="$<TARGET_FILE:test_chat_hook_v2_only>"
            TEST_CHAT_HOOK_DUAL_PATH="$<TARGET_FILE:test_chat_hook_dual_entrypoint>"
            TEST_CHAT_HOOK_THROWING_PATH="$<TARGET_FILE:test_chat_hook_throwing>"
        )
        target_link_libraries(server_plugin_chain_v2_tests PRIVATE
            GTest::gtest_main
            server_chat_runtime
            server_core
            ${CMAKE_DL_LIBS}
        )
        add_dependencies(server_plugin_chain_v2_tests
            chat_hook_sample_v2
            chat_hook_tag
            test_chat_hook_v2_only
            test_chat_hook_dual_entrypoint
            test_chat_hook_throwing
        )
    endif()

    include(GoogleTest)
    discover_labeled_gtests(storage_basic_tests "integration;db")
    discover_labeled_gtests(core_concurrency_tests "core-behavior")
    discover_labeled_gtests(server_state_tests "integration;shared-state")
    discover_labeled_gtests(chat_history_tests "integration;chat-history")
    discover_labeled_gtests(core_general_tests "core-behavior")
    discover_labeled_gtests(core_plugin_runtime_tests "core-behavior;extensibility")
    discover_labeled_gtests(server_general_tests "integration;server-behavior;extensibility")
    if (TARGET server_plugin_chain_tests)
        discover_labeled_gtests(server_plugin_chain_tests "integration;extensibility")
    endif()
    if (TARGET server_plugin_chain_v2_tests)
        discover_labeled_gtests(server_plugin_chain_v2_tests "integration;extensibility")
    endif()
endif()

if (BUILD_CONTRACT_TESTS)
    if (NOT CMAKE_INSTALL_LIBDIR)
        set(CMAKE_INSTALL_LIBDIR "lib")
    endif()

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
    set_tests_properties(CorePublicApiSmoke PROPERTIES LABELS "contract;public-api;smoke")

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
    set_tests_properties(CorePublicApiHeadersCompile PROPERTIES LABELS "contract;public-api;compile")

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
    set_tests_properties(CorePublicApiStableHeaderScenarios PROPERTIES LABELS "contract;public-api;smoke")

    add_test(
      NAME CoreInstalledPackageConsumer
      COMMAND "${CMAKE_COMMAND}"
              "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
              "-DCONSUMER_SOURCE_DIR=${CMAKE_SOURCE_DIR}/tests/package/installed_consumer"
              "-DCONSUMER_BUILD_DIR=${CMAKE_BINARY_DIR}/contract/installed-consumer/build"
              "-DINSTALL_PREFIX=${CMAKE_BINARY_DIR}/contract/installed-consumer/prefix"
              "-DSERVER_CORE_PACKAGE_DIR=${CMAKE_BINARY_DIR}/contract/installed-consumer/prefix/${CMAKE_INSTALL_LIBDIR}/cmake/server_core"
              "-DTOOLCHAIN_FILE=${CMAKE_BINARY_DIR}/conan/build/generators/conan_toolchain.cmake"
              "-DCONFIG=$<CONFIG>"
              "-DGENERATOR=${CMAKE_GENERATOR}"
              "-DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
              "-DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
              -P "${CMAKE_SOURCE_DIR}/tests/core/run_installed_package_consumer.cmake"
    )
    set_tests_properties(CoreInstalledPackageConsumer PROPERTIES LABELS "contract;public-api;installed-consumer")

    if (Python3_Interpreter_FOUND)
      add_test(
        NAME CoreApiBoundaryFixtures
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/check_core_api_contracts.py" --check-boundary-fixtures
      )
      set_tests_properties(CoreApiBoundaryFixtures PROPERTIES LABELS "contract;docs-policy")
      add_test(
        NAME DoxygenCoverageToolTests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/test_check_doxygen_coverage.py"
      )
      set_tests_properties(DoxygenCoverageToolTests PROPERTIES LABELS "contract;docs-policy")
      add_test(
        NAME CoreApiStableGovernanceFixtures
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/check_core_api_contracts.py" --check-stable-governance-fixtures
      )
      set_tests_properties(CoreApiStableGovernanceFixtures PROPERTIES LABELS "contract;docs-policy")

    else()
      message(WARNING "Python3 not found: CoreApiBoundaryFixtures test disabled")
    endif()
endif()

if (Python3_Interpreter_FOUND)
    add_test(
      NAME StackPythonVerifyPluginMetrics
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/run_stack_test.py"
              "${CMAKE_SOURCE_DIR}/tests/python/verify_plugin_hot_reload.py" --check-only
    )
    add_test(
      NAME StackPythonVerifyRuntimeTogglesOn
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/run_stack_test.py"
              "${CMAKE_SOURCE_DIR}/tests/python/verify_runtime_toggle_metrics.py"
              --expect-chat-hook-enabled 1
              --expect-lua-enabled 1
    )
    add_test(
      NAME StackPythonHotReloadPluginV2
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/run_stack_test.py"
              "${CMAKE_SOURCE_DIR}/tests/python/verify_plugin_hot_reload.py"
    )
    add_test(
      NAME StackPythonVerifyPluginV2Fallback
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/run_stack_test.py"
              "${CMAKE_SOURCE_DIR}/tests/python/verify_plugin_v2_fallback.py"
    )
    add_test(
      NAME StackPythonRollbackPluginV1
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/run_stack_test.py"
              "${CMAKE_SOURCE_DIR}/tests/python/verify_plugin_rollback.py"
    )
    add_test(
      NAME StackPythonHotReloadLuaScript
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/run_stack_test.py"
              "${CMAKE_SOURCE_DIR}/tests/python/verify_script_hot_reload.py"
    )
    add_test(
      NAME StackPythonVerifyScriptFallbackSwitch
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/run_stack_test.py"
              "${CMAKE_SOURCE_DIR}/tests/python/verify_script_fallback_switch.py"
    )
    add_test(
      NAME StackPythonChatHookBehavior
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/run_stack_test.py"
              "${CMAKE_SOURCE_DIR}/tests/python/verify_chat_hook_behavior.py"
    )

    set_tests_properties(
      StackPythonVerifyPluginMetrics
      StackPythonVerifyRuntimeTogglesOn
      StackPythonHotReloadPluginV2
      StackPythonVerifyPluginV2Fallback
      StackPythonRollbackPluginV1
      StackPythonHotReloadLuaScript
      StackPythonVerifyScriptFallbackSwitch
      StackPythonChatHookBehavior
      PROPERTIES
        LABELS "python;stack;plugin-script"
        SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE
    )
endif()

if (NOT BUILD_GTEST_TESTS AND NOT BUILD_CONTRACT_TESTS)
    message(STATUS "No test suites selected (BUILD_GTEST_TESTS=OFF, BUILD_CONTRACT_TESTS=OFF)")
endif()
