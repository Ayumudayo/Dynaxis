include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(INFRA_REDIS_PACKAGE_VERSION "1.0.0")
file(READ "${CMAKE_SOURCE_DIR}/core/include/server/core/api/version.hpp" _server_core_api_version_hpp)
string(REGEX MATCH "version_string\\(\\) noexcept \\{ return \"([0-9]+\\.[0-9]+\\.[0-9]+)\"; \\}"
       _server_core_api_version_match
       "${_server_core_api_version_hpp}")
if (CMAKE_MATCH_1)
    set(INFRA_REDIS_PACKAGE_VERSION "${CMAKE_MATCH_1}")
endif()

add_library(infra_redis_objects OBJECT
    src/storage/redis/client.cpp
)
target_include_directories(infra_redis_objects PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(infra_redis_objects PUBLIC server_core::server_core)

add_library(infra_redis_factory STATIC
    src/storage/redis/client_factory.cpp
    $<TARGET_OBJECTS:infra_redis_objects>
)
add_library(infra_redis_factory::infra_redis_factory ALIAS infra_redis_factory)
target_include_directories(infra_redis_factory PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(infra_redis_factory PUBLIC server_core::server_core)

# Legacy in-repo link target retained for a transition window.
add_library(server_storage_redis_factory INTERFACE)
add_library(server_storage_redis_factory::server_storage_redis_factory ALIAS server_storage_redis_factory)
target_link_libraries(server_storage_redis_factory INTERFACE infra_redis_factory)

add_library(server_storage_redis INTERFACE)
target_link_libraries(server_storage_redis INTERFACE infra_redis_factory)

find_package(redis++ CONFIG QUIET)
if (NOT TARGET redis++::redis++)
    if (TARGET redis++)
        add_library(redis++::redis++ INTERFACE IMPORTED)
        target_link_libraries(redis++::redis++ INTERFACE redis++)
    elseif (TARGET redis++_static::redis++_static)
        add_library(redis++::redis++ INTERFACE IMPORTED)
        target_link_libraries(redis++::redis++ INTERFACE redis++_static::redis++_static)
    elseif (TARGET redis++_static)
        add_library(redis++::redis++ INTERFACE IMPORTED)
        target_link_libraries(redis++::redis++ INTERFACE redis++_static)
    endif()
endif()
if (TARGET redis++::redis++)
    target_link_libraries(infra_redis_objects PUBLIC redis++::redis++)
    target_link_libraries(infra_redis_factory PUBLIC redis++::redis++)
    target_compile_definitions(infra_redis_factory PUBLIC HAVE_REDIS_PLUS_PLUS)
    target_compile_definitions(infra_redis_objects PUBLIC HAVE_REDIS_PLUS_PLUS)
elseif (TARGET redis++)
    target_link_libraries(infra_redis_objects PUBLIC redis++)
    target_link_libraries(infra_redis_factory PUBLIC redis++)
    target_compile_definitions(infra_redis_factory PUBLIC HAVE_REDIS_PLUS_PLUS)
    target_compile_definitions(infra_redis_objects PUBLIC HAVE_REDIS_PLUS_PLUS)
elseif (TARGET redis++_static::redis++_static)
    target_link_libraries(infra_redis_objects PUBLIC redis++_static::redis++_static)
    target_link_libraries(infra_redis_factory PUBLIC redis++_static::redis++_static)
    target_compile_definitions(infra_redis_factory PUBLIC HAVE_REDIS_PLUS_PLUS)
    target_compile_definitions(infra_redis_objects PUBLIC HAVE_REDIS_PLUS_PLUS)
elseif (TARGET redis++_static)
    target_link_libraries(infra_redis_objects PUBLIC redis++_static)
    target_link_libraries(infra_redis_factory PUBLIC redis++_static)
    target_compile_definitions(infra_redis_factory PUBLIC HAVE_REDIS_PLUS_PLUS)
    target_compile_definitions(infra_redis_objects PUBLIC HAVE_REDIS_PLUS_PLUS)
else()
    message(WARNING "redis-plus-plus not found ? Redis client stays as stub")
endif()

configure_package_config_file(
    "${CMAKE_CURRENT_LIST_DIR}/cmake/infra_redis_factoryConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/infra_redis_factoryConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/infra_redis_factory"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/infra_redis_factoryConfigVersion.cmake"
    VERSION "${INFRA_REDIS_PACKAGE_VERSION}"
    COMPATIBILITY SameMajorVersion
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/server_storage_redis_factoryConfigVersion.cmake"
    VERSION "${INFRA_REDIS_PACKAGE_VERSION}"
    COMPATIBILITY SameMajorVersion
)
configure_package_config_file(
    "${CMAKE_CURRENT_LIST_DIR}/cmake/server_storage_redis_factoryConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/server_storage_redis_factoryConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/server_storage_redis_factory"
)

install(TARGETS infra_redis_factory
        EXPORT infra_redis_factoryTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(EXPORT infra_redis_factoryTargets
        FILE infra_redis_factoryTargets.cmake
        NAMESPACE infra_redis_factory::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/infra_redis_factory")
install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/infra_redis_factoryConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/infra_redis_factoryConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/infra_redis_factory")
install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/server_storage_redis_factoryConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/server_storage_redis_factoryConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/server_storage_redis_factory")
install(FILES
        "${CMAKE_CURRENT_LIST_DIR}/include/server/storage/redis/factory.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/server/storage/redis")
