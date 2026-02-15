add_library(server_core
    src/net/acceptor.cpp
    src/net/session.cpp
    src/net/dispatcher.cpp
    src/net/hive.cpp
    src/net/connection.cpp
    src/net/listener.cpp
    src/util/log.cpp
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
    src/storage/db_worker_pool.cpp
    src/security/cipher.cpp
    src/compression/compressor.cpp
)

# -----------------------------------------------------------------------------
# Build metadata (git hash/describe + build time)
#
# - Exposed to C++ via `server/core/build_info.hpp`.
# - Kept as compile definitions to avoid generated files.
# - Fallbacks to "unknown" when git is unavailable (eg. source archive builds).
# -----------------------------------------------------------------------------
find_package(Git QUIET)

set(KNIGHTS_GIT_HASH "unknown")
set(KNIGHTS_GIT_DESCRIBE "unknown")
if (GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _knights_git_hash_res
        OUTPUT_VARIABLE _knights_git_hash_out
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if (_knights_git_hash_res EQUAL 0 AND NOT _knights_git_hash_out STREQUAL "")
        set(KNIGHTS_GIT_HASH "${_knights_git_hash_out}")
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --always --dirty
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _knights_git_desc_res
        OUTPUT_VARIABLE _knights_git_desc_out
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if (_knights_git_desc_res EQUAL 0 AND NOT _knights_git_desc_out STREQUAL "")
        set(KNIGHTS_GIT_DESCRIBE "${_knights_git_desc_out}")
    endif()
endif()

string(TIMESTAMP KNIGHTS_BUILD_TIME_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)

target_compile_definitions(server_core PUBLIC
    KNIGHTS_GIT_HASH="${KNIGHTS_GIT_HASH}"
    KNIGHTS_GIT_DESCRIBE="${KNIGHTS_GIT_DESCRIBE}"
    KNIGHTS_BUILD_TIME_UTC="${KNIGHTS_BUILD_TIME_UTC}"
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

target_include_directories(server_core PUBLIC include)
# protobuf 생성 헤더 경로 추가
target_include_directories(server_core PUBLIC ${CMAKE_BINARY_DIR}/proto)

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
    target_compile_definitions(server_core PUBLIC _WIN32_WINNT=0x0602)
endif()

# opcode 헤더 생성에 의존 (가능한 경우)
if (TARGET generate_opcodes)
    add_dependencies(server_core generate_opcodes)
endif()
if (TARGET generate_wire_codec)
    add_dependencies(server_core generate_wire_codec)
endif()

# 설치 규칙: 다른 프로그램이 코어의 패킷 구조를 공유할 수 있도록 export
install(TARGETS server_core
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include
        FILES_MATCHING PATTERN "*.hpp")
