if (BUILD_GTEST_TESTS)
    add_executable(chat_auth_tests test_session_token.cpp)
    target_include_directories(chat_auth_tests PRIVATE ${CMAKE_SOURCE_DIR}/gateway/include ${CMAKE_SOURCE_DIR}/gateway/src)
    target_link_libraries(chat_auth_tests PRIVATE chat_auth GTest::gtest_main nlohmann_json::nlohmann_json)
    discover_labeled_gtests(chat_auth_tests "server;authentication")
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
        test_server_routes.cpp
        ${CMAKE_SOURCE_DIR}/server/src/app/router.cpp
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

    discover_labeled_gtests(chat_history_tests "integration;chat-history;server")
    discover_labeled_gtests(server_general_tests "integration;server-behavior")
endif()
