if (BUILD_GTEST_TESTS)
    add_executable(server_state_tests
        ../state_instance_registry_tests.cpp
        ../gateway_session_directory_tests.cpp
    )
    target_link_libraries(server_state_tests PRIVATE
        GTest::gtest_main
        gateway_session_directory
        server_state_common
        server_state_redis
        server_state_consul
    )

    discover_labeled_gtests(server_state_tests "integration;shared-state;gateway")
endif()
