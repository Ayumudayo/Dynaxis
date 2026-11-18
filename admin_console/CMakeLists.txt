cmake_minimum_required(VERSION 3.20)

project(dynaxis_admin_console)

if (NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json CONFIG REQUIRED)
endif()

set(ADMIN_PQXX_TARGET "")
find_package(libpqxx CONFIG QUIET)
if (TARGET libpqxx::pqxx)
    set(ADMIN_PQXX_TARGET libpqxx::pqxx)
else()
    find_package(unofficial-libpqxx CONFIG QUIET)
    if (TARGET unofficial::libpqxx::libpqxx)
        set(ADMIN_PQXX_TARGET unofficial::libpqxx::libpqxx)
    else()
        find_package(PkgConfig QUIET)
        if (PkgConfig_FOUND)
            pkg_check_modules(libpqxx QUIET IMPORTED_TARGET libpqxx)
        endif()
        if (TARGET PkgConfig::libpqxx)
            set(ADMIN_PQXX_TARGET PkgConfig::libpqxx)
        else()
            find_package(libpqxx QUIET)
            if (TARGET libpqxx::pqxx)
                set(ADMIN_PQXX_TARGET libpqxx::pqxx)
            endif()
        endif()
    endif()
endif()

if (NOT ADMIN_PQXX_TARGET)
    message(FATAL_ERROR "libpqxx target not found for admin_console")
endif()

add_library(admin_console_core STATIC
    src/prometheus_parser.cpp
    src/bounded_http_probe.cpp
    src/resp_probe.cpp
    src/postgres_probe.cpp
    src/service_catalog.cpp
    src/inventory_poller.cpp
    src/admin_http_server.cpp
    src/admin_router.cpp
)

target_include_directories(admin_console_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(admin_console_core PUBLIC cxx_std_20)
set_target_properties(admin_console_core PROPERTIES CXX_EXTENSIONS OFF)

target_link_libraries(admin_console_core
    PUBLIC
        server_core
    PRIVATE
        ${ADMIN_PQXX_TARGET}
        nlohmann_json::nlohmann_json
)

if (NOT SERVER_USE_HEADER_ONLY_BOOST_SYSTEM)
    target_link_libraries(admin_console_core PRIVATE Boost::system)
endif()

if (WIN32)
    target_link_libraries(admin_console_core PUBLIC ws2_32 mswsock)
endif()

add_executable(admin_app
    src/main.cpp
)

target_compile_features(admin_app PRIVATE cxx_std_20)
set_target_properties(admin_app PROPERTIES CXX_EXTENSIONS OFF)

target_link_libraries(admin_app
    PRIVATE
        admin_console_core
        server_core
)

if (NOT SERVER_USE_HEADER_ONLY_BOOST_SYSTEM)
    target_link_libraries(admin_app PRIVATE Boost::system)
endif()

if (WIN32)
    target_link_libraries(admin_app PRIVATE ws2_32 mswsock)
endif()
