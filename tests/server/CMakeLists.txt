if (BUILD_GTEST_TESTS)
    add_executable(chat_history_tests
        ../chat_history_tests.cpp
    )
    target_include_directories(chat_history_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/server/src/chat
    )
    target_link_libraries(chat_history_tests PRIVATE
        GTest::gtest_main
        server_chat_runtime
        ${CMAKE_DL_LIBS}
    )

    add_executable(server_general_tests
        test_server_chat.cpp
        test_server_config.cpp
        test_chat_lua_bindings.cpp
        test_lua_hook_integration.cpp
        test_hook_auto_disable.cpp
    )
    target_include_directories(server_general_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/server/src/chat
    )
    target_link_libraries(server_general_tests PRIVATE
        GTest::gtest_main
        server_app_config
        server_chat_runtime
        server_core
        ${CMAKE_DL_LIBS}
    )

    add_executable(server_fps_tests
        test_fps_runtime.cpp
    )
    target_include_directories(server_fps_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/server/include
        ${CMAKE_BINARY_DIR}/proto
    )
    target_link_libraries(server_fps_tests PRIVATE
        GTest::gtest_main
        server_fps_runtime
        server_core
        wire_proto
    )

    if (TARGET chat_hook_sample AND TARGET chat_hook_sample_v2 AND TARGET chat_hook_tag)
        add_library(test_chat_hook_v2_only MODULE
            chat_hook_v2_only.cpp
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
            chat_hook_dual_entrypoint.cpp
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
            chat_hook_throwing.cpp
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
            test_chat_plugin_chain.cpp
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
            test_chat_plugin_chain_v2.cpp
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

    discover_labeled_gtests(chat_history_tests "integration;chat-history;server")
    discover_labeled_gtests(server_general_tests "integration;server-behavior;extensibility")
    discover_labeled_gtests(server_fps_tests "integration;server-behavior;fps")
    if (TARGET server_plugin_chain_tests)
        discover_labeled_gtests(server_plugin_chain_tests "integration;extensibility;server")
    endif()
    if (TARGET server_plugin_chain_v2_tests)
        discover_labeled_gtests(server_plugin_chain_v2_tests "integration;extensibility;server")
    endif()
endif()
