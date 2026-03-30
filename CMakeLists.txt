cmake_minimum_required(VERSION 3.20)
project(ServerProject LANGUAGES CXX)

option(SERVER_ENABLE_TLS "Enable TLS(Asio SSL)" OFF)
option(SERVER_ENABLE_PROFILING "Enable profiling markers" OFF)
option(SERVER_USE_HEADER_ONLY_BOOST_SYSTEM "Use header-only boost::system (no lib link)" OFF)
option(ENABLE_SANITIZERS "Enable sanitizers (ASan+UBSan)" OFF)
option(ENABLE_IPO "Enable interprocedural optimization (IPO/LTO)" OFF)
option(BUILD_SERVER_STACK "Build server libraries and server_app" ON)
option(BUILD_GATEWAY_APP "Build gateway_app" ON)


option(BUILD_SERVER_TESTS "Build server tests" ON)
option(BUILD_GTEST_TESTS "Build GTest-based unit tests" ON)
option(BUILD_CONTRACT_TESTS "Build non-GTest contract/governance checks" ON)
option(BUILD_WRITE_BEHIND_TOOLS "Build write-behind helper tools" ON)
option(BUILD_LOADGEN_TOOLS "Build unified load generator tools" ON)
option(BUILD_ADMIN_APP "Build admin management app" ON)
option(BUILD_MIGRATIONS_RUNNER "Build migrations runner tool" ON)
set(LUAJIT_SUBMODULE_DIR "${CMAKE_SOURCE_DIR}/external/luajit" CACHE PATH
    "Path to the upstream LuaJIT source submodule")
set(SOL2_SUBMODULE_DIR "${CMAKE_SOURCE_DIR}/external/sol2" CACHE PATH
    "Path to the sol2 header-only source submodule")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(luajit_submodule)
include(sol2_submodule)

if (NOT EXISTS "${LUAJIT_SUBMODULE_DIR}/src/lua.h")
    message(FATAL_ERROR
        "Lua capability requires upstream LuaJIT sources at: ${LUAJIT_SUBMODULE_DIR}. "
        "Run: git submodule update --init --recursive external/luajit")
endif()

file(STRINGS "${LUAJIT_SUBMODULE_DIR}/src/luajit_rolling.h" _LUAJIT_VERSION_LINE
     REGEX "^#define[ \t]+LUAJIT_VERSION[ \t]+")
if (NOT _LUAJIT_VERSION_LINE)
    message(FATAL_ERROR
        "Lua capability expects LuaJIT headers at: ${LUAJIT_SUBMODULE_DIR}. "
        "Detected Lua headers without LUAJIT_VERSION marker.")
endif()

if (NOT EXISTS "${SOL2_SUBMODULE_DIR}/include/sol/sol.hpp")
    message(FATAL_ERROR
        "Lua capability requires sol2 headers at: ${SOL2_SUBMODULE_DIR}. "
        "Run: git submodule update --init --recursive external/sol2")
endif()



if (MSVC)
    add_compile_options(/utf-8 /MP)
endif()

if (ENABLE_SANITIZERS)
    message(STATUS "Sanitizers enabled (ASan+UBSan where supported)")

    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    elseif (MSVC)
        # NOTE: MSVC ASan support depends on toolchain/runtime; keep opt-in.
        add_compile_options(/fsanitize=address)
        add_link_options(/INCREMENTAL:NO)
    else()
        message(WARNING "ENABLE_SANITIZERS=ON but unsupported compiler: ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

if (ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_OUTPUT)
    if (IPO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
        message(STATUS "IPO/LTO enabled (ENABLE_IPO=ON)")
    else()
        message(WARNING "ENABLE_IPO=ON but IPO is not supported: ${IPO_OUTPUT}")
    endif()
endif()

if (BUILD_SERVER_TESTS)
    if (NOT BUILD_SERVER_STACK)
        message(FATAL_ERROR "BUILD_SERVER_TESTS=ON requires BUILD_SERVER_STACK=ON")
    endif()
    if (NOT BUILD_GATEWAY_APP)
        message(FATAL_ERROR "BUILD_SERVER_TESTS=ON requires BUILD_GATEWAY_APP=ON")
    endif()
endif()


# Windows에서 Boost.Asio가 적절한 WinSock API를 사용하도록 타깃에 매크로 부여 예정

set(Boost_USE_STATIC_LIBS OFF CACHE BOOL "link with boost static libs")
set(Boost_USE_MULTITHREADED ON)
set(Boost_USE_STATIC_RUNTIME OFF)

if (SERVER_USE_HEADER_ONLY_BOOST_SYSTEM)
    find_package(Boost REQUIRED)
else()
    find_package(Boost QUIET COMPONENTS system)
    if (NOT Boost_FOUND OR NOT TARGET Boost::system)
        message(WARNING "Boost::system을 찾지 못했습니다. header-only 모드로 폴백합니다. (BOOST_ERROR_CODE_HEADER_ONLY)")
        set(SERVER_USE_HEADER_ONLY_BOOST_SYSTEM ON CACHE BOOL "Use header-only boost::system" FORCE)
        find_package(Boost REQUIRED)
    endif()
endif()

message(STATUS "Boost header-only-system: ${SERVER_USE_HEADER_ONLY_BOOST_SYSTEM}")
message(STATUS "Boost include: ${Boost_INCLUDE_DIRS}")
message(STATUS "Boost libs: ${Boost_LIBRARIES}")
message(STATUS "Boost lib dirs: ${Boost_LIBRARY_DIRS}")

find_package(nlohmann_json CONFIG QUIET)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(DYNAXIS_GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated/include")
set(DYNAXIS_SYSTEM_OPCODES_GENERATED_HEADER
    "${DYNAXIS_GENERATED_INCLUDE_DIR}/server/generated/core/protocol/system_opcodes.generated.hpp")
set(DYNAXIS_GAME_OPCODES_GENERATED_HEADER
    "${DYNAXIS_GENERATED_INCLUDE_DIR}/server/generated/protocol/game_opcodes.generated.hpp")
set(DYNAXIS_WIRE_CODEC_GENERATED_HEADER
    "${DYNAXIS_GENERATED_INCLUDE_DIR}/server/generated/wire/codec.generated.hpp")

set(OPCODES_JSON "${CMAKE_SOURCE_DIR}/core/protocol/system_opcodes.json")
set(GAME_OPCODES_JSON "${CMAKE_SOURCE_DIR}/server/protocol/game_opcodes.json")
set(WIRE_MAP_JSON "${CMAKE_SOURCE_DIR}/protocol/wire_map.json")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${OPCODES_JSON}"
    "${GAME_OPCODES_JSON}"
    "${WIRE_MAP_JSON}"
    "${CMAKE_SOURCE_DIR}/tools/gen_opcodes.py"
    "${CMAKE_SOURCE_DIR}/tools/gen_wire_codec.py"
    "${CMAKE_SOURCE_DIR}/proto/wire.proto")

function(dynaxis_generate_header description script input output)
    get_filename_component(_out_dir "${output}" DIRECTORY)
    file(MAKE_DIRECTORY "${_out_dir}")

    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${script}" "${input}" "${output}" ${ARGN}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _generate_result
        OUTPUT_VARIABLE _generate_stdout
        ERROR_VARIABLE _generate_stderr
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )

    if (NOT _generate_result EQUAL 0)
        string(STRIP "${_generate_stdout}" _generate_stdout)
        string(STRIP "${_generate_stderr}" _generate_stderr)
        message(FATAL_ERROR
            "${description} failed with exit code ${_generate_result}\n"
            "stdout:\n${_generate_stdout}\n"
            "stderr:\n${_generate_stderr}")
    endif()
endfunction()

dynaxis_generate_header(
    "Generating protocol system opcodes header"
    "${CMAKE_SOURCE_DIR}/tools/gen_opcodes.py"
    "${OPCODES_JSON}"
    "${DYNAXIS_SYSTEM_OPCODES_GENERATED_HEADER}")
dynaxis_generate_header(
    "Generating protocol game opcodes header"
    "${CMAKE_SOURCE_DIR}/tools/gen_opcodes.py"
    "${GAME_OPCODES_JSON}"
    "${DYNAXIS_GAME_OPCODES_GENERATED_HEADER}")
dynaxis_generate_header(
    "Generating wire codec header"
    "${CMAKE_SOURCE_DIR}/tools/gen_wire_codec.py"
    "${WIRE_MAP_JSON}"
    "${DYNAXIS_WIRE_CODEC_GENERATED_HEADER}")

add_subdirectory(proto)
add_subdirectory(core)
if (BUILD_SERVER_STACK OR BUILD_GATEWAY_APP OR BUILD_WRITE_BEHIND_TOOLS OR BUILD_ADMIN_APP)
  add_subdirectory(infra)
endif()
if (BUILD_SERVER_STACK)
  add_subdirectory(server)
endif()
if (BUILD_GATEWAY_APP)
  add_subdirectory(gateway)
endif()


if (BUILD_SERVER_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if (BUILD_WRITE_BEHIND_TOOLS OR BUILD_LOADGEN_TOOLS OR BUILD_ADMIN_APP OR BUILD_MIGRATIONS_RUNNER)
  add_subdirectory(tools)
endif()

if (WIN32)
    add_subdirectory(client_gui)
endif()
