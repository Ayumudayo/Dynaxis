if (BUILD_GTEST_TESTS)
    if (TARGET server_app)
        add_test(NAME ServerStartupFailure COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/python/verify_server_startup_failure.py $<TARGET_FILE:server_app>)
        set_tests_properties(ServerStartupFailure PROPERTIES TIMEOUT 15 LABELS "server-behavior;lifecycle")
    endif()
    add_executable(chat_timeline_tests
        ../client/test_message_timeline.cpp
        ${CMAKE_SOURCE_DIR}/client_gui/src/client_app.cpp
    )
    target_include_directories(chat_timeline_tests PRIVATE ${CMAKE_SOURCE_DIR}/client_gui/include)
    target_link_libraries(chat_timeline_tests PRIVATE GTest::gtest_main)
    discover_labeled_gtests(chat_timeline_tests "client;chat-history")
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
        nlohmann_json::nlohmann_json
        ${CMAKE_DL_LIBS}
    )

    discover_labeled_gtests(chat_history_tests "integration;chat-history;server")
    discover_labeled_gtests(server_general_tests "integration;server-behavior")
endif()
