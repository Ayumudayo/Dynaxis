add_executable(protocol_fuzz_harness
    ../core/protocol_fuzz_harness.cpp
)
target_include_directories(protocol_fuzz_harness PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
)
target_link_libraries(protocol_fuzz_harness PRIVATE
    server_core
)
add_test(NAME ProtocolFuzzHarness COMMAND protocol_fuzz_harness)
set_tests_properties(ProtocolFuzzHarness PROPERTIES LABELS "fuzz")

if (BUILD_GTEST_TESTS OR BUILD_CONTRACT_TESTS)
    add_library(test_plugin_runtime_v1 MODULE
        plugin_runtime_v1.cpp
    )
    target_include_directories(test_plugin_runtime_v1 PRIVATE
        ${CMAKE_SOURCE_DIR}/tests/core
    )
    set_target_properties(test_plugin_runtime_v1 PROPERTIES
        PREFIX ""
        OUTPUT_NAME "test_plugin_runtime_v1"
    )

    add_library(test_plugin_runtime_v2 MODULE
        plugin_runtime_v2.cpp
    )
    target_include_directories(test_plugin_runtime_v2 PRIVATE
        ${CMAKE_SOURCE_DIR}/tests/core
    )
    set_target_properties(test_plugin_runtime_v2 PROPERTIES
        PREFIX ""
        OUTPUT_NAME "test_plugin_runtime_v2"
    )

    add_library(test_plugin_runtime_invalid MODULE
        plugin_runtime_invalid.cpp
    )
    target_include_directories(test_plugin_runtime_invalid PRIVATE
        ${CMAKE_SOURCE_DIR}/tests/core
    )
    set_target_properties(test_plugin_runtime_invalid PROPERTIES
        PREFIX ""
        OUTPUT_NAME "test_plugin_runtime_invalid"
    )
endif()

if (BUILD_GTEST_TESTS)
    add_executable(storage_basic_tests
        ../test_storage_basic.cpp
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
        ../task_scheduler_tests.cpp
        ../db_worker_pool_tests.cpp
        ../job_thread_manager_tests.cpp
    )
    target_include_directories(core_concurrency_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
    )
    target_link_libraries(core_concurrency_tests PRIVATE
        GTest::gtest_main
        server_core
    )

    add_executable(core_general_tests
        test_core_memory.cpp
        test_core_util.cpp
        test_core_metrics.cpp
        test_core_paths.cpp
        test_app_host_lifecycle.cpp
        test_engine_composition.cpp
        test_mmorpg_migration.cpp
        test_mmorpg_world_drain.cpp
        test_mmorpg_topology.cpp
        test_worlds_kubernetes.cpp
        test_worlds_aws.cpp
        test_mmorpg_world_transfer.cpp
        test_direct_bind_contract.cpp
        test_core_net.cpp
        test_gateway_resilience_controls.cpp
        test_trace_context.cpp
        test_udp_bind_abuse_guard.cpp
        test_udp_sequenced_metrics.cpp
        test_rudp_ack_window.cpp
        test_rudp_retransmit.cpp
        test_rudp_handshake.cpp
        test_rudp_flow_control.cpp
        test_rudp_fallback.cpp
        test_rudp_engine.cpp
        test_rudp_rollout_policy.cpp
        test_security_compression.cpp
        test_admin_command_auth.cpp
        test_direct_egress_route.cpp
    )
    target_include_directories(core_general_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/gateway/include
        ${CMAKE_SOURCE_DIR}/gateway/src
    )
    target_link_libraries(core_general_tests PRIVATE
        GTest::gtest_main
        server_core
    )

    add_executable(core_plugin_runtime_tests
        test_shared_library.cpp
        test_plugin_host.cpp
        test_plugin_chain_host.cpp
        test_script_watcher.cpp
        test_lua_runtime.cpp
        test_lua_sandbox.cpp
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

    discover_labeled_gtests(storage_basic_tests "integration;db")
    discover_labeled_gtests(core_concurrency_tests "core-behavior")
    discover_labeled_gtests(core_general_tests "core-behavior")
    discover_labeled_gtests(core_plugin_runtime_tests "core-behavior;extensibility")
endif()
