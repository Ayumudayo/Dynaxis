if (BUILD_GTEST_TESTS)
    if (NOT TARGET nlohmann_json::nlohmann_json)
        find_package(nlohmann_json CONFIG REQUIRED)
    endif()

    add_executable(admin_console_tests
        test_admin_console_v1.cpp
    )

    target_compile_features(admin_console_tests PRIVATE cxx_std_20)
    set_target_properties(admin_console_tests PROPERTIES CXX_EXTENSIONS OFF)

    target_compile_definitions(admin_console_tests
        PRIVATE
            ADMIN_WEB_ASSET_DIR="${CMAKE_SOURCE_DIR}/admin_console/web"
    )

    target_link_libraries(admin_console_tests
        PRIVATE
            admin_console_core
            GTest::gtest_main
            nlohmann_json::nlohmann_json
    )

    if (NOT SERVER_USE_HEADER_ONLY_BOOST_SYSTEM)
        target_link_libraries(admin_console_tests PRIVATE Boost::system)
    endif()

    if (WIN32)
        target_link_libraries(admin_console_tests PRIVATE ws2_32 mswsock)
    endif()

    discover_labeled_gtests(admin_console_tests "admin;integration")
endif()
