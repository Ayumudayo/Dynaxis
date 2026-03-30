add_library(server_app_config STATIC
    src/app/config.cpp
)
target_include_directories(server_app_config PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(server_app_config PUBLIC server_core)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(SERVER_FACTORY_PACKAGE_VERSION "1.0.0")
file(READ "${CMAKE_SOURCE_DIR}/core/include/server/core/api/version.hpp" _server_core_api_version_hpp)
string(REGEX MATCH "version_string\\(\\) noexcept \\{ return \"([0-9]+\\.[0-9]+\\.[0-9]+)\"; \\}"
       _server_core_api_version_match
       "${_server_core_api_version_hpp}")
if (CMAKE_MATCH_1)
    set(SERVER_FACTORY_PACKAGE_VERSION "${CMAKE_MATCH_1}")
endif()

add_library(server_app_backends STATIC
    src/app/core_internal_adapter.cpp
)
target_include_directories(server_app_backends PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(server_app_backends PUBLIC server_core)
target_link_libraries(server_app_backends PRIVATE server_storage_pg_factory infra_redis_factory)

add_library(server_chat_runtime STATIC
    src/chat/chat_service_core.cpp
    src/chat/chat_hook_plugin_manager.cpp
    src/chat/chat_hook_plugin_chain.cpp
    src/scripting/chat_lua_bindings.cpp
    src/chat/handlers_login.cpp
    src/chat/handlers_join.cpp
    src/chat/handlers_leave.cpp
    src/chat/handlers_chat.cpp
    src/chat/handlers_ping.cpp
    src/chat/session_events.cpp
)
target_include_directories(server_chat_runtime PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_include_directories(server_chat_runtime PRIVATE ${CMAKE_BINARY_DIR}/proto)
target_link_libraries(server_chat_runtime PUBLIC server_core wire_proto)
target_link_libraries(server_chat_runtime PRIVATE ${CMAKE_DL_LIBS})
if (TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(server_chat_runtime PRIVATE nlohmann_json::nlohmann_json)
endif()

add_library(server_fps_runtime STATIC
    src/fps/fps_service.cpp
)
target_include_directories(server_fps_runtime PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_include_directories(server_fps_runtime PRIVATE ${CMAKE_BINARY_DIR}/proto)
target_link_libraries(server_fps_runtime PUBLIC server_core wire_proto)

add_executable(server_app
    src/main.cpp
    src/app/bootstrap.cpp
    src/app/metrics_server.cpp
    src/app/server_runtime_state.cpp
    src/app/router.cpp
)
target_include_directories(server_app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(server_app PRIVATE server_app_config server_app_backends server_chat_runtime server_fps_runtime server_core wire_proto)
# dlopen/dlsym (Linux)
target_link_libraries(server_app PRIVATE ${CMAKE_DL_LIBS})
if (TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(server_app PRIVATE nlohmann_json::nlohmann_json)
endif()
# protobuf 생성 헤더 경로 추가
target_include_directories(server_app PRIVATE ${CMAKE_BINARY_DIR}/proto)

# Runtime DLL 자동 복사(Windows)
if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.21")
    add_custom_command(TARGET server_app POST_BUILD
        COMMAND ${CMAKE_COMMAND}
        -Druntime_dlls=$<TARGET_RUNTIME_DLLS:server_app>
        -Dtarget_dir=$<TARGET_FILE_DIR:server_app>
        -P ${CMAKE_SOURCE_DIR}/cmake/copy_runtime_dlls_if_any.cmake
        COMMAND_EXPAND_LISTS)
endif()

# Postgres storage adapter
add_library(server_storage_pg_objects OBJECT
    src/storage/postgres/connection_pool.cpp
)
target_include_directories(server_storage_pg_objects PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(server_storage_pg_objects PUBLIC server_core::server_core)

add_library(server_storage_pg_factory STATIC
    src/storage/postgres/connection_pool_factory.cpp
    $<TARGET_OBJECTS:server_storage_pg_objects>
)
add_library(server_storage_pg_factory::server_storage_pg_factory ALIAS server_storage_pg_factory)
target_include_directories(server_storage_pg_factory PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(server_storage_pg_factory PUBLIC server_core::server_core)

# Compatibility umbrella for broader legacy link points.
add_library(server_storage_pg INTERFACE)
target_link_libraries(server_storage_pg INTERFACE server_storage_pg_factory)

# libpqxx 필수: provider가 libpqxx::pqxx 또는 unofficial::libpqxx::libpqxx를 제공해야 함
find_package(libpqxx CONFIG QUIET)
if (NOT TARGET libpqxx::pqxx)
    pkg_check_modules(libpqxx QUIET IMPORTED_TARGET libpqxx)
    if (libpqxx_FOUND)
        add_library(libpqxx::pqxx INTERFACE IMPORTED)
        target_link_libraries(libpqxx::pqxx INTERFACE PkgConfig::libpqxx)
    else()
        find_package(unofficial-libpqxx CONFIG QUIET)
        if (unofficial-libpqxx_FOUND AND TARGET unofficial::libpqxx::libpqxx)
            add_library(libpqxx::pqxx INTERFACE IMPORTED)
            target_link_libraries(libpqxx::pqxx INTERFACE unofficial::libpqxx::libpqxx)
        else()
            message(FATAL_ERROR "libpqxx not found — PostgreSQL storage requires libpqxx")
        endif()
    endif()
endif()
target_link_libraries(server_storage_pg_objects PUBLIC libpqxx::pqxx)
target_link_libraries(server_storage_pg_factory PUBLIC libpqxx::pqxx)

add_library(server_state_common STATIC
    src/state/instance_registry_common.cpp
)
target_include_directories(server_state_common PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(server_state_common PUBLIC server_core)

add_library(server_state_redis STATIC
    src/state/instance_registry_redis.cpp
)
target_include_directories(server_state_redis PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(server_state_redis PUBLIC server_core server_state_common)

add_library(server_state_redis_factory STATIC
    src/state/redis_backend_factory.cpp
)
target_include_directories(server_state_redis_factory PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(server_state_redis_factory PUBLIC server_core)
target_link_libraries(server_state_redis_factory PRIVATE server_state_redis)

add_library(server_state_consul STATIC
    src/state/instance_registry_consul.cpp
)
target_include_directories(server_state_consul PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(server_state_consul PUBLIC server_state_common)

configure_package_config_file(
    "${CMAKE_CURRENT_LIST_DIR}/cmake/server_storage_pg_factoryConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/server_storage_pg_factoryConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/server_storage_pg_factory"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/server_storage_pg_factoryConfigVersion.cmake"
    VERSION "${SERVER_FACTORY_PACKAGE_VERSION}"
    COMPATIBILITY SameMajorVersion
)
install(TARGETS server_storage_pg_factory
        EXPORT server_storage_pg_factoryTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(EXPORT server_storage_pg_factoryTargets
        FILE server_storage_pg_factoryTargets.cmake
        NAMESPACE server_storage_pg_factory::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/server_storage_pg_factory")
install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/server_storage_pg_factoryConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/server_storage_pg_factoryConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/server_storage_pg_factory")
install(FILES
        "${CMAKE_CURRENT_LIST_DIR}/include/server/storage/postgres/connection_pool.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/server/storage/postgres")
install(FILES
        "${CMAKE_CURRENT_LIST_DIR}/include/server/storage/connection_pool.hpp"
        "${CMAKE_CURRENT_LIST_DIR}/include/server/storage/unit_of_work.hpp"
        "${CMAKE_CURRENT_LIST_DIR}/include/server/storage/repositories.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/server/storage")

add_subdirectory(plugins)
