add_subdirectory(runtime)

if (NOT BUILD_CORE_EXTENSIONS)
    if (BUILD_GTEST_TESTS)
        add_executable(core_runtime_tests
            ../task_scheduler_tests.cpp
            ../job_thread_manager_tests.cpp
            test_core_memory.cpp
            test_core_util.cpp
            test_core_metrics.cpp
            test_core_paths.cpp
            test_app_host_lifecycle.cpp
            test_engine_composition.cpp
            test_trace_context.cpp
            test_core_transport.cpp
            test_transport_router.cpp
        )
        target_link_libraries(core_runtime_tests PRIVATE server_core GTest::gtest_main)
        discover_labeled_gtests(core_runtime_tests "core-behavior;core-runtime")
    endif()
    return()
endif()

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

if (BUILD_GTEST_TESTS)
    if (TARGET server_storage_pg)
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

    endif()

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
        test_core_net.cpp
        test_core_transport.cpp
        test_transport_router.cpp
        test_gateway_resilience_controls.cpp
        test_trace_context.cpp
        test_security_compression.cpp
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

    if (TARGET storage_basic_tests)
        discover_labeled_gtests(storage_basic_tests "integration;db")
    endif()
    discover_labeled_gtests(core_concurrency_tests "core-behavior")
    discover_labeled_gtests(core_general_tests "core-behavior")
endif()
