add_library(server_core
    src/app/app_host.cpp
    src/app/engine_builder.cpp
    src/app/engine_context.cpp
    src/app/engine_runtime.cpp
    src/realtime/runtime.cpp
    src/net/acceptor.cpp
    src/net/session.cpp
    src/net/dispatcher.cpp
    src/net/hive.cpp
    src/net/connection.cpp
    src/net/listener.cpp
    src/net/transport_router.cpp
    src/net/rudp/ack_window.cpp
    src/net/rudp/retransmission_queue.cpp
    src/net/rudp/rudp_engine.cpp
    src/util/log.cpp
    src/trace/context.cpp
    src/metrics/metrics.cpp
    src/metrics/http_server.cpp
    src/runtime_metrics.cpp
    src/concurrent/job_queue.cpp
    src/concurrent/thread_manager.cpp
    src/concurrent/task_scheduler.cpp
    src/memory/memory_pool.cpp
    src/util/service_registry.cpp
    src/util/paths.cpp
    src/util/crash_handler.cpp
    src/state/instance_registry.cpp
    src/state/redis_instance_registry.cpp
    src/state/redis_backend_factory.cpp
    src/storage/db_worker_pool.cpp
    src/security/cipher.cpp
    src/security/admin_command_auth.cpp
    src/compression/compressor.cpp
    src/plugin/shared_library.cpp
    src/scripting/lua_sandbox.cpp
    src/scripting/script_watcher.cpp
    src/scripting/lua_runtime.cpp
)
add_library(server_core::server_core ALIAS server_core)

configure_luajit_submodule(LUAJIT_VENDOR_TARGET)
configure_sol2_submodule(SOL2_VENDOR_TARGET)
include(GNUInstallDirs)

# -----------------------------------------------------------------------------
# Build metadata (git hash/describe + build time)
#
# - Exposed to C++ via `server/core/build_info.hpp`.
# - Kept as compile definitions to avoid generated files.
# - Fallbacks to "unknown" when git is unavailable (eg. source archive builds).
# -----------------------------------------------------------------------------
find_package(Git QUIET)

set(BUILD_GIT_HASH "unknown")
set(BUILD_GIT_DESCRIBE "unknown")
if (GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _build_git_hash_res
        OUTPUT_VARIABLE _build_git_hash_out
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if (_build_git_hash_res EQUAL 0 AND NOT _build_git_hash_out STREQUAL "")
        set(BUILD_GIT_HASH "${_build_git_hash_out}")
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --always --dirty
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _build_git_desc_res
        OUTPUT_VARIABLE _build_git_desc_out
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if (_build_git_desc_res EQUAL 0 AND NOT _build_git_desc_out STREQUAL "")
        set(BUILD_GIT_DESCRIBE "${_build_git_desc_out}")
    endif()
endif()

string(TIMESTAMP BUILD_TIME_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)

target_compile_definitions(server_core PUBLIC
    BUILD_GIT_HASH="${BUILD_GIT_HASH}"
    BUILD_GIT_DESCRIBE="${BUILD_GIT_DESCRIBE}"
    BUILD_TIME_UTC="${BUILD_TIME_UTC}"
)

find_package(OpenSSL REQUIRED)
find_package(lz4 CONFIG QUIET)
if (NOT TARGET lz4::lz4)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LZ4 REQUIRED liblz4)
    add_library(lz4::lz4 INTERFACE IMPORTED)
    target_include_directories(lz4::lz4 INTERFACE ${LZ4_INCLUDE_DIRS})
    target_link_libraries(lz4::lz4 INTERFACE ${LZ4_LIBRARIES})
endif()

target_link_libraries(server_core PUBLIC OpenSSL::SSL OpenSSL::Crypto lz4::lz4)

if (LUAJIT_VENDOR_TARGET)
    get_target_property(LUAJIT_VENDOR_LOCATION ${LUAJIT_VENDOR_TARGET} IMPORTED_LOCATION)
    get_filename_component(LUAJIT_VENDOR_INSTALL_NAME "${LUAJIT_VENDOR_LOCATION}" NAME)

    target_link_libraries(server_core PUBLIC
        $<BUILD_INTERFACE:${LUAJIT_VENDOR_TARGET}>
        $<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_LIBDIR}/${LUAJIT_VENDOR_INSTALL_NAME}>
    )

    if (UNIX AND NOT APPLE)
        target_link_libraries(server_core PUBLIC $<INSTALL_INTERFACE:m> $<INSTALL_INTERFACE:dl>)
    endif()
endif()

if (SOL2_VENDOR_TARGET)
    target_link_libraries(server_core PRIVATE $<BUILD_INTERFACE:${SOL2_VENDOR_TARGET}>)
endif()

target_include_directories(server_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if (SERVER_USE_HEADER_ONLY_BOOST_SYSTEM)
    # 헤더온리 boost::system 사용: 라이브러리 링크 생략, 매크로 정의 필요
    target_compile_definitions(server_core PUBLIC BOOST_ERROR_CODE_HEADER_ONLY)
    target_include_directories(server_core PUBLIC ${Boost_INCLUDE_DIRS})
else()
    target_link_libraries(server_core PUBLIC Boost::system)
endif()

if (WIN32)
    # Asio의 WinSock 종속성
    target_link_libraries(server_core PUBLIC ws2_32 mswsock dbghelp)
endif()

if (MSVC)
    # Public headers contain UTF-8 comments and exported consumers do not inherit
    # the repo-level root add_compile_options(/utf-8). Propagate the charset
    # requirement through the package target so installed consumers compile the
    # headers consistently.
    target_compile_options(server_core PUBLIC /utf-8)
    target_compile_definitions(server_core PUBLIC _WIN32_WINNT=0x0602)
endif()

# opcode 헤더 생성에 의존 (가능한 경우)
if (TARGET generate_opcodes)
    add_dependencies(server_core generate_opcodes)
endif()
if (TARGET generate_wire_codec)
    add_dependencies(server_core generate_wire_codec)
endif()

include(CMakePackageConfigHelpers)

set(SERVER_CORE_PACKAGE_VERSION "1.0.0")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/server/core/api/version.hpp" _server_core_api_version_hpp)
string(REGEX MATCH "version_string\\(\\) noexcept \\{ return \"([0-9]+\\.[0-9]+\\.[0-9]+)\"; \\}" _server_core_api_version_match "${_server_core_api_version_hpp}")
if (CMAKE_MATCH_1)
    set(SERVER_CORE_PACKAGE_VERSION "${CMAKE_MATCH_1}")
endif()

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/server_coreConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/server_coreConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/server_core"
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/server_coreConfigVersion.cmake"
    VERSION "${SERVER_CORE_PACKAGE_VERSION}"
    COMPATIBILITY SameMajorVersion
)

install(TARGETS server_core
        EXPORT server_coreTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

install(EXPORT server_coreTargets
        FILE server_coreTargets.cmake
        NAMESPACE server_core::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/server_core")

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/server_coreConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/server_coreConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/server_core")

if (LUAJIT_VENDOR_TARGET)
    install(FILES
            "${LUAJIT_VENDOR_LOCATION}"
            DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endif()

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        FILES_MATCHING PATTERN "*.hpp")
