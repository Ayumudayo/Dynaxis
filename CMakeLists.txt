cmake_minimum_required(VERSION 3.20)
project(ServerProject LANGUAGES CXX)

option(SERVER_ENABLE_TLS "Enable TLS(Asio SSL)" OFF)
option(SERVER_ENABLE_PROFILING "Enable profiling markers" OFF)
option(SERVER_USE_HEADER_ONLY_BOOST_SYSTEM "Use header-only boost::system (no lib link)" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (MSVC)
    add_compile_options(/utf-8)
endif()

# write-behind worker (optional)
add_executable(wb_worker tools/wb_worker/main.cpp)
target_include_directories(wb_worker PRIVATE ${CMAKE_SOURCE_DIR}/core/include ${CMAKE_SOURCE_DIR}/server/include)
if (TARGET server_core)
  target_link_libraries(wb_worker PRIVATE server_core)
endif()
if (TARGET server_storage_pg)
  target_link_libraries(wb_worker PRIVATE server_storage_pg)
endif()
if (TARGET server_storage_redis)
  target_link_libraries(wb_worker PRIVATE server_storage_redis)
endif()

# Windows에서 Boost.Asio가 적절한 WinSock API를 사용하도록 타깃에 매크로 부여 예정

# Windows에 수동 설치된 Boost 힌트
if (WIN32 AND NOT DEFINED BOOST_ROOT AND NOT DEFINED ENV{BOOST_ROOT})
    set(BOOST_ROOT "C:/local/boost_1_89_0" CACHE PATH "Path to Boost root")
endif()

if (DEFINED ENV{BOOST_LIBRARYDIR})
    set(Boost_LIBRARY_DIRS $ENV{BOOST_LIBRARYDIR})
endif()

set(Boost_USE_STATIC_LIBS OFF CACHE BOOL "link with boost static libs")
set(Boost_USE_MULTITHREADED ON)
set(Boost_USE_STATIC_RUNTIME OFF)

if (SERVER_USE_HEADER_ONLY_BOOST_SYSTEM)
    find_package(Boost REQUIRED)
else()
    find_package(Boost QUIET COMPONENTS system)
    if (NOT Boost_FOUND)
        message(WARNING "Boost::system을 찾지 못했습니다. header-only 모드로 폴백합니다. (BOOST_ERROR_CODE_HEADER_ONLY)")
        set(SERVER_USE_HEADER_ONLY_BOOST_SYSTEM ON CACHE BOOL "Use header-only boost::system" FORCE)
        find_package(Boost REQUIRED)
    endif()
endif()

message(STATUS "Boost header-only-system: ${SERVER_USE_HEADER_ONLY_BOOST_SYSTEM}")
message(STATUS "Boost include: ${Boost_INCLUDE_DIRS}")
message(STATUS "Boost libs: ${Boost_LIBRARIES}")
message(STATUS "Boost lib dirs: ${Boost_LIBRARY_DIRS}")

# Python3가 있으면 opcode 헤더를 빌드 시 자동 생성
find_package(Python3 COMPONENTS Interpreter QUIET)
if (Python3_Interpreter_FOUND)
    set(OPCODES_JSON ${CMAKE_SOURCE_DIR}/protocol/opcodes.json)
    set(GENERATED_HEADER ${CMAKE_SOURCE_DIR}/core/include/server/core/protocol/opcodes.hpp)
    add_custom_command(
        OUTPUT ${GENERATED_HEADER}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_opcodes.py ${OPCODES_JSON} ${GENERATED_HEADER}
        DEPENDS ${OPCODES_JSON} ${CMAKE_SOURCE_DIR}/tools/gen_opcodes.py
        COMMENT "Generating protocol opcodes header"
        VERBATIM)
    add_custom_target(generate_opcodes DEPENDS ${GENERATED_HEADER})
    # wire codec header generation
    set(WIRE_MAP_JSON ${CMAKE_SOURCE_DIR}/protocol/wire_map.json)
    set(GENERATED_WIRE_CODEC ${CMAKE_SOURCE_DIR}/core/include/server/wire/codec.hpp)
    add_custom_command(
        OUTPUT ${GENERATED_WIRE_CODEC}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_wire_codec.py ${WIRE_MAP_JSON} ${GENERATED_WIRE_CODEC}
        DEPENDS ${WIRE_MAP_JSON} ${CMAKE_SOURCE_DIR}/tools/gen_wire_codec.py ${CMAKE_SOURCE_DIR}/proto/wire.proto
        COMMENT "Generating wire codec header"
        VERBATIM)
    add_custom_target(generate_wire_codec DEPENDS ${GENERATED_WIRE_CODEC})
else()
    message(WARNING "Python3 not found: opcode 자동 생성 비활성화. 기존 헤더를 사용합니다.")
endif()

add_subdirectory(core)
add_subdirectory(proto)
add_subdirectory(server)
add_subdirectory(devclient)
# add_subdirectory(devclient)  # 이후 단계
# add_subdirectory(tests)      # 이후 단계
# migrations runner (requires libpqxx)
add_executable(migrations_runner tools/migrations/runner.cpp)
target_include_directories(migrations_runner PRIVATE ${CMAKE_SOURCE_DIR}/core/include)
find_package(libpqxx CONFIG QUIET)
if (libpqxx_FOUND AND TARGET libpqxx::pqxx)
  target_link_libraries(migrations_runner PRIVATE libpqxx::pqxx)
elseif (NOT libpqxx_FOUND)
  find_package(unofficial-libpqxx CONFIG QUIET)
  if (unofficial-libpqxx_FOUND AND TARGET unofficial::libpqxx::libpqxx)
    target_link_libraries(migrations_runner PRIVATE unofficial::libpqxx::libpqxx)
  else()
    message(WARNING "libpqxx not found — migrations_runner will fail to link")
  endif()
endif()
